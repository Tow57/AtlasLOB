#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atlaslob/persistence/snapshot.hpp"

namespace atlaslob::persistence::detail {

inline constexpr std::array<std::uint8_t, 8U> snapshot_magic{
    'A', 'T', 'L', 'S', 'S', 'N', '0', '1',
};
inline constexpr std::size_t snapshot_fixed_prefix_bytes{24U};
inline constexpr std::size_t snapshot_fixed_bytes{169U};
inline constexpr std::size_t snapshot_catalog_entry_bytes{28U};
inline constexpr std::size_t snapshot_instrument_fixed_bytes{36U};
inline constexpr std::size_t snapshot_level_fixed_bytes{32U};
inline constexpr std::size_t snapshot_order_bytes{41U};

template <typename Value>
struct SnapshotCodecResult final {
  std::optional<Value> value;
  SnapshotError error{};

  [[nodiscard]] bool has_value() const noexcept { return value.has_value() && error.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
};

// Captures an already canonical engine state and computes both persisted
// digests. The log offset must identify the boundary immediately after the
// covered command record; cross-checking that boundary belongs to recovery.
[[nodiscard]] SnapshotCodecResult<SnapshotFile> make_snapshot_file(
    const EngineSnapshot& snapshot, LogId log_id, std::uint64_t covered_log_byte_offset);

// Converts a validated persisted image into the host representation used by
// the all-or-nothing core restore seam.
[[nodiscard]] SnapshotCodecResult<EngineSnapshot> host_engine_snapshot(
    const SnapshotFile& snapshot);

[[nodiscard]] SnapshotCodecResult<std::size_t> inspect_snapshot_length(
    std::span<const std::uint8_t> fixed_prefix, CodecLimits limits = {}) noexcept;

[[nodiscard]] SnapshotCodecResult<std::vector<std::uint8_t>> encode_snapshot(
    const SnapshotFile& snapshot, CodecLimits limits = {});

[[nodiscard]] SnapshotCodecResult<SnapshotFile> decode_snapshot(std::span<const std::uint8_t> bytes,
                                                                CodecLimits limits = {});

}  // namespace atlaslob::persistence::detail
