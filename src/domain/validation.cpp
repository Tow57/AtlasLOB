#include "atlaslob/domain/validation.hpp"

namespace atlaslob::domain {

ValidationResult validate(const NewOrder& order) noexcept {
  if (order.client_id.value() == 0U) {
    return {RejectReason::invalid_client_id};
  }
  if (order.order_id.value() == 0U) {
    return {RejectReason::invalid_order_id};
  }
  if (order.instrument_id.value() == 0U) {
    return {RejectReason::invalid_instrument_id};
  }
  if (order.quantity.value() == 0U) {
    return {RejectReason::invalid_quantity};
  }
  if (!is_valid(order.side)) {
    return {RejectReason::invalid_side};
  }
  if (!is_valid(order.order_type)) {
    return {RejectReason::invalid_order_type};
  }
  if (!is_valid(order.time_in_force)) {
    return {RejectReason::invalid_time_in_force};
  }

  // FOK is part of the versioned domain vocabulary but intentionally deferred
  // until an all-or-none liquidity preflight can be implemented and verified.
  if (order.time_in_force == TimeInForce::fok) {
    return {RejectReason::unsupported_time_in_force};
  }

  switch (order.order_type) {
    case OrderType::limit:
      if (!order.limit_price.has_value()) {
        return {RejectReason::missing_limit_price};
      }
      if (order.limit_price->value() <= 0) {
        return {RejectReason::invalid_price};
      }
      return {};
    case OrderType::market:
      if (order.limit_price.has_value()) {
        return {RejectReason::unexpected_limit_price};
      }
      if (order.time_in_force != TimeInForce::ioc) {
        return {RejectReason::invalid_order_type_time_in_force};
      }
      return {};
  }
  return {RejectReason::invalid_order_type};
}

ValidationResult validate(const CancelOrder& order) noexcept {
  if (order.client_id.value() == 0U) {
    return {RejectReason::invalid_client_id};
  }
  if (order.order_id.value() == 0U) {
    return {RejectReason::invalid_order_id};
  }
  if (order.instrument_id.value() == 0U) {
    return {RejectReason::invalid_instrument_id};
  }
  return {};
}

ValidationResult validate(const ReplaceOrder& order) noexcept {
  if (order.client_id.value() == 0U) {
    return {RejectReason::invalid_client_id};
  }
  if (order.old_order_id.value() == 0U) {
    return {RejectReason::invalid_order_id};
  }
  if (order.new_order_id.value() == 0U) {
    return {RejectReason::invalid_order_id};
  }
  if (order.old_order_id == order.new_order_id) {
    return {RejectReason::invalid_replacement_id};
  }
  if (order.instrument_id.value() == 0U) {
    return {RejectReason::invalid_instrument_id};
  }
  if (order.new_quantity.value() == 0U) {
    return {RejectReason::invalid_quantity};
  }
  if (order.new_limit_price.value() <= 0) {
    return {RejectReason::invalid_price};
  }
  return {};
}

}  // namespace atlaslob::domain
