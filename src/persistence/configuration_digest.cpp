#include "atlaslob/persistence/configuration_digest.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../utility/sha256.hpp"
#include "binary_codec.hpp"

namespace atlaslob::persistence {
namespace {

constexpr std::array<std::uint8_t, 8U> configuration_prefix{
    'A', 'T', 'L', 'S', 'C', 'F', '0', '1',
};

void update_u16(utility::Sha256& hash, std::uint16_t value) noexcept {
  const std::array bytes{
      static_cast<std::uint8_t>((value >> 8U) & 0xffU),
      static_cast<std::uint8_t>(value & 0xffU),
  };
  hash.update(bytes);
}

void update_u32(utility::Sha256& hash, std::uint32_t value) noexcept {
  const std::array bytes{
      static_cast<std::uint8_t>((value >> 24U) & 0xffU),
      static_cast<std::uint8_t>((value >> 16U) & 0xffU),
      static_cast<std::uint8_t>((value >> 8U) & 0xffU),
      static_cast<std::uint8_t>(value & 0xffU),
  };
  hash.update(bytes);
}

void update_u64(utility::Sha256& hash, std::uint64_t value) noexcept {
  std::array<std::uint8_t, 8U> bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    const auto shift = static_cast<unsigned>((7U - index) * 8U);
    bytes[index] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
  }
  hash.update(bytes);
}

}  // namespace

Digest256 configuration_digest(std::span<const PersistedInstrumentConfig> canonical_catalog,
                               PersistedEngineConfig engine_config,
                               std::uint16_t semantics_version) noexcept {
  utility::Sha256 hash;
  hash.update(configuration_prefix);
  update_u16(hash, semantics_version);
  update_u64(hash, engine_config.max_total_active_orders);
  update_u64(hash, static_cast<std::uint64_t>(canonical_catalog.size()));
  for (const auto& instrument : canonical_catalog) {
    update_u32(hash, instrument.instrument_id.value());
    update_u64(hash, instrument.max_order_quantity);
    update_u64(hash, static_cast<std::uint64_t>(instrument.tick_increment.value()));
    update_u64(hash, instrument.max_active_orders);
  }
  return hash.finish();
}

Digest256 configuration_digest(std::span<const InstrumentConfig> canonical_catalog,
                               MultiInstrumentEngineConfig engine_config,
                               std::uint16_t semantics_version) noexcept {
  utility::Sha256 hash;
  hash.update(configuration_prefix);
  update_u16(hash, semantics_version);
  update_u64(hash, detail::canonical_capacity(engine_config.max_total_active_orders));
  update_u64(hash, static_cast<std::uint64_t>(canonical_catalog.size()));
  for (const auto& instrument : canonical_catalog) {
    update_u32(hash, instrument.instrument_id.value());
    update_u64(hash, instrument.matching.max_order_quantity.value());
    update_u64(hash, static_cast<std::uint64_t>(instrument.matching.tick_increment.value()));
    update_u64(hash, detail::canonical_capacity(instrument.matching.max_active_orders));
  }
  return hash.finish();
}

}  // namespace atlaslob::persistence
