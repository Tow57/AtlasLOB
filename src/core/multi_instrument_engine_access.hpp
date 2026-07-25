#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "atlaslob/domain/commands.hpp"
#include "atlaslob/domain/event_batch.hpp"
#include "atlaslob/matching_engine.hpp"
#include "atlaslob/multi_instrument_engine.hpp"

namespace atlaslob::core {

// Private cross-target seam used by the persistence layer. It is deliberately
// absent from AtlasLOB's installed public header set: callers cannot supply a
// sequence or prepare overlapping commands.
class PreparedMultiInstrumentCommand final {
 public:
  class Impl;

  PreparedMultiInstrumentCommand(const PreparedMultiInstrumentCommand&) = delete;
  PreparedMultiInstrumentCommand& operator=(const PreparedMultiInstrumentCommand&) = delete;
  PreparedMultiInstrumentCommand(PreparedMultiInstrumentCommand&& other) noexcept;
  PreparedMultiInstrumentCommand& operator=(PreparedMultiInstrumentCommand&& other) noexcept;
  ~PreparedMultiInstrumentCommand() noexcept;

  [[nodiscard]] const domain::Command& command() const noexcept;
  [[nodiscard]] const domain::EventBatch* batch() const noexcept;
  [[nodiscard]] EngineError error() const noexcept;
  [[nodiscard]] bool has_value() const noexcept;
  [[nodiscard]] bool rejected() const noexcept;
  [[nodiscard]] bool committed() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  // One-shot, allocation-free publication of the already prepared book,
  // identity, sequence, and result state.
  [[nodiscard]] EngineResult commit() noexcept;

 private:
  friend class MultiInstrumentEngineAccess;

  explicit PreparedMultiInstrumentCommand(std::unique_ptr<Impl> implementation) noexcept;

  std::unique_ptr<Impl> impl_;
};

class MultiInstrumentEngineAccess final {
 public:
  enum class RestoreAllocationStage : std::uint8_t {
    engine = 1,
    global_directory_reserve = 2,
    global_priority_directory_reserve = 3,
    book_storage_reserve = 4,
    book_index_reserve = 5,
    price_level = 6,
    order_storage = 7,
    order_index = 8,
    global_identity = 9,
    global_priority_identity = 10,
  };

  using RestoreAllocationHook = void (*)(RestoreAllocationStage);

  MultiInstrumentEngineAccess() = delete;

  [[nodiscard]] static PreparedMultiInstrumentCommand prepare(MultiInstrumentEngine& engine,
                                                              const domain::Command& command);
  [[nodiscard]] static bool validate_invariants(const MultiInstrumentEngine& engine) noexcept;

  // Reconstructs an unpublished engine from an already decoded snapshot.
  // Structural validation, allocation, intrusive-link/index reconstruction,
  // invariant validation, and the optional digest check all complete before
  // ownership is returned. Malformed snapshots throw std::invalid_argument;
  // allocation failures propagate unchanged.
  [[nodiscard]] static std::unique_ptr<MultiInstrumentEngine> restore_snapshot(
      const EngineSnapshot& snapshot, std::optional<Digest256> expected_digest = std::nullopt);

  // Private deterministic fault-injection seam used only by restoration tests.
  // The hook is invoked before each allocation-capable reconstruction step.
  static void set_restore_allocation_hook_for_testing(RestoreAllocationHook hook) noexcept;
  static void set_order_priority_for_testing(MultiInstrumentEngine& engine,
                                             domain::OrderId order_id, domain::Sequence priority);

  // Private deterministic seam used only by persistence boundary tests. It is
  // intentionally absent from the installed public API.
  static void set_next_sequence_for_testing(MultiInstrumentEngine& engine,
                                            domain::Sequence next_sequence);
};

}  // namespace atlaslob::core
