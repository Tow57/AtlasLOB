#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#include "atlaslob/digest.hpp"
#include "atlaslob/persistence/command_log.hpp"
#include "log_io.hpp"

namespace atlaslob::persistence::detail {

enum class LogScanTermination : std::uint8_t {
  clean_eof = 1,
  truncated_tail = 2,
  corruption = 3,
  io_failure = 4,
};

[[nodiscard]] constexpr std::string_view to_string(LogScanTermination value) noexcept {
  switch (value) {
    case LogScanTermination::clean_eof:
      return "clean_eof";
    case LogScanTermination::truncated_tail:
      return "truncated_tail";
    case LogScanTermination::corruption:
      return "corruption";
    case LogScanTermination::io_failure:
      return "io_failure";
  }
  return "unknown";
}

struct LogScanOptions final {
  CodecLimits codec_limits{};
  std::size_t read_chunk_bytes{default_log_io_chunk_bytes};

  [[nodiscard]] constexpr bool valid() const noexcept { return read_chunk_bytes != 0U; }
};

struct LogScanResult final {
  LogScanTermination termination{LogScanTermination::corruption};
  LogError error{};
  std::uint64_t source_extent{};
  std::uint64_t header_end_offset{};
  std::uint64_t valid_end_offset{};
  std::uint64_t record_count{};
  std::optional<domain::Sequence> last_sequence;
  std::optional<domain::Sequence> next_sequence;
  std::optional<LogHeader> header;
  // SHA-256 of the exact encoded header and complete validated records. It is
  // present only for clean logs and repairable torn tails.
  std::optional<Digest256> valid_prefix_digest;

  [[nodiscard]] bool clean() const noexcept {
    return termination == LogScanTermination::clean_eof && error.ok();
  }

  [[nodiscard]] bool repairable() const noexcept {
    return header.has_value() && termination == LogScanTermination::truncated_tail;
  }
};

// Bounded pull visitor. Encoded spans are valid only for the duration of the
// callback. Exceptions propagate, including allocation/resource failures.
class LogScanVisitor {
 public:
  LogScanVisitor() = default;
  LogScanVisitor(const LogScanVisitor&) = delete;
  LogScanVisitor& operator=(const LogScanVisitor&) = delete;
  LogScanVisitor(LogScanVisitor&&) = delete;
  LogScanVisitor& operator=(LogScanVisitor&&) = delete;
  virtual ~LogScanVisitor() = default;

  virtual void on_header(const LogHeader& header, std::span<const std::uint8_t> encoded) {
    static_cast<void>(header);
    static_cast<void>(encoded);
  }

  virtual void on_record(const CommandRecord& record, std::span<const std::uint8_t> encoded,
                         std::uint64_t frame_begin, std::uint64_t frame_end) {
    static_cast<void>(record);
    static_cast<void>(encoded);
    static_cast<void>(frame_begin);
    static_cast<void>(frame_end);
  }
};

[[nodiscard]] LogScanResult scan_command_log(LogSource& source, LogScanOptions options = {},
                                             LogScanVisitor* visitor = nullptr);

// Writes only complete frames after length, checksum, schema, and sequence
// validation. The destination must start empty. A hard-corruption or I/O result
// means the caller must discard the partial destination.
[[nodiscard]] LogScanResult scan_command_log_to_sink(LogSource& source, LogSink& sink,
                                                     LogScanOptions options = {},
                                                     LogScanVisitor* visitor = nullptr);

struct LogRepairResult final {
  LogScanResult scan;
  bool output_created{};
};

// Never modifies the input and never overwrites an existing output path. Only a
// torn final record is repairable; clean logs and hard corruption leave no
// destination after normal return.
[[nodiscard]] LogRepairResult repair_command_log_to_new_file(
    const std::filesystem::path& input_path, const std::filesystem::path& output_path,
    LogScanOptions options = {}, LogScanVisitor* visitor = nullptr);

}  // namespace atlaslob::persistence::detail
