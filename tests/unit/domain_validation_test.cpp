#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>
#include <unordered_map>

#include "atlaslob/domain/validation.hpp"

namespace {

using namespace atlaslob::domain;

static_assert(!std::is_convertible_v<OrderId, InstrumentId>);
static_assert(!std::is_convertible_v<ClientId, InstrumentId>);
static_assert(!std::is_convertible_v<PriceTicks, Quantity>);
static_assert(std::is_same_v<std::underlying_type_t<RejectReason>, std::uint16_t>);
static_assert(static_cast<std::uint16_t>(RejectReason::none) == 0U);
static_assert(static_cast<std::uint16_t>(RejectReason::invalid_order_id) == 1U);
static_assert(static_cast<std::uint16_t>(RejectReason::invalid_instrument_id) == 2U);
static_assert(static_cast<std::uint16_t>(RejectReason::invalid_quantity) == 3U);
static_assert(static_cast<std::uint16_t>(RejectReason::invalid_side) == 4U);
static_assert(static_cast<std::uint16_t>(RejectReason::invalid_order_type) == 5U);
static_assert(static_cast<std::uint16_t>(RejectReason::invalid_time_in_force) == 6U);
static_assert(static_cast<std::uint16_t>(RejectReason::missing_limit_price) == 7U);
static_assert(static_cast<std::uint16_t>(RejectReason::unexpected_limit_price) == 8U);
static_assert(static_cast<std::uint16_t>(RejectReason::invalid_price) == 9U);
static_assert(static_cast<std::uint16_t>(RejectReason::invalid_order_type_time_in_force) == 10U);
static_assert(static_cast<std::uint16_t>(RejectReason::unsupported_time_in_force) == 11U);
static_assert(static_cast<std::uint16_t>(RejectReason::invalid_client_id) == 12U);
static_assert(static_cast<std::uint16_t>(RejectReason::capacity_exceeded) == 21U);

NewOrder valid_limit_order() {
  return {
      .client_id = ClientId{17U},
      .order_id = OrderId{101U},
      .instrument_id = InstrumentId{7U},
      .side = Side::sell,
      .order_type = OrderType::limit,
      .time_in_force = TimeInForce::gtc,
      .limit_price = PriceTicks{10'250},
      .quantity = Quantity{25U},
  };
}

CancelOrder valid_cancel_order() {
  return {
      .client_id = ClientId{17U},
      .order_id = OrderId{101U},
      .instrument_id = InstrumentId{7U},
  };
}

ReplaceOrder valid_replace_order() {
  return {
      .client_id = ClientId{17U},
      .old_order_id = OrderId{101U},
      .new_order_id = OrderId{102U},
      .instrument_id = InstrumentId{7U},
      .new_limit_price = PriceTicks{10'300},
      .new_quantity = Quantity{30U},
  };
}

TEST(StrongValues, HashByUnderlyingRepresentation) {
  std::unordered_map<OrderId, int, StrongValueHash<OrderId>> values;
  values.emplace(OrderId{42U}, 9);

  ASSERT_TRUE(values.contains(OrderId{42U}));
  EXPECT_EQ(values.at(OrderId{42U}), 9);
}

TEST(DomainEnums, ZeroAndUnknownValuesAreInvalid) {
  EXPECT_FALSE(is_valid(static_cast<Side>(0U)));
  EXPECT_FALSE(is_valid(static_cast<OrderType>(0U)));
  EXPECT_FALSE(is_valid(static_cast<TimeInForce>(0U)));
  EXPECT_FALSE(is_valid(static_cast<CommandType>(0U)));
  EXPECT_FALSE(is_valid(static_cast<EventType>(0U)));
  EXPECT_FALSE(is_valid(static_cast<DoneReason>(0U)));
  EXPECT_FALSE(is_valid(RejectReason::none));
  EXPECT_FALSE(is_valid(static_cast<RejectReason>(65'535U)));

  EXPECT_EQ(to_string(static_cast<Side>(0U)), "unknown");
  EXPECT_EQ(to_string(static_cast<RejectReason>(65'535U)), "unknown");
}

TEST(NewOrderValidation, AcceptsSupportedCombinations) {
  auto order = valid_limit_order();
  EXPECT_TRUE(validate(order).accepted());

  order.time_in_force = TimeInForce::ioc;
  EXPECT_TRUE(validate(order).accepted());

  order.order_type = OrderType::market;
  order.limit_price.reset();
  EXPECT_TRUE(validate(order).accepted());
}

TEST(NewOrderValidation, AppliesDeterministicPrecedence) {
  auto order = valid_limit_order();
  order.client_id = ClientId{};
  order.order_id = OrderId{};
  order.instrument_id = InstrumentId{};
  order.quantity = Quantity{};
  order.side = static_cast<Side>(0U);

  EXPECT_EQ(validate(order).reason, RejectReason::invalid_client_id);

  order.client_id = ClientId{17U};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_order_id);

  order.order_id = OrderId{101U};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_instrument_id);

  order.instrument_id = InstrumentId{7U};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_quantity);

  order.quantity = Quantity{25U};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_side);
}

TEST(NewOrderValidation, RejectsUnknownEnumValuesInOrder) {
  auto order = valid_limit_order();
  order.side = static_cast<Side>(255U);
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_side);

  order = valid_limit_order();
  order.order_type = static_cast<OrderType>(255U);
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_order_type);

  order = valid_limit_order();
  order.time_in_force = static_cast<TimeInForce>(255U);
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_time_in_force);
}

TEST(NewOrderValidation, AppliesSideThenTypeThenTimeInForcePrecedence) {
  auto order = valid_limit_order();
  order.side = static_cast<Side>(255U);
  order.order_type = static_cast<OrderType>(255U);
  order.time_in_force = static_cast<TimeInForce>(255U);
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_side);

  order.side = Side::sell;
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_order_type);

