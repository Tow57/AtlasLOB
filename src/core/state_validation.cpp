#include "state_validation.hpp"

#include <variant>

#include "atlaslob/domain/validation.hpp"

namespace atlaslob::core {
namespace {

[[nodiscard]] std::optional<domain::OrderId> nonzero(domain::OrderId order_id) noexcept {
  if (order_id.value() == 0U) {
    return std::nullopt;
  }
  return order_id;
}

[[nodiscard]] StateValidationResult reject(domain::RejectReason reason,
                                           domain::OrderId order_id) noexcept {
  return {
      .reason = reason,
      .relevant_order_id = nonzero(order_id),
      .internal_error = StateValidationError::none,
  };
}

[[nodiscard]] StateValidationResult invalid_policy() noexcept {
  return {
      .reason = domain::RejectReason::none,
      .relevant_order_id = std::nullopt,
      .internal_error = StateValidationError::invalid_policy,
  };
}

[[nodiscard]] StateValidationResult book_invariant_violation() noexcept {
  return {
      .reason = domain::RejectReason::none,
      .relevant_order_id = std::nullopt,
      .internal_error = StateValidationError::book_invariant_violation,
  };
}

[[nodiscard]] bool is_tick_aligned(domain::PriceTicks price,
                                   domain::PriceTicks tick_increment) noexcept {
  return price.value() % tick_increment.value() == 0;
}

}  // namespace

StateValidationResult validate_state(const domain::NewOrder& order, const InstrumentBook& book,
                                     const ExecutionPolicy& policy) noexcept {
  if (!policy.valid()) {
    return invalid_policy();
  }
  if (!book.validate_invariants()) {
    return book_invariant_violation();
  }
  const auto pure_validation = domain::validate(order);
  if (!pure_validation.accepted()) {
    return reject(pure_validation.reason, order.order_id);
  }
  if (order.instrument_id != book.instrument_id()) {
    return reject(domain::RejectReason::unknown_instrument, order.order_id);
  }
  if (order.quantity > policy.max_order_quantity) {
    return reject(domain::RejectReason::quantity_out_of_range, order.order_id);
  }
  if (order.order_type == domain::OrderType::limit &&
      !is_tick_aligned(*order.limit_price, policy.tick_increment)) {
    return reject(domain::RejectReason::invalid_tick, order.order_id);
  }
  if (book.find(order.order_id) != nullptr) {
    return reject(domain::RejectReason::duplicate_order_id, order.order_id);
  }
  return {};
}

StateValidationResult validate_state(const domain::CancelOrder& order, const InstrumentBook& book,
                                     const ExecutionPolicy& policy) noexcept {
  if (!policy.valid()) {
    return invalid_policy();
  }
  if (!book.validate_invariants()) {
    return book_invariant_violation();
  }
  const auto pure_validation = domain::validate(order);
  if (!pure_validation.accepted()) {
    return reject(pure_validation.reason, order.order_id);
  }
  if (order.instrument_id != book.instrument_id()) {
    return reject(domain::RejectReason::unknown_instrument, order.order_id);
  }

  const auto* const resting_order = book.find(order.order_id);
  if (resting_order == nullptr) {
    return reject(domain::RejectReason::unknown_order_id, order.order_id);
  }
  if (resting_order->client_id() != order.client_id) {
    return reject(domain::RejectReason::ownership_mismatch, order.order_id);
  }
  if (resting_order->instrument_id() != order.instrument_id) {
    return reject(domain::RejectReason::instrument_mismatch, order.order_id);
  }
  return {};
}

StateValidationResult validate_state(const domain::ReplaceOrder& order, const InstrumentBook& book,
                                     const ExecutionPolicy& policy) noexcept {
  if (!policy.valid()) {
    return invalid_policy();
  }
  if (!book.validate_invariants()) {
    return book_invariant_violation();
  }
  const auto pure_validation = domain::validate(order);
  if (!pure_validation.accepted()) {
    auto relevant_id = order.old_order_id;
    switch (pure_validation.reason) {
      case domain::RejectReason::invalid_order_id:
        if (order.old_order_id.value() != 0U) {
          relevant_id = order.new_order_id;
        }
        break;
      case domain::RejectReason::invalid_replacement_id:
      case domain::RejectReason::invalid_quantity:
      case domain::RejectReason::invalid_price:
        relevant_id = order.new_order_id;
        break;
      default:
        break;
    }
    return reject(pure_validation.reason, relevant_id);
  }
  if (order.instrument_id != book.instrument_id()) {
    return reject(domain::RejectReason::unknown_instrument, order.old_order_id);
  }
  if (order.new_quantity > policy.max_order_quantity) {
    return reject(domain::RejectReason::quantity_out_of_range, order.new_order_id);
  }
  if (!is_tick_aligned(order.new_limit_price, policy.tick_increment)) {
    return reject(domain::RejectReason::invalid_tick, order.new_order_id);
  }

  const auto* const resting_order = book.find(order.old_order_id);
  if (resting_order == nullptr) {
    return reject(domain::RejectReason::unknown_order_id, order.old_order_id);
  }
  if (resting_order->client_id() != order.client_id) {
    return reject(domain::RejectReason::ownership_mismatch, order.old_order_id);
  }
  if (resting_order->instrument_id() != order.instrument_id) {
    return reject(domain::RejectReason::instrument_mismatch, order.old_order_id);
  }
  if (book.find(order.new_order_id) != nullptr) {
    return reject(domain::RejectReason::invalid_replacement_id, order.new_order_id);
  }

  // Capacity depends on the final plan: full passive fills and the old replacement order free
  // slots, while only a GTC residual consumes one. The execution-preparation boundary evaluates
  // that final count before mutation.
  return {};
}

StateValidationResult validate_state(const domain::Command& command, const InstrumentBook& book,
                                     const ExecutionPolicy& policy) noexcept {
  return std::visit(
      [&book, &policy](const auto& value) noexcept { return validate_state(value, book, policy); },
      command);
}

}  // namespace atlaslob::core
