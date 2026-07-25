#pragma once

#include <cstdint>
#include <string_view>
#include <system_error>
#include <vector>

#include "atlaslob/digest.hpp"
#include "atlaslob/multi_instrument_engine.hpp"
#include "atlaslob/persistence/command_log.hpp"
#include "atlaslob/persistence/types.hpp"

namespace atlaslob::persistence {

enum class SnapshotErrorCategory : std::uint8_t {
  none = 0,
  invalid_length = 1,
  excessive_length = 2,
  unsupported_format_version = 3,
  semantic_version_mismatch = 4,
  bad_checksum = 5,
  configuration_digest_mismatch = 6,
  invalid_catalog = 7,
  invalid_snapshot_schema = 8,
  state_digest_mismatch = 9,
  log_id_mismatch = 10,
  log_boundary_mismatch = 11,
  sequence_mismatch = 12,
  io_failure = 13,
};

[[nodiscard]] constexpr std::string_view to_string(SnapshotErrorCategory value) noexcept {
  switch (value) {
    case SnapshotErrorCategory::none:
      return "none";
    case SnapshotErrorCategory::invalid_length:
      return "invalid_length";
    case SnapshotErrorCategory::excessive_length:
      return "excessive_length";
    case SnapshotErrorCategory::unsupported_format_version:
      return "unsupported_format_version";
    case SnapshotErrorCategory::semantic_version_mismatch:
      return "semantic_version_mismatch";
    case SnapshotErrorCategory::bad_checksum:
      return "bad_checksum";
    case SnapshotErrorCategory::configuration_digest_mismatch:
      return "configuration_digest_mismatch";
    case SnapshotErrorCategory::invalid_catalog:
      return "invalid_catalog";
    case SnapshotErrorCategory::invalid_snapshot_schema:
      return "invalid_snapshot_schema";
    case SnapshotErrorCategory::state_digest_mismatch:
      return "state_digest_mismatch";
    case SnapshotErrorCategory::log_id_mismatch:
      return "log_id_mismatch";
    case SnapshotErrorCategory::log_boundary_mismatch:
      return "log_boundary_mismatch";
    case SnapshotErrorCategory::sequence_mismatch:
      return "sequence_mismatch";
    case SnapshotErrorCategory::io_failure:
      return "io_failure";
  }
  return "unknown";
}

struct SnapshotError final {
  SnapshotErrorCategory category{SnapshotErrorCategory::none};
  std::uint64_t byte_offset{};
  std::error_code system_error{};

  [[nodiscard]] bool ok() const noexcept { return category == SnapshotErrorCategory::none; }
  [[nodiscard]] explicit operator bool() const noexcept { return !ok(); }

  bool operator==(const SnapshotError&) const = default;
};

// The bounded, value-only representation of an ATLSSN01 file. Its catalog and
// instruments are canonical: both are sorted by ascending instrument ID,
// levels are best-to-worst, and orders retain exact FIFO order.
struct SnapshotFile final {
  std::uint16_t format_version{snapshot_format_version};
  std::uint16_t semantics_version{atlaslob_semantics_version};
  LogId log_id{};
  domain::Sequence covered_sequence{};
  std::uint64_t covered_log_byte_offset{};
  bool sequence_exhausted{};
  PersistedEngineConfig engine_config{};
  std::vector<PersistedInstrumentConfig> catalog;
  Digest256 configuration_digest{};
  std::uint64_t active_order_count{};
  std::vector<InstrumentSnapshot> instruments;
  Digest256 state_digest{};

  bool operator==(const SnapshotFile&) const = default;
};

}  // namespace atlaslob::persistence
