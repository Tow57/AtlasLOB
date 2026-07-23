#include <cstdlib>
#include <iostream>
#include <string_view>
#include <type_traits>

#include "atlaslob/domain/validation.hpp"

namespace {

using namespace atlaslob::domain;

int failure_count = 0;

void expect(bool condition, std::string_view description) {
  if (!condition) {
    std::cerr << "FAILED: " << description << '\n';
    ++failure_count;
  }
}

NewOrder valid_limit_order() {
  return {
      .order_id = OrderId{101U},
      .instrument_id = InstrumentId{7U},
      .side = Side::sell,
      .order_type = OrderType::limit,
      .time_in_force = TimeInForce::gtc,
      .limit_price = PriceTicks{10'250},
      .quantity = Quantity{25U},
  };
}

}  // namespace

int main() {
  static_assert(!std::is_convertible_v<OrderId, InstrumentId>);
  static_assert(!std::is_convertible_v<PriceTicks, Quantity>);

  auto order = valid_limit_order();
  expect(validate(order).accepted(), "a valid limit GTC order is accepted");

  order = valid_limit_order();
  order.time_in_force = TimeInForce::ioc;
  expect(validate(order).accepted(), "a valid limit IOC order is accepted");

  order = valid_limit_order();
  order.order_type = OrderType::market;
  order.time_in_force = TimeInForce::ioc;
  order.limit_price.reset();
  expect(validate(order).accepted(), "a valid market IOC order is accepted");

  order = valid_limit_order();
  order.order_id = OrderId{0U};
  expect(validate(order).reason == RejectReason::invalid_order_id,
         "zero order IDs are rejected first");

  order = valid_limit_order();
  order.order_id = OrderId{0U};
  order.quantity = Quantity{0U};
  expect(validate(order).reason == RejectReason::invalid_order_id,
         "validation precedence is deterministic when several fields are invalid");

  order = valid_limit_order();
  order.instrument_id = InstrumentId{0U};
  expect(validate(order).reason == RejectReason::invalid_instrument_id,
         "zero instrument IDs are rejected");

  order = valid_limit_order();
  order.quantity = Quantity{0U};
  expect(validate(order).reason == RejectReason::invalid_quantity, "zero quantity is rejected");

  order = valid_limit_order();
  order.side = static_cast<Side>(255U);
  expect(validate(order).reason == RejectReason::invalid_side, "unknown side values are rejected");

  order = valid_limit_order();
  order.order_type = static_cast<OrderType>(255U);
  expect(validate(order).reason == RejectReason::invalid_order_type,
         "unknown order type values are rejected");

  order = valid_limit_order();
  order.time_in_force = static_cast<TimeInForce>(255U);
  expect(validate(order).reason == RejectReason::invalid_time_in_force,
         "unknown time-in-force values are rejected");

  order = valid_limit_order();
  order.limit_price.reset();
  expect(validate(order).reason == RejectReason::missing_limit_price,
         "limit orders require a price");

  order = valid_limit_order();
  order.limit_price = PriceTicks{0};
  expect(validate(order).reason == RejectReason::invalid_price,
         "limit prices must be positive integer ticks");

  order = valid_limit_order();
  order.order_type = OrderType::market;
  order.time_in_force = TimeInForce::ioc;
  expect(validate(order).reason == RejectReason::unexpected_limit_price,
         "market orders must not carry a limit price");

  order = valid_limit_order();
  order.order_type = OrderType::market;
  order.time_in_force = TimeInForce::gtc;
  order.limit_price.reset();
  expect(validate(order).reason == RejectReason::invalid_order_type_time_in_force,
         "market GTC is an invalid combination");

  order = valid_limit_order();
  order.time_in_force = TimeInForce::fok;
  expect(validate(order).reason == RejectReason::unsupported_time_in_force,
         "FOK is explicitly unsupported in the current release");

  if (failure_count != 0) {
    std::cerr << failure_count << " test(s) failed\n";
    return EXIT_FAILURE;
  }

  std::cout << "All domain validation tests passed\n";
  return EXIT_SUCCESS;
}
