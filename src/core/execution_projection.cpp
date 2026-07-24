#include "execution_projection.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

#include "atlaslob/domain/validation.hpp"
#include "checked_arithmetic.hpp"

namespace atlaslob::core {
namespace {

[[nodiscard]] ExecutionProjectionResult fail(ExecutionProjectionError error) noexcept {
  return {.snapshot = {}, .error = error};
}

[[nodiscard]] bool valid_plan_shape(const MatchPlan& plan) noexcept {
  const bool has_residual = plan.residual_quantity.value() != 0U;
  switch (plan.residual_disposition) {
    case ResidualDisposition::rest:
      if (!has_residual) {
        return false;
      }
      break;
    case ResidualDisposition::filled:
      if (has_residual || plan.trades.empty()) {
        return false;
      }
      break;
    case ResidualDisposition::ioc_canceled:
    case ResidualDisposition::market_exhausted:
      if (!has_residual) {
        return false;
      }
      break;
    default:
      return false;
  }

  std::optional<std::uint64_t> previous_aggressor_remaining;
  for (const auto& trade : plan.trades) {
    const auto execution = trade.execution_quantity.value();
    const auto resting_before = trade.resting_remaining_before.value();
    const auto resting_after = trade.resting_remaining_after.value();
    const auto aggressor_after = trade.aggressor_remaining.value();
    if (trade.resting_order_id.value() == 0U || trade.resting_client_id.value() == 0U ||
        trade.execution_price.value() <= 0 || execution == 0U || resting_before == 0U ||
        trade.expected_resting_priority.value() == 0U || execution > resting_before ||
        resting_after != resting_before - execution ||
        (resting_after != 0U && aggressor_after != 0U)) {
      return false;
    }

    std::uint64_t aggressor_before{};
    if (!detail::checked_add(execution, aggressor_after, aggressor_before)) {
      return false;
    }
    if (previous_aggressor_remaining.has_value() &&
        *previous_aggressor_remaining != aggressor_before) {
      return false;
    }
    previous_aggressor_remaining = aggressor_after;
  }

  return !previous_aggressor_remaining.has_value() ||
         *previous_aggressor_remaining == plan.residual_quantity.value();
}

[[nodiscard]] bool valid_resting_residual(
    const MatchPlan& plan,
    const std::optional<ProjectedRestingResidual>& resting_residual) noexcept {
  if (plan.residual_disposition != ResidualDisposition::rest) {
    return !resting_residual.has_value();
  }
  return resting_residual.has_value() && resting_residual->price.value() > 0 &&
         resting_residual->quantity == plan.residual_quantity;
}

[[nodiscard]] bool crosses(const domain::NewOrder& order,
                           domain::PriceTicks resting_price) noexcept {
  if (order.order_type == domain::OrderType::market) {
    return true;
  }
  if (!order.limit_price.has_value()) {
    return false;
  }
  return order.side == domain::Side::buy ? *order.limit_price >= resting_price
                                         : *order.limit_price <= resting_price;
}

[[nodiscard]] bool valid_plan_for_order(const domain::NewOrder& order,
                                        const MatchPlan& plan) noexcept {
  if (!valid_plan_shape(plan)) {
    return false;
  }

  std::uint64_t planned_initial_quantity = plan.residual_quantity.value();
  if (!plan.trades.empty()) {
    if (!detail::checked_add(plan.trades.front().execution_quantity.value(),
                             plan.trades.front().aggressor_remaining.value(),
                             planned_initial_quantity)) {
      return false;
    }
  }
  if (planned_initial_quantity != order.quantity.value()) {
    return false;
  }
  for (const auto& trade : plan.trades) {
    if (!crosses(order, trade.execution_price)) {
      return false;
    }
  }

  const auto expected_disposition = plan.residual_quantity.value() == 0U
                                        ? ResidualDisposition::filled
                                        : ResidualDisposition::rest;
  return plan.residual_disposition == expected_disposition;
}

[[nodiscard]] bool valid_replace_order(const InstrumentBook& book,
                                       const CancelProjectionTarget& old_target,
                                       const domain::NewOrder& replacement_order) noexcept {
  const auto* const old_order = book.find(old_target.order_id);
  return old_order != nullptr && domain::validate(replacement_order).accepted() &&
         replacement_order.instrument_id == book.instrument_id() &&
         replacement_order.order_type == domain::OrderType::limit &&
         replacement_order.time_in_force == domain::TimeInForce::gtc &&
         replacement_order.limit_price.has_value() &&
         replacement_order.client_id == old_order->client_id() &&
         replacement_order.side == old_target.side &&
         replacement_order.order_id != old_target.order_id &&
         book.find(replacement_order.order_id) == nullptr;
}

[[nodiscard]] bool matches_cancel_target(const InstrumentBook& book,
                                         const CancelProjectionTarget& target) noexcept {
  if (target.order_id.value() == 0U || target.price.value() <= 0 ||
      target.remaining_quantity.value() == 0U || target.priority_sequence.value() == 0U) {
    return false;
  }
  const auto* node = book.find(target.order_id);
  return node != nullptr && node->side() == target.side && node->price() == target.price &&
         node->remaining_quantity() == target.remaining_quantity &&
         node->priority_sequence() == target.priority_sequence;
}

[[nodiscard]] bool matches_trade(const PlannedTrade& trade, const PriceLevel& level,
                                 const OrderNode& node) noexcept {
  return trade.resting_order_id == node.order_id() && trade.resting_client_id == node.client_id() &&
         trade.execution_price == level.price() && trade.execution_price == node.price() &&
         trade.resting_remaining_before == node.remaining_quantity() &&
         trade.expected_resting_priority == node.priority_sequence();
}

template <domain::Side RestingSide>
[[nodiscard]] ExecutionProjectionError project_side_after_trades(
    const BookSide<RestingSide>& side, const MatchPlan& plan,
    std::optional<domain::TopOfBookLevel>& projected_best) noexcept {
  std::size_t trade_index = 0U;
  for (const PriceLevel& level : side) {
    std::uint64_t level_reduction = 0U;
    for (const OrderNode* node = level.head(); node != nullptr; node = node->next()) {
      if (trade_index == plan.trades.size()) {
        std::uint64_t aggregate_after{};
        if (!detail::checked_subtract(level.aggregate_quantity().value(), level_reduction,
                                      aggregate_after)) {
          return ExecutionProjectionError::aggregate_underflow;
        }
        if (aggregate_after == 0U) {
          return ExecutionProjectionError::plan_book_mismatch;
        }
        projected_best = domain::TopOfBookLevel{
            .price = level.price(),
            .aggregate_quantity = domain::Quantity{aggregate_after},
        };
        return ExecutionProjectionError::none;
      }

      const auto& trade = plan.trades[trade_index];
      if (!matches_trade(trade, level, *node)) {
        return ExecutionProjectionError::plan_book_mismatch;
      }
      if (!detail::checked_add(level_reduction, trade.execution_quantity.value(),
                               level_reduction)) {
        return ExecutionProjectionError::aggregate_overflow;
      }
      if (level_reduction > level.aggregate_quantity().value()) {
        return ExecutionProjectionError::aggregate_underflow;
      }
      ++trade_index;

      if (trade.resting_remaining_after.value() != 0U) {
        if (trade_index != plan.trades.size()) {
          return ExecutionProjectionError::invalid_plan;
        }
        std::uint64_t aggregate_after{};
        if (!detail::checked_subtract(level.aggregate_quantity().value(), level_reduction,
                                      aggregate_after)) {
          return ExecutionProjectionError::aggregate_underflow;
        }
        if (aggregate_after == 0U) {
          return ExecutionProjectionError::plan_book_mismatch;
        }
        projected_best = domain::TopOfBookLevel{
            .price = level.price(),
            .aggregate_quantity = domain::Quantity{aggregate_after},
        };
        return ExecutionProjectionError::none;
      }
    }

    std::uint64_t aggregate_after{};
    if (!detail::checked_subtract(level.aggregate_quantity().value(), level_reduction,
                                  aggregate_after)) {
      return ExecutionProjectionError::aggregate_underflow;
    }
    if (aggregate_after != 0U) {
      return ExecutionProjectionError::plan_book_mismatch;
    }
  }

  if (trade_index != plan.trades.size()) {
    return ExecutionProjectionError::plan_book_mismatch;
  }
  projected_best.reset();
  return ExecutionProjectionError::none;
}

template <domain::Side RestingSide>
[[nodiscard]] ExecutionProjectionError apply_resting_residual(
    const BookSide<RestingSide>& side, const ProjectedRestingResidual& residual,
    std::optional<domain::TopOfBookLevel>& projected_best) noexcept {
  std::uint64_t projected_aggregate = residual.quantity.value();
  if (const auto* existing = side.find_level(residual.price); existing != nullptr) {
    if (!detail::checked_add(existing->aggregate_quantity().value(), residual.quantity.value(),
                             projected_aggregate)) {
      return ExecutionProjectionError::aggregate_overflow;
    }
  }

  if (!projected_best.has_value() ||
      detail::BestPriceFirst<RestingSide>{}(residual.price, projected_best->price) ||
      residual.price == projected_best->price) {
    projected_best = domain::TopOfBookLevel{
        .price = residual.price,
        .aggregate_quantity = domain::Quantity{projected_aggregate},
    };
  }
  return ExecutionProjectionError::none;
}

template <domain::Side RestingSide>
[[nodiscard]] ExecutionProjectionError project_side_after_replace(
    const BookSide<RestingSide>& side, const CancelProjectionTarget& old_target,
    const std::optional<ProjectedRestingResidual>& residual,
    std::optional<domain::TopOfBookLevel>& projected_best) noexcept {
  const auto* old_level = side.find_level(old_target.price);
  if (old_level == nullptr) {
    return ExecutionProjectionError::cancel_target_mismatch;
  }

  std::uint64_t old_level_after{};
  if (!detail::checked_subtract(old_level->aggregate_quantity().value(),
                                old_target.remaining_quantity.value(), old_level_after)) {
    return ExecutionProjectionError::aggregate_underflow;
  }

  std::uint64_t residual_level_after{};
  const PriceLevel* residual_level = nullptr;
  if (residual.has_value()) {
    residual_level = side.find_level(residual->price);
    const auto aggregate_before =
        residual->price == old_target.price
            ? old_level_after
            : (residual_level == nullptr ? 0U : residual_level->aggregate_quantity().value());
    if (!detail::checked_add(aggregate_before, residual->quantity.value(), residual_level_after)) {
      return ExecutionProjectionError::aggregate_overflow;
    }
  }

  projected_best.reset();
  for (const PriceLevel& level : side) {
    auto aggregate_after = level.aggregate_quantity().value();
    if (level.price() == old_target.price) {
      aggregate_after = old_level_after;
    }
    if (residual.has_value() && level.price() == residual->price) {
      aggregate_after = residual_level_after;
    }
    if (aggregate_after != 0U) {
      projected_best = domain::TopOfBookLevel{
          .price = level.price(),
          .aggregate_quantity = domain::Quantity{aggregate_after},
      };
      break;
    }
  }

  if (residual.has_value() && residual_level == nullptr &&
      (!projected_best.has_value() ||
       detail::BestPriceFirst<RestingSide>{}(residual->price, projected_best->price))) {
    projected_best = domain::TopOfBookLevel{
        .price = residual->price,
        .aggregate_quantity = domain::Quantity{residual_level_after},
    };
  }
  return ExecutionProjectionError::none;
}

[[nodiscard]] bool is_crossed(const TopOfBookSnapshot& snapshot) noexcept {
  return snapshot.best_bid.has_value() && snapshot.best_ask.has_value() &&
         snapshot.best_bid->price >= snapshot.best_ask->price;
}

template <domain::Side RestingSide>
[[nodiscard]] ExecutionProjectionError project_side_after_cancel(
    const BookSide<RestingSide>& side, const CancelProjectionTarget& target,
    std::optional<domain::TopOfBookLevel>& projected_best) noexcept {
  const auto* level = side.find_level(target.price);
  if (level == nullptr) {
    return ExecutionProjectionError::cancel_target_mismatch;
  }

  std::uint64_t aggregate_after{};
  if (!detail::checked_subtract(level->aggregate_quantity().value(),
                                target.remaining_quantity.value(), aggregate_after)) {
    return ExecutionProjectionError::aggregate_underflow;
  }
  if (!projected_best.has_value() || projected_best->price != target.price) {
    return ExecutionProjectionError::none;
  }
  if (aggregate_after != 0U) {
    projected_best->aggregate_quantity = domain::Quantity{aggregate_after};
    return ExecutionProjectionError::none;
  }

  auto position = side.begin();
  if (position == side.end() || position->price() != target.price) {
    return ExecutionProjectionError::cancel_target_mismatch;
  }
  ++position;
  if (position == side.end()) {
    projected_best.reset();
  } else {
    projected_best = domain::TopOfBookLevel{
        .price = position->price(),
        .aggregate_quantity = position->aggregate_quantity(),
    };
  }
  return ExecutionProjectionError::none;
}

}  // namespace

ExecutionProjectionResult project_new_top_of_book(
    const InstrumentBook& book, domain::Side aggressor_side, const MatchPlan& plan,
    std::optional<ProjectedRestingResidual> resting_residual) noexcept {
  if (!domain::is_valid(aggressor_side)) {
    return fail(ExecutionProjectionError::invalid_side);
  }
  if (!book.validate_invariants()) {
    return fail(ExecutionProjectionError::book_invariant_violation);
  }
  if (!valid_plan_shape(plan)) {
    return fail(ExecutionProjectionError::invalid_plan);
  }
  if (!valid_resting_residual(plan, resting_residual)) {
    return fail(ExecutionProjectionError::invalid_resting_residual);
  }

  auto snapshot = snapshot_top_of_book(book);
  ExecutionProjectionError error{};
  if (aggressor_side == domain::Side::buy) {
    error = project_side_after_trades(book.asks(), plan, snapshot.best_ask);
    if (error == ExecutionProjectionError::none && resting_residual.has_value()) {
      error = apply_resting_residual(book.bids(), *resting_residual, snapshot.best_bid);
    }
  } else {
    error = project_side_after_trades(book.bids(), plan, snapshot.best_bid);
    if (error == ExecutionProjectionError::none && resting_residual.has_value()) {
      error = apply_resting_residual(book.asks(), *resting_residual, snapshot.best_ask);
    }
  }
  if (error != ExecutionProjectionError::none) {
    return fail(error);
  }
  if (is_crossed(snapshot)) {
    return fail(ExecutionProjectionError::crossed_book);
  }
  return {.snapshot = snapshot, .error = ExecutionProjectionError::none};
}

ExecutionProjectionResult project_cancel_top_of_book(
    const InstrumentBook& book, const CancelProjectionTarget& target) noexcept {
  if (!domain::is_valid(target.side)) {
    return fail(ExecutionProjectionError::invalid_side);
  }
  if (!book.validate_invariants()) {
    return fail(ExecutionProjectionError::book_invariant_violation);
  }
  if (!matches_cancel_target(book, target)) {
    return fail(ExecutionProjectionError::cancel_target_mismatch);
  }

  auto snapshot = snapshot_top_of_book(book);
  const auto error = target.side == domain::Side::buy
                         ? project_side_after_cancel(book.bids(), target, snapshot.best_bid)
                         : project_side_after_cancel(book.asks(), target, snapshot.best_ask);
  if (error != ExecutionProjectionError::none) {
    return fail(error);
  }
  return {.snapshot = snapshot, .error = ExecutionProjectionError::none};
}

ExecutionProjectionResult project_replace_top_of_book(
    const InstrumentBook& book, const CancelProjectionTarget& old_target,
    const domain::NewOrder& replacement_order, const MatchPlan& plan,
    std::optional<ProjectedRestingResidual> resting_residual) noexcept {
  if (!domain::is_valid(old_target.side) || !domain::is_valid(replacement_order.side)) {
    return fail(ExecutionProjectionError::invalid_side);
  }
  if (!book.validate_invariants()) {
    return fail(ExecutionProjectionError::book_invariant_violation);
  }
  if (!matches_cancel_target(book, old_target)) {
    return fail(ExecutionProjectionError::cancel_target_mismatch);
  }
  if (!valid_replace_order(book, old_target, replacement_order)) {
    return fail(ExecutionProjectionError::replacement_order_mismatch);
  }
  if (!valid_plan_for_order(replacement_order, plan)) {
    return fail(ExecutionProjectionError::invalid_plan);
  }
  if (!valid_resting_residual(plan, resting_residual) ||
      (resting_residual.has_value() &&
       resting_residual->price != replacement_order.limit_price.value())) {
    return fail(ExecutionProjectionError::invalid_resting_residual);
  }

  auto snapshot = snapshot_top_of_book(book);
  ExecutionProjectionError opposite_error{};
  ExecutionProjectionError same_side_error{};
  if (replacement_order.side == domain::Side::buy) {
    opposite_error = project_side_after_trades(book.asks(), plan, snapshot.best_ask);
    same_side_error =
        project_side_after_replace(book.bids(), old_target, resting_residual, snapshot.best_bid);
  } else {
    opposite_error = project_side_after_trades(book.bids(), plan, snapshot.best_bid);
    same_side_error =
        project_side_after_replace(book.asks(), old_target, resting_residual, snapshot.best_ask);
  }
  if (opposite_error != ExecutionProjectionError::none) {
    return fail(opposite_error);
  }
  if (same_side_error != ExecutionProjectionError::none) {
    return fail(same_side_error);
  }
  if (is_crossed(snapshot)) {
    return fail(ExecutionProjectionError::crossed_book);
  }
  return {.snapshot = snapshot, .error = ExecutionProjectionError::none};
}

}  // namespace atlaslob::core
