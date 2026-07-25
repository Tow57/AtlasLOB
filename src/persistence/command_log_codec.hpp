#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atlaslob/persistence/command_log.hpp"

namespace atlaslob::persistence::detail {

inline constexpr std::array<std::uint8_t, 8U> command_log_magic{
    'A', 'T', 'L', 'S', 'L', 'G', '0', '1',
};
inline constexpr std::size_t command_log_header_fixed_prefix_bytes{60U};
inline constexpr std::size_t command_log_catalog_entry_bytes{28U};
inline constexpr std::size_t command_log_header_fixed_bytes{96U};
inline constexpr std::size_t command_log_record_length_prefix_bytes{8U};
inline constexpr std::size_t command_log_record_fixed_bytes{66U};
inline constexpr std::size_t new_order_payload_bytes{36U};
inline constexpr std::size_t cancel_order_payload_bytes{16U};
inline constexpr std::size_t replace_order_payload_bytes{40U};

template <typename Value>
struct CodecResult final {
  std::optional<Value> value;
  LogError error{};

  [[nodiscard]] bool has_value() const noexcept { return value.has_value() && error.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
};

struct HostConfiguration final {
  MultiInstrumentEngineConfig engine_config{};
  std::vector<InstrumentConfig> catalog;
};

// The writer supplies the engine's canonical sorted catalog.
[[nodiscard]] CodecResult<LogHeader> make_log_header(
    std::span<const InstrumentConfig> canonical_catalog, MultiInstrumentEngineConfig engine_config,
    LogId log_id);

// The codec retains canonical u64 capacities. Only this replay boundary
// rejects finite capacities that the host engine cannot represent.
[[nodiscard]] CodecResult<HostConfiguration> host_configuration(const LogHeader& header);

[[nodiscard]] CodecResult<std::size_t> inspect_log_header_length(
    std::span<const std::uint8_t> fixed_prefix, CodecLimits limits = {}) noexcept;
[[nodiscard]] CodecResult<std::size_t> inspect_log_record_length(
    std::span<const std::uint8_t> length_prefix, CodecLimits limits = {}) noexcept;

[[nodiscard]] CodecResult<std::vector<std::uint8_t>> encode_log_header(const LogHeader& header,
                                                                       CodecLimits limits = {});
[[nodiscard]] CodecResult<LogHeader> decode_log_header(std::span<const std::uint8_t> bytes,
                                                       CodecLimits limits = {});

[[nodiscard]] CodecResult<std::vector<std::uint8_t>> encode_command_record(
    const CommandRecord& record, CodecLimits limits = {});
[[nodiscard]] CodecResult<CommandRecord> decode_command_record(std::span<const std::uint8_t> bytes,
                                                               CodecLimits limits = {});

}  // namespace atlaslob::persistence::detail
