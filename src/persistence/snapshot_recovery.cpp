#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "atlaslob/persistence/configuration_digest.hpp"
#include "atlaslob/persistence/snapshot_store.hpp"
#include "command_log_codec.hpp"
#include "log_io.hpp"
#include "log_scanner.hpp"
#include "multi_instrument_engine_access.hpp"
#include "replay_internal.hpp"
#include "snapshot_codec.hpp"

namespace atlaslob::persistence {
namespace {

struct LogBoundary final {
  domain::Sequence sequence{};
  std::uint64_t end_offset{};
};

struct SnapshotCandidate final {
  std::filesystem::path path;
  domain::Sequence filename_sequence{};
  SnapshotError discovery_error{};
};

struct SelectedSnapshot final {
  std::filesystem::path path;
  SnapshotFile file;
  std::unique_ptr<MultiInstrumentEngine> engine;
  RecoverySource source{RecoverySource::full_log};
};

[[nodiscard]] LogError log_io_error(std::uint64_t offset, std::error_code error) noexcept {
  return {
      .category = LogErrorCategory::io_failure,
      .byte_offset = offset,
      .system_error = error,
  };
}

[[nodiscard]] SnapshotError snapshot_error(SnapshotErrorCategory category,
                                           std::uint64_t offset = 0U,
                                           std::error_code error = {}) noexcept {
  return {
      .category = category,
      .byte_offset = offset,
      .system_error = error,
  };
}

class BoundaryVisitor final : public detail::LogScanVisitor {
 public:
  explicit BoundaryVisitor(std::vector<domain::Sequence> targets = {})
      : targets_{std::move(targets)} {
    std::sort(targets_.begin(), targets_.end());
    targets_.erase(std::unique(targets_.begin(), targets_.end()), targets_.end());
    boundaries_.reserve(targets_.size());
  }

  void on_record(const CommandRecord& record, std::span<const std::uint8_t> encoded,
                 std::uint64_t frame_begin, std::uint64_t frame_end) override {
    static_cast<void>(encoded);
    static_cast<void>(frame_begin);
    while (next_target_ < targets_.size() && targets_[next_target_] < record.sequence) {
      ++next_target_;
    }
    if (next_target_ < targets_.size() && targets_[next_target_] == record.sequence) {
      boundaries_.push_back({
          .sequence = record.sequence,
          .end_offset = frame_end,
      });
      ++next_target_;
    }
    if (record.outcome == RecordOutcome::committed) {
      ++committed_;
    } else {
      ++rejected_;
    }
  }

  [[nodiscard]] const std::vector<LogBoundary>& boundaries() const noexcept { return boundaries_; }
  [[nodiscard]] std::uint64_t committed() const noexcept { return committed_; }
  [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }

