#include "atlaslob/matching_engine.hpp"

#include <array>
#include <memory>
#include <stdexcept>
#include <utility>

#include "atlaslob/multi_instrument_engine.hpp"

namespace atlaslob {

class MatchingEngine::Impl final {
 public:
  Impl(domain::InstrumentId instrument_id, const MatchingEngineConfig& config)
      : catalog_{InstrumentConfig{
            .instrument_id = instrument_id,
            .matching = config,
        }},
        engine_{catalog_} {}

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl() = default;

  std::array<InstrumentConfig, 1U> catalog_;
  MultiInstrumentEngine engine_;
};

MatchingEngine::MatchingEngine(domain::InstrumentId instrument_id, MatchingEngineConfig config) {
  if (!config.valid()) {
    throw std::invalid_argument{"MatchingEngine requires a valid configuration"};
  }
  impl_ = std::make_unique<Impl>(instrument_id, config);
}

MatchingEngine::~MatchingEngine() noexcept = default;

EngineResult MatchingEngine::execute(const domain::NewOrder& order) {
  return impl_->engine_.execute(order);
}

EngineResult MatchingEngine::execute(const domain::CancelOrder& order) {
  return impl_->engine_.execute(order);
}

EngineResult MatchingEngine::execute(const domain::ReplaceOrder& order) {
  return impl_->engine_.execute(order);
}

EngineResult MatchingEngine::execute(const domain::Command& command) {
  return impl_->engine_.execute(command);
}

domain::InstrumentId MatchingEngine::instrument_id() const noexcept {
  return impl_->catalog_[0].instrument_id;
}

std::size_t MatchingEngine::active_order_count() const noexcept {
  return impl_->engine_.active_order_count();
}

bool MatchingEngine::empty() const noexcept { return impl_->engine_.active_order_count() == 0U; }

BookTop MatchingEngine::top() const noexcept {
  const auto result = impl_->engine_.top(instrument_id());
  if (!result.has_value()) {
    std::terminate();
  }
  return *result;
}

BookSnapshot MatchingEngine::snapshot() const {
  const auto engine_snapshot = impl_->engine_.snapshot();
  if (engine_snapshot.instruments.size() != 1U) {
    std::terminate();
  }
  const auto& instrument = engine_snapshot.instruments[0];
  return {
      .semantics_version = engine_snapshot.semantics_version,
      .instrument_id = instrument.instrument_id,
      .last_sequence = engine_snapshot.last_sequence,
      .sequence_exhausted = engine_snapshot.sequence_exhausted,
      .active_order_count = instrument.active_order_count,
      .bids = instrument.bids,
      .asks = instrument.asks,
  };
}

Digest256 MatchingEngine::state_digest() const { return atlaslob::state_digest(snapshot()); }

domain::Sequence MatchingEngine::next_sequence() const noexcept {
  return impl_->engine_.next_sequence();
}

bool MatchingEngine::sequence_exhausted() const noexcept {
  return impl_->engine_.sequence_exhausted();
}

}  // namespace atlaslob
