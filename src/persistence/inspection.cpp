#include "atlaslob/persistence/inspection.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atlaslob/persistence/configuration_digest.hpp"
#include "command_log_codec.hpp"
#include "log_io.hpp"
#include "log_scanner.hpp"

namespace atlaslob::persistence {
namespace {

[[nodiscard]] LogTail public_tail(detail::LogScanTermination termination) noexcept {
  switch (termination) {
    case detail::LogScanTermination::clean_eof:
      return LogTail::clean;
    case detail::LogScanTermination::truncated_tail:
      return LogTail::torn;
    case detail::LogScanTermination::corruption:
    case detail::LogScanTermination::io_failure:
      return LogTail::unknown;
  }
  return LogTail::unknown;
}

class SummaryVisitor final : public detail::LogScanVisitor {
 public:
  explicit SummaryVisitor(bool enabled) {
    if (enabled) {
      records_.emplace();
    }
  }

  void on_record(const CommandRecord& record, std::span<const std::uint8_t> encoded,
                 std::uint64_t frame_begin, std::uint64_t frame_end) override {
    if (!records_.has_value()) {
      return;
    }
    const auto payload_length =
        static_cast<std::uint32_t>(encoded.size() - detail::command_log_record_fixed_bytes);
    records_->push_back({
        .offset = frame_begin,
        .total_length = static_cast<std::uint32_t>(frame_end - frame_begin),
        .payload_length = payload_length,
        .record_version = record.record_version,
        .sequence = record.sequence,
        .command_type = domain::command_type(record.command),
        .outcome = record.outcome,
        .rejection_reason = record.rejection_reason,
        .event_count = record.event_count,
        .event_digest = record.event_digest,
    });
  }

  [[nodiscard]] std::optional<std::vector<RecordSummary>> take() { return std::move(records_); }

 private:
  std::optional<std::vector<RecordSummary>> records_;
};

[[nodiscard]] LogInspectionReport make_report(detail::LogScanResult scan, SummaryVisitor& visitor) {
  LogInspectionReport report{
      .header = std::move(scan.header),
      .configuration_digest = std::nullopt,
      .last_sequence = scan.last_sequence,
      .header_length = scan.header_end_offset,
      .catalog_length = 0U,
      .input_bytes = scan.source_extent,
      .valid_prefix_bytes = scan.valid_end_offset,
      .records_scanned = scan.record_count,
      .tail = public_tail(scan.termination),
      .error = scan.error,
      .records = visitor.take(),
  };
  if (report.header.has_value()) {
    report.catalog_length = static_cast<std::uint64_t>(report.header->catalog.size()) *
                            detail::command_log_catalog_entry_bytes;
    report.configuration_digest = configuration_digest(
        report.header->catalog, report.header->engine_config, report.header->semantics_version);
  }
  return report;
}

[[nodiscard]] detail::LogScanResult open_failure(const detail::LogIoFailure& failure) {
  detail::LogScanResult result;
  result.termination = detail::LogScanTermination::io_failure;
  result.error = {
      .category = LogErrorCategory::io_failure,
      .byte_offset = failure.offset,
      .system_error = failure.system_error,
  };
  return result;
}

}  // namespace

LogInspectionReport inspect_log(const std::filesystem::path& path, bool include_records) {
  SummaryVisitor visitor{include_records};
  auto source = detail::open_native_log_source(path);
  if (!source) {
    return make_report(open_failure(source.failure), visitor);
  }
  return make_report(detail::scan_command_log(*source.source, {}, &visitor), visitor);
}

LogRepairReport repair_log_tail(const std::filesystem::path& input,
                                const std::filesystem::path& new_output, bool include_records) {
  SummaryVisitor visitor{include_records};
  auto repaired = detail::repair_command_log_to_new_file(input, new_output, {}, &visitor);
  const auto output_bytes = repaired.output_created
                                ? std::optional<std::uint64_t>{repaired.scan.valid_end_offset}
                                : std::nullopt;
  return {
      .inspection = make_report(std::move(repaired.scan), visitor),
      .output_created = repaired.output_created,
      .output_bytes = output_bytes,
      .unpublished_artifact = std::move(repaired.unpublished_artifact),
  };
}

}  // namespace atlaslob::persistence