 private:
  std::vector<domain::Sequence> targets_;
  std::vector<LogBoundary> boundaries_;
  std::size_t next_target_{};
  std::uint64_t committed_{};
  std::uint64_t rejected_{};
};

void initialize_replay_report(ReplayReport& report, ReplayOptions options,
                              const detail::LogScanResult& scan) {
  report.mode = options.mode;
  report.tail_policy = options.tail_policy;
  report.tail = detail::public_replay_tail(scan.termination);
  report.header = scan.header;
  report.last_sequence = scan.last_sequence;
  report.valid_end_offset = scan.valid_end_offset;
  report.records_scanned = scan.record_count;
  report.used_valid_prefix = scan.termination == detail::LogScanTermination::truncated_tail &&
                             options.tail_policy == TailPolicy::valid_prefix;
  if (report.used_valid_prefix) {
    report.warning = scan.error;
  }
}

[[nodiscard]] bool scan_usable(const detail::LogScanResult& scan, ReplayOptions options,
                               ReplayReport& report) {
  if (scan.termination == detail::LogScanTermination::io_failure ||
      scan.termination == detail::LogScanTermination::corruption) {
    report.error = scan.error;
    return false;
  }
  if (scan.termination == detail::LogScanTermination::truncated_tail &&
      options.tail_policy == TailPolicy::strict) {
    report.error = scan.error;
    return false;
  }
  if (!scan.header.has_value()) {
    report.error = {
        .category = LogErrorCategory::invalid_length,
        .byte_offset = 0U,
    };
    return false;
  }
  return true;
}

[[nodiscard]] std::optional<std::uint64_t> boundary_for(
    domain::Sequence sequence, const detail::LogScanResult& scan,
    const std::vector<LogBoundary>& boundaries) noexcept {
  if (sequence.value() == 0U) {
    return scan.header_end_offset;
  }
  const auto position = std::lower_bound(boundaries.begin(), boundaries.end(), sequence,
                                         [](const LogBoundary& boundary, domain::Sequence value) {
                                           return boundary.sequence < value;
                                         });
  if (position == boundaries.end() || position->sequence != sequence) {
    return std::nullopt;
  }
  return position->end_offset;
}

[[nodiscard]] SnapshotError compatible_snapshot(const SnapshotFile& snapshot,
                                                const LogHeader& header,
                                                const detail::LogScanResult& scan,
                                                const std::vector<LogBoundary>& boundaries) {
  if (snapshot.log_id != header.log_id) {
    return snapshot_error(SnapshotErrorCategory::log_id_mismatch);
  }
  if (snapshot.semantics_version != header.semantics_version ||
      snapshot.catalog != header.catalog || snapshot.engine_config != header.engine_config ||
      snapshot.configuration_digest !=
          configuration_digest(header.catalog, header.engine_config, header.semantics_version)) {
    return snapshot_error(SnapshotErrorCategory::configuration_digest_mismatch);
  }
  const auto expected_boundary = boundary_for(snapshot.covered_sequence, scan, boundaries);
  if (!expected_boundary.has_value()) {
    return snapshot_error(SnapshotErrorCategory::sequence_mismatch,
                          snapshot.covered_log_byte_offset);
  }
  if (*expected_boundary != snapshot.covered_log_byte_offset ||
      snapshot.covered_log_byte_offset > scan.valid_end_offset) {
    return snapshot_error(SnapshotErrorCategory::log_boundary_mismatch,
                          snapshot.covered_log_byte_offset);
  }
  if (snapshot.sequence_exhausted && snapshot.covered_log_byte_offset != scan.valid_end_offset) {
    return snapshot_error(SnapshotErrorCategory::sequence_mismatch,
                          snapshot.covered_log_byte_offset);
  }
  return {};
}

[[nodiscard]] std::optional<domain::Sequence> parse_snapshot_filename(std::string_view filename,
                                                                      LogId log_id) {
  const std::string prefix = "atlaslob-" + log_id.hex() + "-";
  constexpr std::string_view suffix{".snapshot"};
  if (!filename.starts_with(prefix) || !filename.ends_with(suffix)) {
    return std::nullopt;
  }
  const auto digits =
      filename.substr(prefix.size(), filename.size() - prefix.size() - suffix.size());
  if (digits.size() != 20U) {
    return std::nullopt;
  }
  std::uint64_t value{};
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (error != std::errc{} || end != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return domain::Sequence{value};
}

[[nodiscard]] std::optional<std::string> ascii_filename(const std::filesystem::path& path) {
  const auto native = path.filename().native();
  using NativeChar = std::filesystem::path::string_type::value_type;
  using UnsignedNativeChar = std::make_unsigned_t<NativeChar>;

  std::string ascii;
  ascii.reserve(native.size());
  for (const auto code_unit : native) {
    const auto value = static_cast<UnsignedNativeChar>(code_unit);
    if (value > static_cast<UnsignedNativeChar>(0x7fU)) {
      return std::nullopt;
    }
    ascii.push_back(static_cast<char>(value));
  }
  return ascii;
}

struct CandidateList final {
  std::vector<SnapshotCandidate> candidates;
  SnapshotError error{};
};

[[nodiscard]] CandidateList list_candidates(const std::filesystem::path& directory, LogId log_id) {
  std::error_code iterator_error;
  std::filesystem::directory_iterator iterator{directory, iterator_error};
  if (iterator_error) {
    return {
        .candidates = {},
        .error = snapshot_error(SnapshotErrorCategory::io_failure, 0U, iterator_error),
    };
  }

  std::vector<SnapshotCandidate> candidates;
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    const auto& entry = *iterator;
    if (const auto filename = ascii_filename(entry.path()); filename.has_value()) {
      if (const auto sequence = parse_snapshot_filename(*filename, log_id); sequence.has_value()) {
        std::error_code type_error;
        const auto status = entry.symlink_status(type_error);
        const bool regular = !type_error && std::filesystem::is_regular_file(status);
        candidates.push_back({
            .path = entry.path(),
            .filename_sequence = *sequence,
            .discovery_error =
                type_error
                    ? snapshot_error(SnapshotErrorCategory::io_failure, 0U, type_error)
                    : (regular ? SnapshotError{}
                               : snapshot_error(SnapshotErrorCategory::io_failure, 0U,
                                                std::make_error_code(std::errc::invalid_argument))),
        });
      }
    }
    iterator.increment(iterator_error);
    if (iterator_error) {
      return {
          .candidates = {},
          .error = snapshot_error(SnapshotErrorCategory::io_failure, 0U, iterator_error),
      };
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const SnapshotCandidate& left, const SnapshotCandidate& right) {
              if (left.filename_sequence != right.filename_sequence) {
                return left.filename_sequence > right.filename_sequence;
              }
              return left.path.filename().native() < right.path.filename().native();
            });
  return {
      .candidates = std::move(candidates),
      .error = {},
  };
}

[[nodiscard]] SnapshotError restore_candidate(const SnapshotFile& file,
                                              std::unique_ptr<MultiInstrumentEngine>& engine) {
  auto host = detail::host_engine_snapshot(file);
  if (!host) {
    return host.error;
  }
  try {
    engine = core::MultiInstrumentEngineAccess::restore_snapshot(*host.value, file.state_digest);
  } catch (const std::invalid_argument&) {
    return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema);
  }
  return {};
}

[[nodiscard]] std::optional<SelectedSnapshot> select_explicit_snapshot(
    const std::filesystem::path& path, SnapshotInspectionReport inspected, const LogHeader& header,
    const detail::LogScanResult& scan, const std::vector<LogBoundary>& boundaries,
    SnapshotRecoveryReport& report) {
  auto compatibility = compatible_snapshot(*inspected.snapshot, header, scan, boundaries);
  if (compatibility) {
    report.snapshot_error = compatibility;
    return std::nullopt;
  }
  std::unique_ptr<MultiInstrumentEngine> engine;
  auto restore_error = restore_candidate(*inspected.snapshot, engine);
  if (restore_error) {
    report.snapshot_error = restore_error;
    return std::nullopt;
  }
  return SelectedSnapshot{
      .path = path,
      .file = std::move(*inspected.snapshot),
      .engine = std::move(engine),
      .source = RecoverySource::explicit_snapshot,
  };
}

[[nodiscard]] std::optional<SelectedSnapshot> select_directory_snapshot(
    CandidateList listed, CodecLimits limits, const LogHeader& header,
    const detail::LogScanResult& scan, const std::vector<LogBoundary>& boundaries,
    SnapshotRecoveryReport& report) {
  for (const auto& candidate : listed.candidates) {
    SnapshotInspectionReport inspected;
    SnapshotError candidate_error = candidate.discovery_error;
    if (!candidate_error) {
      inspected = inspect_snapshot(candidate.path, limits);
      candidate_error = inspected.error;
    }
    if (!candidate_error && inspected.snapshot->covered_sequence != candidate.filename_sequence) {
      candidate_error = snapshot_error(SnapshotErrorCategory::sequence_mismatch);
    }
    if (!candidate_error) {
      candidate_error = compatible_snapshot(*inspected.snapshot, header, scan, boundaries);
    }

    std::unique_ptr<MultiInstrumentEngine> engine;
    if (!candidate_error) {
      candidate_error = restore_candidate(*inspected.snapshot, engine);
    }
    if (candidate_error) {
      report.skipped_snapshots.push_back({
          .path = candidate.path,
          .filename_sequence = candidate.filename_sequence,
          .error = candidate_error,
      });
      continue;
    }
    return SelectedSnapshot{
        .path = candidate.path,
        .file = std::move(*inspected.snapshot),
        .engine = std::move(engine),
        .source = RecoverySource::directory_snapshot,
    };
  }
  return std::nullopt;
}

[[nodiscard]] SnapshotRecoveryResult replay_selected_snapshot(
    detail::LogSource& source, const detail::LogScanResult& first_scan, SelectedSnapshot selected,
    ReplayOptions options, SnapshotRecoveryReport report) {
  report.recovery_source = selected.source;
  report.selected_snapshot = selected.path;
  report.covered_sequence = selected.file.covered_sequence;
  report.covered_log_byte_offset = selected.file.covered_log_byte_offset;
  report.snapshot_state_digest = selected.file.state_digest;

  auto pass = detail::execute_replay_pass(source, *selected.engine, options,
                                          selected.file.covered_log_byte_offset);
  if (!detail::same_validated_source(first_scan, pass.scan)) {
    report.replay.error = log_io_error(pass.scan.valid_end_offset,
                                       std::make_error_code(std::errc::state_not_recoverable));
    return {
        .engine = nullptr,
        .report = std::move(report),
    };
  }
  if (pass.scan.termination == detail::LogScanTermination::io_failure ||
      pass.scan.termination == detail::LogScanTermination::corruption) {
    report.replay.error = pass.scan.error;
    return {
        .engine = nullptr,
        .report = std::move(report),
    };
  }

  report.replay.records_replayed = pass.records_replayed;
  report.replay.committed = pass.committed;
  report.replay.rejected = pass.rejected;
  report.replay.divergence = std::move(pass.divergence);
  if (!report.replay.divergence.has_value() &&
      !core::MultiInstrumentEngineAccess::validate_invariants(*selected.engine)) {
    report.replay.divergence = ReplayDivergence{
        .record_offset = report.replay.valid_end_offset,
        .sequence = report.replay.last_sequence.value_or(domain::Sequence{}),
        .category = ReplayDivergenceCategory::invariant,
        .command = std::nullopt,
        .expected = {},
        .actual = {},
        .actual_engine_error = EngineError::none,
        .actual_events = {},
    };
  }
  if (report.replay.divergence.has_value()) {
    return {
        .engine = nullptr,
        .report = std::move(report),
    };
  }
  report.replay.final_state_digest = selected.engine->state_digest();
  return {
      .engine = std::move(selected.engine),
      .report = std::move(report),
  };
}

enum class SnapshotSelectionMode : std::uint8_t {
  explicit_path = 1,
  directory = 2,
};

[[nodiscard]] SnapshotRecoveryResult recover_with_selection(
    detail::LogSource& source, const std::filesystem::path& snapshot_location,
    SnapshotSelectionMode selection_mode, ReplayOptions options) {
  SnapshotRecoveryReport report;
  report.recovery_source = selection_mode == SnapshotSelectionMode::explicit_path
                               ? RecoverySource::explicit_snapshot
                               : RecoverySource::directory_snapshot;
  report.replay.mode = options.mode;
  report.replay.tail_policy = options.tail_policy;
  if (!options.valid()) {
    report.replay.error = {
        .category = LogErrorCategory::invalid_length,
        .byte_offset = 0U,
        .system_error = std::make_error_code(std::errc::invalid_argument),
    };
    return {
        .engine = nullptr,
        .report = std::move(report),
    };
  }

  BoundaryVisitor outcomes;
  auto first_scan =
      detail::scan_command_log(source, {.codec_limits = options.codec_limits}, &outcomes);
  initialize_replay_report(report.replay, options, first_scan);
  report.replay.committed = outcomes.committed();
  report.replay.rejected = outcomes.rejected();
  if (!scan_usable(first_scan, options, report.replay)) {
    return {
        .engine = nullptr,
        .report = std::move(report),
    };
  }

  SnapshotInspectionReport explicit_inspection;
  CandidateList directory_candidates;
  std::vector<domain::Sequence> boundary_targets;
  if (selection_mode == SnapshotSelectionMode::explicit_path) {
    explicit_inspection = inspect_snapshot(snapshot_location, options.codec_limits);
    if (!explicit_inspection) {
      report.snapshot_error = explicit_inspection.error;
      return {
          .engine = nullptr,
          .report = std::move(report),
      };
    }
    boundary_targets.push_back(explicit_inspection.snapshot->covered_sequence);
  } else {
    directory_candidates = list_candidates(snapshot_location, first_scan.header->log_id);
    if (directory_candidates.error) {
      report.snapshot_error = directory_candidates.error;
      return {
          .engine = nullptr,
          .report = std::move(report),
      };
    }
    boundary_targets.reserve(directory_candidates.candidates.size());
    for (const auto& candidate : directory_candidates.candidates) {
      boundary_targets.push_back(candidate.filename_sequence);
    }
  }

  BoundaryVisitor boundaries{std::move(boundary_targets)};
  auto boundary_scan =
      detail::scan_command_log(source, {.codec_limits = options.codec_limits}, &boundaries);
  if (!detail::same_validated_source(first_scan, boundary_scan)) {
    report.replay.error = log_io_error(boundary_scan.valid_end_offset,
                                       std::make_error_code(std::errc::state_not_recoverable));
    return {
        .engine = nullptr,
        .report = std::move(report),
    };
  }

  std::optional<SelectedSnapshot> selected;
  if (selection_mode == SnapshotSelectionMode::explicit_path) {
    selected =
        select_explicit_snapshot(snapshot_location, std::move(explicit_inspection),
                                 *first_scan.header, first_scan, boundaries.boundaries(), report);
    if (!selected.has_value()) {
      return {
          .engine = nullptr,
          .report = std::move(report),
      };
    }
  } else {
    selected =
        select_directory_snapshot(std::move(directory_candidates), options.codec_limits,
                                  *first_scan.header, first_scan, boundaries.boundaries(), report);
    if (!selected.has_value()) {
      if (report.snapshot_error) {
        return {
            .engine = nullptr,
            .report = std::move(report),
        };
      }
      auto replayed = detail::replay_validated_log_source(source, first_scan, options);
      report.recovery_source = RecoverySource::full_log;
      report.replay = std::move(replayed.report);
      return {
          .engine = std::move(replayed.engine),
          .report = std::move(report),
      };
    }
  }

  return replay_selected_snapshot(source, first_scan, std::move(*selected), options,
                                  std::move(report));
}

}  // namespace

