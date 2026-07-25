#pragma once

#include <cstdint>
#include <vector>

#include "atlaslob/digest.hpp"
#include "atlaslob/domain/commands.hpp"
#include "atlaslob/domain/types.hpp"
#include "atlaslob/multi_instrument_engine.hpp"
#include "atlaslob/persistence/types.hpp"

namespace atlaslob::persistence {

enum class RecordOutcome : std::uint8_t {
  committed = 1,
  rejected = 2,
};

[[nodiscard]] constexpr bool is_valid(RecordOutcome value) noexcept {
  switch (value) {
    case RecordOutcome::committed:
    case RecordOutcome::rejected:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr std::string_view to_string(RecordOutcome value) noexcept {
  switch (value) {
    case RecordOutcome::committed:
      return "committed";
    case RecordOutcome::rejected:
      return "rejected";
  }
  return "unknown";
}

struct PersistedInstrumentConfig final {
  domain::InstrumentId instrument_id{};
  std::uint64_t max_order_quantity{};
  domain::PriceTicks tick_increment{};
  std::uint64_t max_active_orders{};

  bool operator==(const PersistedInstrumentConfig&) const = default;
};

struct PersistedEngineConfig final {
  std::uint64_t max_total_active_orders{};

  bool operator==(const PersistedEngineConfig&) const = default;
};

struct LogHeader final {
  std::uint16_t format_version{command_log_format_version};
  std::uint16_t semantics_version{atlaslob_semantics_version};
  LogId log_id{};
  domain::Sequence first_sequence{command_log_first_sequence};
  PersistedEngineConfig engine_config{};
  std::vector<PersistedInstrumentConfig> catalog;
};

struct CommandRecord final {
  std::uint16_t record_version{command_log_record_version};
  domain::Command command{domain::NewOrder{}};
  domain::Sequence sequence{};
  RecordOutcome outcome{RecordOutcome::committed};
  domain::RejectReason rejection_reason{domain::RejectReason::none};
  std::uint64_t event_count{};
  Digest256 event_digest{};
};

}  // namespace atlaslob::persistence
