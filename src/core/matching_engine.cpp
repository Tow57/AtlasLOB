#include "atlaslob/matching_engine.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "command_executor.hpp"
#include "execution_policy.hpp"
#include "instrument_book.hpp"
#include "snapshot_sequence.hpp"
#include "top_of_book.hpp"

namespace atlaslob {
namespace {

[[nodiscard]] core::ExecutionPolicy make_execution_policy(
    const MatchingEngineConfig& config) noexcept {
  return {
      .max_order_quantity = config.max_order_quantity,
      .tick_increment = config.tick_increment,
      .max_active_orders = config.max_active_orders,
  };
}

[[nodiscard]] EngineResult translate(core::CommandExecutionResult result) {
  EngineResult translated;
  if (result) {
    translated.batch.emplace(std::move(*result.batch));
    return translated;
  }

  translated.error = result.admission_error == core::CommandAdmissionError::sequence_exhausted
                         ? EngineError::sequence_exhausted
                         : EngineError::internal_failure;
  return translated;
}

template <domain::Side RestingSide>
[[nodiscard]] std::vector<PriceLevelSnapshot> snapshot_side(
    const core::BookSide<RestingSide>& side) {
  std::vector<PriceLevelSnapshot> levels;
  levels.reserve(side.level_count());
  for (const core::PriceLevel& level : side) {
    PriceLevelSnapshot level_snapshot{
        .price = level.price(),
        .aggregate_quantity = level.aggregate_quantity(),
        .orders = {},
    };
    level_snapshot.orders.reserve(level.order_count());
    for (const core::OrderNode* node = level.head(); node != nullptr; node = node->next()) {
      level_snapshot.orders.push_back({
          .order_id = node->order_id(),
          .client_id = node->client_id(),
          .instrument_id = node->instrument_id(),
          .side = node->side(),
          .price = node->price(),
          .remaining_quantity = node->remaining_quantity(),
          .priority_sequence = node->priority_sequence(),
      });
    }
    levels.push_back(std::move(level_snapshot));
  }
  return levels;
}

[[nodiscard]] domain::Sequence last_issued_sequence(
    const core::CommandExecutor& executor) noexcept {
  const auto next = executor.next_sequence();
  const auto exhausted = executor.sequence_exhausted();
  if (!exhausted && next.value() == 0U) {
    std::terminate();
  }
  return core::snapshot_last_sequence(next, exhausted);
}

}  // namespace

class MatchingEngine::Impl final {
 public:
  Impl(domain::InstrumentId instrument_id, const MatchingEngineConfig& config)
      : book_{instrument_id}, executor_{book_, make_execution_policy(config)} {}

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl() = default;

  core::InstrumentBook book_;
  core::CommandExecutor executor_;
};

MatchingEngine::MatchingEngine(domain::InstrumentId instrument_id, MatchingEngineConfig config) {
  if (!config.valid()) {
    throw std::invalid_argument{"MatchingEngine requires a valid configuration"};
  }
  impl_ = std::make_unique<Impl>(instrument_id, config);
}

MatchingEngine::~MatchingEngine() noexcept = default;

EngineResult MatchingEngine::execute(const domain::NewOrder& order) {
  return translate(impl_->executor_.execute(order));
}

EngineResult MatchingEngine::execute(const domain::CancelOrder& order) {
  return translate(impl_->executor_.execute(order));
}

EngineResult MatchingEngine::execute(const domain::ReplaceOrder& order) {
  return translate(impl_->executor_.execute(order));
}

EngineResult MatchingEngine::execute(const domain::Command& command) {
  return std::visit(
      [this](const auto& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, domain::NewOrder>) {
          return execute(value);
        } else if constexpr (std::is_same_v<Value, domain::CancelOrder>) {
          return execute(value);
        } else {
          static_assert(std::is_same_v<Value, domain::ReplaceOrder>);
          return execute(value);
        }
      },
      command);
}

domain::InstrumentId MatchingEngine::instrument_id() const noexcept {
  return impl_->book_.instrument_id();
}

std::size_t MatchingEngine::active_order_count() const noexcept {
  return impl_->book_.active_order_count();
}

bool MatchingEngine::empty() const noexcept { return impl_->book_.empty(); }

BookTop MatchingEngine::top() const noexcept {
  const auto snapshot = core::snapshot_top_of_book(impl_->book_);
  return {
      .best_bid = snapshot.best_bid,
      .best_ask = snapshot.best_ask,
  };
}

BookSnapshot MatchingEngine::snapshot() const {
  if (!impl_->book_.validate_invariants() || impl_->book_.has_pending_preparation()) {
    std::terminate();
  }
  return {
      .semantics_version = atlaslob_semantics_version,
      .instrument_id = impl_->book_.instrument_id(),
      .last_sequence = last_issued_sequence(impl_->executor_),
      .sequence_exhausted = impl_->executor_.sequence_exhausted(),
      .active_order_count = static_cast<std::uint64_t>(impl_->book_.active_order_count()),
      .bids = snapshot_side(impl_->book_.bids()),
      .asks = snapshot_side(impl_->book_.asks()),
  };
}

Digest256 MatchingEngine::state_digest() const { return atlaslob::state_digest(snapshot()); }

domain::Sequence MatchingEngine::next_sequence() const noexcept {
  return impl_->executor_.next_sequence();
}

bool MatchingEngine::sequence_exhausted() const noexcept {
  return impl_->executor_.sequence_exhausted();
}

}  // namespace atlaslob
