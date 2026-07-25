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
  ReplayVisitor(MultiInstrumentEngine& engine, ReplayReport& report, ReplayOptions options) noexcept
      : engine_{engine}, report_{report}, options_{options} {}

  void on_record(const CommandRecord& record, std::span<const std::uint8_t> encoded,
                 std::uint64_t frame_begin, std::uint64_t frame_end) override {
    static_cast<void>(encoded);
    static_cast<void>(frame_end);
    if (report_.divergence.has_value()) {
      return;
    }

    auto result = engine_.execute(record.command);
    ++report_.records_replayed;
    const auto expected = expected_summary(record);
    const auto actual = actual_summary(result);

    if (result.committed()) {
      ++report_.committed;
    } else if (result.rejected()) {
      ++report_.rejected;
    }

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
};

[[nodiscard]] bool same_validated_source(const detail::LogScanResult& first,
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

ReplayResult replay_log(const std::filesystem::path& path, ReplayOptions options) {
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

  auto first_source = detail::open_native_log_source(path);
  if (!first_source) {
    report.error = io_error(first_source.failure.offset, first_source.failure.system_error);
    return {.engine = nullptr, .report = std::move(report)};
  }
  const auto scan_options = detail::LogScanOptions{
      .codec_limits = options.codec_limits,
  };
  auto first_scan = detail::scan_command_log(*first_source.source, scan_options);
  report.tail = replay_tail(first_scan.termination);
  report.header = first_scan.header;
  report.last_sequence = first_scan.last_sequence;
  report.valid_end_offset = first_scan.valid_end_offset;
  report.records_scanned = first_scan.record_count;

  if (first_scan.termination == detail::LogScanTermination::io_failure ||
      first_scan.termination == detail::LogScanTermination::corruption) {
    report.error = first_scan.error;
    return {.engine = nullptr, .report = std::move(report)};
  }
  if (first_scan.termination == detail::LogScanTermination::truncated_tail &&
      options.tail_policy == TailPolicy::strict) {
    report.error = first_scan.error;
    return {.engine = nullptr, .report = std::move(report)};
  }
  report.used_valid_prefix = first_scan.termination == detail::LogScanTermination::truncated_tail;
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
  ReplayReport replay_evidence;
  ReplayVisitor visitor{*engine, replay_evidence, options};
  const auto second_scan = detail::scan_command_log(*first_source.source, scan_options, &visitor);
  if (!same_validated_source(first_scan, second_scan)) {
    report.error = io_error(second_scan.valid_end_offset,
                            std::make_error_code(std::errc::state_not_recoverable));
    return {.engine = nullptr, .report = std::move(report)};
  }
  if (second_scan.termination == detail::LogScanTermination::io_failure ||
      second_scan.termination == detail::LogScanTermination::corruption) {
    report.error = second_scan.error;
    return {.engine = nullptr, .report = std::move(report)};
  }

  report.records_replayed = replay_evidence.records_replayed;
  report.committed = replay_evidence.committed;
  report.rejected = replay_evidence.rejected;
  report.divergence = std::move(replay_evidence.divergence);
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

}  // namespace atlaslob::persistence