  order.order_type = OrderType::limit;
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_time_in_force);
}

TEST(NewOrderValidation, RejectsUnsupportedFokBeforePriceRules) {
  auto order = valid_limit_order();
  order.time_in_force = TimeInForce::fok;
  order.limit_price.reset();

  EXPECT_EQ(validate(order).reason, RejectReason::unsupported_time_in_force);

  order.order_type = OrderType::market;
  EXPECT_EQ(validate(order).reason, RejectReason::unsupported_time_in_force);
}

TEST(NewOrderValidation, EnforcesLimitPriceRules) {
  auto order = valid_limit_order();
  order.limit_price.reset();
  EXPECT_EQ(validate(order).reason, RejectReason::missing_limit_price);

  order.limit_price = PriceTicks{0};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_price);

  order.limit_price = PriceTicks{-1};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_price);
}

TEST(NewOrderValidation, EnforcesMarketPriceAndTimeInForceRules) {
  auto order = valid_limit_order();
  order.order_type = OrderType::market;
  order.time_in_force = TimeInForce::ioc;
  EXPECT_EQ(validate(order).reason, RejectReason::unexpected_limit_price);

  order.limit_price.reset();
  order.time_in_force = TimeInForce::gtc;
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_order_type_time_in_force);
}

TEST(NewOrderValidation, ChecksMarketPriceBeforeTypeTimeInForceCompatibility) {
  auto order = valid_limit_order();
  order.order_type = OrderType::market;
  order.time_in_force = TimeInForce::gtc;

  EXPECT_EQ(validate(order).reason, RejectReason::unexpected_limit_price);

  order.limit_price.reset();
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_order_type_time_in_force);
}

TEST(CancelOrderValidation, AcceptsAWellFormedCancel) {
  EXPECT_TRUE(validate(valid_cancel_order()).accepted());
}

TEST(CancelOrderValidation, AppliesDeterministicPrecedence) {
  auto order = valid_cancel_order();
  order.client_id = ClientId{};
  order.order_id = OrderId{};
  order.instrument_id = InstrumentId{};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_client_id);

  order.client_id = ClientId{17U};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_order_id);

  order.order_id = OrderId{101U};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_instrument_id);
}

TEST(ReplaceOrderValidation, AcceptsAWellFormedReplacement) {
  EXPECT_TRUE(validate(valid_replace_order()).accepted());
}

TEST(ReplaceOrderValidation, AppliesDeterministicPrecedence) {
  auto order = valid_replace_order();
  order.client_id = ClientId{};
  order.old_order_id = OrderId{};
  order.new_order_id = OrderId{};
  order.instrument_id = InstrumentId{};
  order.new_quantity = Quantity{};
  order.new_limit_price = PriceTicks{};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_client_id);

  order.client_id = ClientId{17U};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_order_id);

  order.old_order_id = OrderId{101U};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_order_id);

  order.new_order_id = OrderId{101U};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_replacement_id);

  order.new_order_id = OrderId{102U};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_instrument_id);

  order.instrument_id = InstrumentId{7U};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_quantity);

  order.new_quantity = Quantity{30U};
  EXPECT_EQ(validate(order).reason, RejectReason::invalid_price);
}

TEST(CommandVariant, ReportsTheContainedCommandType) {
  EXPECT_EQ(command_type(Command{valid_limit_order()}), CommandType::new_order);
  EXPECT_EQ(command_type(Command{valid_cancel_order()}), CommandType::cancel);
  EXPECT_EQ(command_type(Command{valid_replace_order()}), CommandType::replace);
}

TEST(RejectReasonVocabulary, CoversStateDependentFutureReasons) {
  EXPECT_TRUE(is_valid(RejectReason::unknown_instrument));
  EXPECT_TRUE(is_valid(RejectReason::quantity_out_of_range));
  EXPECT_TRUE(is_valid(RejectReason::invalid_tick));
  EXPECT_TRUE(is_valid(RejectReason::duplicate_order_id));
  EXPECT_TRUE(is_valid(RejectReason::unknown_order_id));
  EXPECT_TRUE(is_valid(RejectReason::ownership_mismatch));
  EXPECT_TRUE(is_valid(RejectReason::instrument_mismatch));
  EXPECT_TRUE(is_valid(RejectReason::capacity_exceeded));
}

}  // namespace
