#include "atlaslob/persistence/replay.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <variant>

#include "atlaslob/domain/events.hpp"
#include "command_log_codec.hpp"
#include "log_io.hpp"
#include "log_scanner.hpp"
#include "multi_instrument_engine_access.hpp"
#include "replay_internal.hpp"

namespace atlaslob::persistence {
namespace {

[[nodiscard]] LogError io_error(std::uint64_t offset, std::error_code error) noexcept {
  return {
      .category = LogErrorCategory::io_failure,
      .byte_offset = offset,
      .system_error = error,
  };
}

[[nodiscard]] ReplayTail replay_tail(detail::LogScanTermination termination) noexcept {
  switch (termination) {
    case detail::LogScanTermination::clean_eof:
      return ReplayTail::clean;
    case detail::LogScanTermination::truncated_tail:
      return ReplayTail::torn;
    case detail::LogScanTermination::corruption:
    case detail::LogScanTermination::io_failure:
      return ReplayTail::unknown;
  }
  return ReplayTail::unknown;
}

[[nodiscard]] ReplayEvidenceSummary expected_summary(const CommandRecord& record) {
  return {
      .outcome = record.outcome,
      .rejection_reason = record.rejection_reason,
      .event_count = record.event_count,
      .event_digest = record.event_digest,
  };
}

[[nodiscard]] ReplayEvidenceSummary actual_summary(const EngineResult& result) {
  const auto* batch = result.batch();
  if (batch == nullptr) {
    return {};
  }
  auto rejection = domain::RejectReason::none;
  if (result.rejected()) {
    if (batch->empty()) {
      return {};
    }
    const auto* rejected = std::get_if<domain::RejectedEvent>(&(*batch)[0]);
    if (rejected == nullptr) {
      return {};
    }
    rejection = rejected->reason;
  }
  return {
      .outcome = result.rejected() ? RecordOutcome::rejected : RecordOutcome::committed,
      .rejection_reason = rejection,
      .event_count = static_cast<std::uint64_t>(batch->size()),
      .event_digest = atlaslob::event_digest(*batch),
  };
}

[[nodiscard]] std::optional<ReplayDivergenceCategory> first_difference(
    const ReplayEvidenceSummary& expected, const ReplayEvidenceSummary& actual) noexcept {
  if (!actual.outcome.has_value()) {
    return ReplayDivergenceCategory::engine_error;
  }
  if (expected.outcome != actual.outcome) {
    return ReplayDivergenceCategory::outcome;
  }
  if (expected.rejection_reason != actual.rejection_reason) {
    return ReplayDivergenceCategory::rejection_reason;
  }
  if (expected.event_count != actual.event_count) {
    return ReplayDivergenceCategory::event_count;
  }
  if (expected.event_digest != actual.event_digest) {
    return ReplayDivergenceCategory::event_digest;
  }
  return std::nullopt;
}

class ReplayVisitor final : public detail::LogScanVisitor {
 public:
  ReplayVisitor(MultiInstrumentEngine& engine, ReplayReport& report, ReplayOptions options,
                std::uint64_t start_offset) noexcept
      : engine_{engine}, report_{report}, options_{options}, start_offset_{start_offset} {}

