#pragma once

#include <cstdint>
#include <span>

#include "atlaslob/digest.hpp"
#include "atlaslob/multi_instrument_engine.hpp"
#include "atlaslob/persistence/command_log.hpp"

namespace atlaslob::persistence {

// Hashes the canonical ATLSCF01 configuration encoding. Catalogs must already
// be sorted by ascending, unique instrument ID.
[[nodiscard]] Digest256 configuration_digest(
    std::span<const PersistedInstrumentConfig> canonical_catalog,
    PersistedEngineConfig engine_config,
    std::uint16_t semantics_version = atlaslob_semantics_version) noexcept;

// Convenience overload for an already canonical host-engine configuration.
[[nodiscard]] Digest256 configuration_digest(
    std::span<const InstrumentConfig> canonical_catalog, MultiInstrumentEngineConfig engine_config,
    std::uint16_t semantics_version = atlaslob_semantics_version) noexcept;

}  // namespace atlaslob::persistence
