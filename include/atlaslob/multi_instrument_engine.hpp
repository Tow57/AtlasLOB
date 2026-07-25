#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "atlaslob/book_snapshot.hpp"
#include "atlaslob/digest.hpp"
#include "atlaslob/matching_engine.hpp"

namespace atlaslob {

namespace core {
class MultiInstrumentEngineAccess;
class PreparedMultiInstrumentCommand;
}  // namespace core

struct InstrumentConfig final {
  domain::InstrumentId instrument_id{};
  MatchingEngineConfig matching{};

  bool operator==(const InstrumentConfig&) const = default;
};

struct MultiInstrumentEngineConfig final {
  std::size_t max_total_active_orders{std::numeric_limits<std::size_t>::max()};

  bool operator==(const MultiInstrumentEngineConfig&) const = default;
};

struct InstrumentSnapshot final {
  domain::InstrumentId instrument_id{};
  std::uint64_t active_order_count{};
  std::vector<PriceLevelSnapshot> bids;
  std::vector<PriceLevelSnapshot> asks;

  bool operator==(const InstrumentSnapshot&) const = default;
};

struct EngineSnapshot final {
  std::uint16_t semantics_version{atlaslob_semantics_version};
  MultiInstrumentEngineConfig engine_config{};
  std::vector<InstrumentConfig> catalog;
  domain::Sequence last_sequence{};
  bool sequence_exhausted{};
  std::uint64_t active_order_count{};
  std::vector<InstrumentSnapshot> instruments;

  bool operator==(const EngineSnapshot&) const = default;
};

// Hashes ATLSME01, the documented canonical multi-engine snapshot encoding.
// This is deterministic correctness evidence, not authentication.
[[nodiscard]] Digest256 state_digest(const EngineSnapshot& snapshot) noexcept;

class MultiInstrumentEngine final {
 public:
  explicit MultiInstrumentEngine(std::span<const InstrumentConfig> catalog,
                                 MultiInstrumentEngineConfig config = {});
  ~MultiInstrumentEngine() noexcept;

  MultiInstrumentEngine(const MultiInstrumentEngine&) = delete;
  MultiInstrumentEngine& operator=(const MultiInstrumentEngine&) = delete;
  MultiInstrumentEngine(MultiInstrumentEngine&&) = delete;
  MultiInstrumentEngine& operator=(MultiInstrumentEngine&&) = delete;

  [[nodiscard]] EngineResult execute(const domain::NewOrder& order);
  [[nodiscard]] EngineResult execute(const domain::CancelOrder& order);
  [[nodiscard]] EngineResult execute(const domain::ReplaceOrder& order);
  [[nodiscard]] EngineResult execute(const domain::Command& command);

  [[nodiscard]] bool contains_instrument(domain::InstrumentId instrument_id) const noexcept;
  [[nodiscard]] std::size_t active_order_count() const noexcept;
  [[nodiscard]] std::optional<BookTop> top(domain::InstrumentId instrument_id) const noexcept;
  [[nodiscard]] std::optional<InstrumentSnapshot> snapshot(
      domain::InstrumentId instrument_id) const;
  [[nodiscard]] EngineSnapshot snapshot() const;
  [[nodiscard]] Digest256 state_digest() const;
  [[nodiscard]] domain::Sequence next_sequence() const noexcept;
  [[nodiscard]] bool sequence_exhausted() const noexcept;

#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
  using BeforeEventAllocationHook = void (*)();
  [[nodiscard]] bool validate_invariants_for_testing() const noexcept;
  void set_next_sequence_for_testing(domain::Sequence next_sequence);
  void set_before_event_allocation_hook_for_testing(domain::InstrumentId instrument_id,
                                                    BeforeEventAllocationHook hook);
  void erase_active_identity_for_testing(domain::OrderId order_id);
  void set_max_total_active_orders_for_testing(std::size_t max_total_active_orders) noexcept;
  void set_instrument_max_active_orders_for_testing(domain::InstrumentId instrument_id,
                                                    std::size_t max_active_orders);
  void set_sequence_state_for_testing(domain::Sequence next_sequence, bool exhausted) noexcept;
#endif

 private:
  friend class core::MultiInstrumentEngineAccess;
  friend class core::PreparedMultiInstrumentCommand;

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace atlaslob