  void on_record(const CommandRecord& record, std::span<const std::uint8_t> encoded,
                 std::uint64_t frame_begin, std::uint64_t frame_end) override {
    static_cast<void>(encoded);
    static_cast<void>(frame_end);
    if (record.outcome == RecordOutcome::committed) {
      ++report_.committed;
    } else {
      ++report_.rejected;
    }
    if (frame_begin < start_offset_) {
      return;
    }
    if (report_.divergence.has_value()) {
      return;
    }

    auto result = engine_.execute(record.command);
    ++report_.records_replayed;
    const auto expected = expected_summary(record);
    const auto actual = actual_summary(result);

    std::optional<ReplayDivergenceCategory> difference;
    if (result.error() != EngineError::none || !actual.outcome.has_value()) {
      difference = ReplayDivergenceCategory::engine_error;
    } else if (options_.mode != ReplayMode::fast) {
      difference = first_difference(expected, actual);
    }

    if (!difference.has_value()) {
      const bool check_invariants = options_.mode == ReplayMode::diagnostic ||
                                    (options_.mode == ReplayMode::verify &&
                                     report_.records_replayed % options_.invariant_interval == 0U);
      if (check_invariants && !core::MultiInstrumentEngineAccess::validate_invariants(engine_)) {
        difference = ReplayDivergenceCategory::invariant;
      }
    }

    if (difference.has_value()) {
      ReplayDivergence divergence{
          .record_offset = frame_begin,
          .sequence = record.sequence,
          .category = *difference,
          .command = record.command,
          .expected = expected,
          .actual = actual,
          .actual_engine_error = result.error(),
          .actual_events = {},
      };
      if (const auto* batch = result.batch(); batch != nullptr) {
        divergence.actual_events.assign(batch->events().begin(), batch->events().end());
      }
      report_.divergence = std::move(divergence);
    }
  }

