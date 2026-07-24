#include "atlaslob/matching_engine.hpp"

#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "command_executor.hpp"
#include "execution_policy.hpp"
#include "instrument_book.hpp"
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

domain::Sequence MatchingEngine::next_sequence() const noexcept {
  return impl_->executor_.next_sequence();
}

bool MatchingEngine::sequence_exhausted() const noexcept {
  return impl_->executor_.sequence_exhausted();
}

}  // namespace atlaslob
