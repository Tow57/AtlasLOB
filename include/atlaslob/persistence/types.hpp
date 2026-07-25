#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace atlaslob::persistence {

inline constexpr std::uint16_t command_log_format_version{1U};
inline constexpr std::uint16_t command_log_record_version{1U};
inline constexpr std::uint32_t command_log_byte_order_marker{0x01020304U};
inline constexpr std::uint64_t command_log_first_sequence{1U};

inline constexpr std::uint32_t default_max_log_header_bytes{1024U * 1024U};
inline constexpr std::uint32_t default_max_log_record_bytes{64U * 1024U};
inline constexpr std::uint64_t default_max_snapshot_bytes{256ULL * 1024ULL * 1024ULL};

struct LogId final {
  std::array<std::uint8_t, 16U> bytes{};

  [[nodiscard]] std::string hex() const {
    static constexpr std::string_view digits{"0123456789abcdef"};
    std::string result(bytes.size() * 2U, '0');
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
      result[index * 2U] = digits[(bytes[index] >> 4U) & 0x0fU];
      result[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
    }
    return result;
  }

  bool operator==(const LogId&) const = default;
};

enum class Durability : std::uint8_t {
  buffered = 1,
  flush_each_record = 2,
  sync_each_record = 3,
};

[[nodiscard]] constexpr bool is_valid(Durability value) noexcept {
  switch (value) {
    case Durability::buffered:
    case Durability::flush_each_record:
    case Durability::sync_each_record:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr std::string_view to_string(Durability value) noexcept {
  switch (value) {
    case Durability::buffered:
      return "buffered";
    case Durability::flush_each_record:
      return "flush_each_record";
    case Durability::sync_each_record:
      return "sync_each_record";
  }
  return "unknown";
}

struct CodecLimits final {
  static constexpr std::uint32_t max_header_bytes{default_max_log_header_bytes};
  static constexpr std::uint32_t max_record_bytes{default_max_log_record_bytes};

  // The log V1 bounds are fixed so a caller cannot turn an untrusted length
  // into an unbounded allocation. Snapshot callers may explicitly raise their
  // separate bound for a known, larger state image.
  std::uint64_t max_snapshot_bytes{default_max_snapshot_bytes};

  [[nodiscard]] constexpr bool valid() const noexcept { return max_snapshot_bytes != 0U; }

  bool operator==(const CodecLimits&) const = default;
};

enum class LogErrorCategory : std::uint8_t {
  none = 0,
  truncated_final_record = 1,
  invalid_length = 2,
  excessive_length = 3,
  unsupported_format_version = 4,
  unsupported_record_version = 5,
  unknown_record_type = 6,
  bad_header_checksum = 7,
  bad_record_checksum = 8,
  invalid_command_schema = 9,
  duplicate_sequence = 10,
  missing_sequence = 11,
  semantic_version_mismatch = 12,
  catalog_configuration_mismatch = 13,
  io_failure = 14,
};

[[nodiscard]] constexpr std::string_view to_string(LogErrorCategory value) noexcept {
  switch (value) {
    case LogErrorCategory::none:
      return "none";
    case LogErrorCategory::truncated_final_record:
      return "truncated_final_record";
    case LogErrorCategory::invalid_length:
      return "invalid_length";
    case LogErrorCategory::excessive_length:
      return "excessive_length";
    case LogErrorCategory::unsupported_format_version:
      return "unsupported_format_version";
    case LogErrorCategory::unsupported_record_version:
      return "unsupported_record_version";
    case LogErrorCategory::unknown_record_type:
      return "unknown_record_type";
    case LogErrorCategory::bad_header_checksum:
      return "bad_header_checksum";
    case LogErrorCategory::bad_record_checksum:
      return "bad_record_checksum";
    case LogErrorCategory::invalid_command_schema:
      return "invalid_command_schema";
    case LogErrorCategory::duplicate_sequence:
      return "duplicate_sequence";
    case LogErrorCategory::missing_sequence:
      return "missing_sequence";
    case LogErrorCategory::semantic_version_mismatch:
      return "semantic_version_mismatch";
    case LogErrorCategory::catalog_configuration_mismatch:
      return "catalog_configuration_mismatch";
    case LogErrorCategory::io_failure:
      return "io_failure";
  }
  return "unknown";
}

struct LogError final {
  LogErrorCategory category{LogErrorCategory::none};
  std::uint64_t byte_offset{};
  std::error_code system_error{};

  [[nodiscard]] bool ok() const noexcept { return category == LogErrorCategory::none; }
  [[nodiscard]] explicit operator bool() const noexcept { return !ok(); }

  bool operator==(const LogError&) const = default;
};

}  // namespace atlaslob::persistence