 private:
  MultiInstrumentEngine& engine_;
  ReplayReport& report_;
  ReplayOptions options_;
  std::uint64_t start_offset_{};
};

[[nodiscard]] bool same_source(const detail::LogScanResult& first,
                               const detail::LogScanResult& second) noexcept {
  return first.valid_prefix_digest.has_value() && second.valid_prefix_digest.has_value() &&
         second.source_extent == first.source_extent &&
         second.header_end_offset == first.header_end_offset &&
         second.valid_end_offset == first.valid_end_offset &&
         second.record_count == first.record_count && second.last_sequence == first.last_sequence &&
         second.next_sequence == first.next_sequence && second.termination == first.termination &&
         second.valid_prefix_digest == first.valid_prefix_digest;
}

}  // namespace

namespace detail {

ReplayTail public_replay_tail(LogScanTermination termination) noexcept {
  return replay_tail(termination);
}

bool same_validated_source(const LogScanResult& first, const LogScanResult& second) noexcept {
  return same_source(first, second);
}

ReplayPassResult execute_replay_pass(LogSource& source, MultiInstrumentEngine& engine,
                                     ReplayOptions options, std::uint64_t start_offset) {
  ReplayReport evidence;
  ReplayVisitor visitor{engine, evidence, options, start_offset};
  auto scan = scan_command_log(source, {.codec_limits = options.codec_limits}, &visitor);
  return {
      .scan = std::move(scan),
      .records_replayed = evidence.records_replayed,
      .committed = evidence.committed,
      .rejected = evidence.rejected,
      .divergence = std::move(evidence.divergence),
  };
}

ReplayResult replay_validated_log_source(LogSource& source, const LogScanResult& first_scan,
                                         ReplayOptions options) {
  ReplayReport report;
  report.mode = options.mode;
  report.tail_policy = options.tail_policy;
  if (!options.valid()) {
    report.error = {
        .category = LogErrorCategory::invalid_length,
        .byte_offset = 0U,
        .system_error = std::make_error_code(std::errc::invalid_argument),
    };
    return {.engine = nullptr, .report = std::move(report)};
  }

  report.tail = public_replay_tail(first_scan.termination);
  report.header = first_scan.header;
  report.last_sequence = first_scan.last_sequence;
  report.valid_end_offset = first_scan.valid_end_offset;
  report.records_scanned = first_scan.record_count;

  if (first_scan.termination == LogScanTermination::io_failure ||
      first_scan.termination == LogScanTermination::corruption) {
    report.error = first_scan.error;
    return {.engine = nullptr, .report = std::move(report)};
  }
  if (first_scan.termination == LogScanTermination::truncated_tail &&
      options.tail_policy == TailPolicy::strict) {
    report.error = first_scan.error;
    return {.engine = nullptr, .report = std::move(report)};
  }
  report.used_valid_prefix = first_scan.termination == LogScanTermination::truncated_tail &&
                             options.tail_policy == TailPolicy::valid_prefix;
  if (report.used_valid_prefix) {
    report.warning = first_scan.error;
  }

  if (!first_scan.header.has_value()) {
    report.error = {
        .category = LogErrorCategory::invalid_length,
        .byte_offset = 0U,
    };
    return {.engine = nullptr, .report = std::move(report)};
  }
  auto host = detail::host_configuration(*first_scan.header);
  if (!host) {
    report.error = host.error;
    return {.engine = nullptr, .report = std::move(report)};
  }

  auto engine =
      std::make_unique<MultiInstrumentEngine>(host.value->catalog, host.value->engine_config);
  auto replay_pass = execute_replay_pass(source, *engine, options, first_scan.header_end_offset);
  if (!same_validated_source(first_scan, replay_pass.scan)) {
    report.error = io_error(replay_pass.scan.valid_end_offset,
                            std::make_error_code(std::errc::state_not_recoverable));
    return {.engine = nullptr, .report = std::move(report)};
  }
  if (replay_pass.scan.termination == LogScanTermination::io_failure ||
      replay_pass.scan.termination == LogScanTermination::corruption) {
    report.error = replay_pass.scan.error;
    return {.engine = nullptr, .report = std::move(report)};
  }

  report.records_replayed = replay_pass.records_replayed;
  report.committed = replay_pass.committed;
  report.rejected = replay_pass.rejected;
  report.divergence = std::move(replay_pass.divergence);
  if (!report.divergence.has_value() &&
      !core::MultiInstrumentEngineAccess::validate_invariants(*engine)) {
    report.divergence = ReplayDivergence{
        .record_offset = report.valid_end_offset,
        .sequence = report.last_sequence.value_or(domain::Sequence{}),
        .category = ReplayDivergenceCategory::invariant,
        .command = std::nullopt,
        .expected = {},
        .actual = {},
        .actual_engine_error = EngineError::none,
        .actual_events = {},
    };
  }
  if (report.divergence.has_value()) {
    return {.engine = nullptr, .report = std::move(report)};
  }

  report.final_state_digest = engine->state_digest();
  return {
      .engine = std::move(engine),
      .report = std::move(report),
  };
}

ReplayResult replay_log_source(LogSource& source, ReplayOptions options) {
  if (!options.valid()) {
    ReplayReport report;
    report.mode = options.mode;
    report.tail_policy = options.tail_policy;
    report.error = {
        .category = LogErrorCategory::invalid_length,
        .byte_offset = 0U,
        .system_error = std::make_error_code(std::errc::invalid_argument),
    };
    return {.engine = nullptr, .report = std::move(report)};
  }
  const auto scan_options = LogScanOptions{
      .codec_limits = options.codec_limits,
  };
  auto first_scan = scan_command_log(source, scan_options);
  return replay_validated_log_source(source, first_scan, options);
}

}  // namespace detail

ReplayResult replay_log(const std::filesystem::path& path, ReplayOptions options) {
  if (!options.valid()) {
    ReplayReport report;
    report.mode = options.mode;
    report.tail_policy = options.tail_policy;
    report.error = {
        .category = LogErrorCategory::invalid_length,
        .byte_offset = 0U,
        .system_error = std::make_error_code(std::errc::invalid_argument),
    };
    return {.engine = nullptr, .report = std::move(report)};
  }

  auto source = detail::open_native_log_source(path);
  if (!source) {
    ReplayReport report;
    report.mode = options.mode;
    report.tail_policy = options.tail_policy;
    report.error = io_error(source.failure.offset, source.failure.system_error);
    return {.engine = nullptr, .report = std::move(report)};
  }
  return detail::replay_log_source(*source.source, options);
}

}  // namespace atlaslob::persistence