namespace detail {

SnapshotRecoveryResult recover_log_from_snapshot_source(LogSource& source,
                                                        const std::filesystem::path& snapshot_path,
                                                        ReplayOptions options) {
  return recover_with_selection(source, snapshot_path, SnapshotSelectionMode::explicit_path,
                                options);
}

SnapshotRecoveryResult recover_log_from_snapshot_directory_source(
    LogSource& source, const std::filesystem::path& snapshot_directory, ReplayOptions options) {
  return recover_with_selection(source, snapshot_directory, SnapshotSelectionMode::directory,
                                options);
}

}  // namespace detail

SnapshotRecoveryResult recover_log_from_snapshot(const std::filesystem::path& log_path,
                                                 const std::filesystem::path& snapshot_path,
                                                 ReplayOptions options) {
  auto source = detail::open_native_log_source(log_path);
  if (!source) {
    SnapshotRecoveryReport report;
    report.recovery_source = RecoverySource::explicit_snapshot;
    report.replay.mode = options.mode;
    report.replay.tail_policy = options.tail_policy;
    report.replay.error = log_io_error(source.failure.offset, source.failure.system_error);
    return {
        .engine = nullptr,
        .report = std::move(report),
    };
  }
  return detail::recover_log_from_snapshot_source(*source.source, snapshot_path, options);
}

SnapshotRecoveryResult recover_log_from_snapshot_directory(
    const std::filesystem::path& log_path, const std::filesystem::path& snapshot_directory,
    ReplayOptions options) {
  auto source = detail::open_native_log_source(log_path);
  if (!source) {
    SnapshotRecoveryReport report;
    report.recovery_source = RecoverySource::directory_snapshot;
    report.replay.mode = options.mode;
    report.replay.tail_policy = options.tail_policy;
    report.replay.error = log_io_error(source.failure.offset, source.failure.system_error);
    return {
        .engine = nullptr,
        .report = std::move(report),
    };
  }
  return detail::recover_log_from_snapshot_directory_source(*source.source, snapshot_directory,
                                                            options);
}

}  // namespace atlaslob::persistence
