#include "match_plan.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <utility>

#include "atlaslob/domain/validation.hpp"

namespace atlaslob::core {
namespace {

[[nodiscard]] bool crosses(const domain::NewOrder& order,
                           domain::PriceTicks resting_price) noexcept {
  if (order.order_type == domain::OrderType::market) {
    return true;
  }
  if (order.side == domain::Side::buy) {
    return order.limit_price.value() >= resting_price;
  }
  return order.limit_price.value() <= resting_price;
}

template <typename OppositeSide>
[[nodiscard]] std::size_t count_trades(const domain::NewOrder& order,
                                       const OppositeSide& opposite) noexcept {
  auto residual = order.quantity.value();
  std::size_t count = 0U;
  for (const PriceLevel& level : opposite) {
    if (!crosses(order, level.price())) {
      break;
    }
    for (const OrderNode* node = level.head(); node != nullptr && residual != 0U;
         node = node->next()) {
      residual -= std::min(residual, node->remaining_quantity().value());
      if (count == std::numeric_limits<std::size_t>::max()) {
        std::terminate();
      }
      ++count;
    }
    if (residual == 0U) {
      break;
    }
  }
  return count;
}

template <typename OppositeSide>
void populate_trades(const domain::NewOrder& order, const OppositeSide& opposite, MatchPlan& plan) {
  auto residual = order.quantity.value();
  for (const PriceLevel& level : opposite) {
    if (!crosses(order, level.price())) {
      break;
    }
    for (const OrderNode* node = level.head(); node != nullptr && residual != 0U;
         node = node->next()) {
      const auto resting_before = node->remaining_quantity().value();
      const auto execution = std::min(residual, resting_before);
      residual -= execution;
      plan.trades.push_back({
          .resting_order_id = node->order_id(),
          .resting_client_id = node->client_id(),
          .execution_price = node->price(),
          .execution_quantity = domain::Quantity{execution},
          .aggressor_remaining = domain::Quantity{residual},
          .resting_remaining_before = domain::Quantity{resting_before},
          .resting_remaining_after = domain::Quantity{resting_before - execution},
          .expected_resting_priority = node->priority_sequence(),
      });
    }
    if (residual == 0U) {
      break;
    }
  }
  plan.residual_quantity = domain::Quantity{residual};
}

[[nodiscard]] ResidualDisposition classify_residual(const domain::NewOrder& order,
                                                    domain::Quantity residual) noexcept {
  if (residual.value() == 0U) {
    return ResidualDisposition::filled;
  }
  if (order.order_type == domain::OrderType::market) {
    return ResidualDisposition::market_exhausted;
  }
  if (order.time_in_force == domain::TimeInForce::ioc) {
    return ResidualDisposition::ioc_canceled;
  }
  return ResidualDisposition::rest;
}

}  // namespace

MatchPlanResult plan_matches(const domain::NewOrder& order, const InstrumentBook& book) {
  const auto validation = domain::validate(order);
  if (!validation.accepted()) {
    return {
        .plan = {},
        .error = MatchPlanError::invalid_request,
        .validation_reason = validation.reason,
    };
  }
  if (order.instrument_id != book.instrument_id()) {
    return {
        .plan = {},
        .error = MatchPlanError::instrument_mismatch,
        .validation_reason = domain::RejectReason::unknown_instrument,
    };
  }
  if (!book.validate_invariants()) {
    return {
        .plan = {},
        .error = MatchPlanError::book_invariant_violation,
        .validation_reason = domain::RejectReason::none,
    };
  }
  if (book.find(order.order_id) != nullptr) {
    return {
        .plan = {},
        .error = MatchPlanError::duplicate_order_id,
        .validation_reason = domain::RejectReason::duplicate_order_id,
    };
  }

  MatchPlan plan{};
  const auto trade_count = order.side == domain::Side::buy ? count_trades(order, book.asks())
                                                           : count_trades(order, book.bids());
  plan.trades.reserve(trade_count);
  if (order.side == domain::Side::buy) {
    populate_trades(order, book.asks(), plan);
  } else {
    populate_trades(order, book.bids(), plan);
  }
  if (plan.trades.size() != trade_count) {
    std::terminate();
  }
  plan.residual_disposition = classify_residual(order, plan.residual_quantity);
  return {
      .plan = std::move(plan),
      .error = MatchPlanError::none,
      .validation_reason = domain::RejectReason::none,
  };
}

ActiveOrderProjection project_active_order_count(const MatchPlan& plan,
                                                 std::size_t current_active_order_count,
                                                 bool removes_existing_order) noexcept {
  const bool residual_is_zero = plan.residual_quantity.value() == 0U;
  bool residual_rests = false;
  switch (plan.residual_disposition) {
    case ResidualDisposition::rest:
      residual_rests = true;
      if (residual_is_zero) {
        return {.final_active_order_count = 0U, .error = ActiveOrderProjectionError::invalid_plan};
      }
      break;
    case ResidualDisposition::filled:
      if (!residual_is_zero) {
        return {.final_active_order_count = 0U, .error = ActiveOrderProjectionError::invalid_plan};
      }
      break;
    case ResidualDisposition::ioc_canceled:
    case ResidualDisposition::market_exhausted:
      if (residual_is_zero) {
        return {.final_active_order_count = 0U, .error = ActiveOrderProjectionError::invalid_plan};
      }
      break;
    default:
      return {.final_active_order_count = 0U, .error = ActiveOrderProjectionError::invalid_plan};
  }

  std::size_t terminal_passive_orders = 0U;
  for (const auto& trade : plan.trades) {
    if (trade.execution_quantity.value() == 0U ||
        trade.execution_quantity > trade.resting_remaining_before ||
        trade.resting_remaining_after.value() !=
            trade.resting_remaining_before.value() - trade.execution_quantity.value()) {
      return {.final_active_order_count = 0U, .error = ActiveOrderProjectionError::invalid_plan};
    }
    if (trade.resting_remaining_after.value() == 0U) {
      if (terminal_passive_orders == std::numeric_limits<std::size_t>::max()) {
        return {.final_active_order_count = 0U,
                .error = ActiveOrderProjectionError::count_overflow};
      }
      ++terminal_passive_orders;
    }
  }

  if (removes_existing_order) {
    if (terminal_passive_orders == std::numeric_limits<std::size_t>::max()) {
      return {.final_active_order_count = 0U, .error = ActiveOrderProjectionError::count_overflow};
    }
    ++terminal_passive_orders;
  }
  if (terminal_passive_orders > current_active_order_count) {
    return {.final_active_order_count = 0U, .error = ActiveOrderProjectionError::count_underflow};
  }

  auto final_count = current_active_order_count - terminal_passive_orders;
  if (residual_rests) {
    if (final_count == std::numeric_limits<std::size_t>::max()) {
      return {.final_active_order_count = 0U, .error = ActiveOrderProjectionError::count_overflow};
    }
    ++final_count;
  }
  return {
      .final_active_order_count = final_count,
      .error = ActiveOrderProjectionError::none,
  };
}

}  // namespace atlaslob::core
