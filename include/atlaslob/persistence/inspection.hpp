#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include "atlaslob/persistence/command_log.hpp"

namespace atlaslob::persistence {

enum class LogTail : std::uint8_t {
  clean = 1,
  torn = 2,
  unknown = 3,
};

[[nodiscard]] constexpr std::string_view to_string(LogTail value) noexcept {
  switch (value) {
    case LogTail::clean:
      return "clean";
    case LogTail::torn:
      return "torn";
    case LogTail::unknown:
      return "unknown";
  }
  return "unknown";
}

struct RecordSummary final {
  std::uint64_t offset{};
  std::uint32_t total_length{};
  std::uint32_t payload_length{};
  std::uint16_t record_version{};
  domain::Sequence sequence{};
  domain::CommandType command_type{};
  RecordOutcome outcome{RecordOutcome::committed};
  domain::RejectReason rejection_reason{domain::RejectReason::none};
  std::uint64_t event_count{};
  Digest256 event_digest{};
};

struct LogInspectionReport final {
  std::optional<LogHeader> header;
  std::optional<Digest256> configuration_digest;
  std::optional<domain::Sequence> last_sequence;
  std::uint64_t header_length{};
  std::uint64_t catalog_length{};
  std::uint64_t input_bytes{};
  std::uint64_t valid_prefix_bytes{};
  std::uint64_t records_scanned{};
  LogTail tail{LogTail::unknown};
  LogError error{};
  std::optional<std::vector<RecordSummary>> records;

  [[nodiscard]] bool clean() const noexcept { return error.ok() && tail == LogTail::clean; }
  [[nodiscard]] bool warning() const noexcept {
    return error.category == LogErrorCategory::truncated_final_record;
  }
};

struct LogRepairReport final {
  LogInspectionReport inspection;
  bool output_created{};
  std::optional<std::uint64_t> output_bytes;
};

[[nodiscard]] LogInspectionReport inspect_log(const std::filesystem::path& path,
                                              bool include_records = false);

[[nodiscard]] LogRepairReport repair_log_tail(const std::filesystem::path& input,
                                              const std::filesystem::path& new_output,
                                              bool include_records = false);

}  // namespace atlaslob::persistence
