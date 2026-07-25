#pragma once

#include <memory>

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
  MultiInstrumentEngineAccess() = delete;

  [[nodiscard]] static PreparedMultiInstrumentCommand prepare(MultiInstrumentEngine& engine,
                                                              const domain::Command& command);
  [[nodiscard]] static bool validate_invariants(const MultiInstrumentEngine& engine) noexcept;

  // Private deterministic seam used only by persistence boundary tests. It is
  // intentionally absent from the installed public API.
  static void set_next_sequence_for_testing(MultiInstrumentEngine& engine,
                                            domain::Sequence next_sequence);
};

}  // namespace atlaslob::core
