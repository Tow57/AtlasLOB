#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "atlaslob/matching_engine.hpp"

namespace {

using namespace atlaslob;

constexpr domain::InstrumentId instrument_id{7U};

static_assert(!std::is_copy_constructible_v<MatchingEngine>);
static_assert(!std::is_copy_assignable_v<MatchingEngine>);
static_assert(!std::is_move_constructible_v<MatchingEngine>);
static_assert(!std::is_move_assignable_v<MatchingEngine>);
static_assert(!std::is_copy_constructible_v<EngineResult>);
static_assert(std::is_nothrow_move_constructible_v<EngineResult>);
static_assert(std::is_nothrow_move_assignable_v<EngineResult>);

domain::NewOrder limit_order(std::uint64_t order_id, domain::Side side, std::int64_t price,
                             std::uint64_t quantity,
                             domain::TimeInForce time_in_force = domain::TimeInForce::gtc) {
  return {
      .client_id = domain::ClientId{11U},
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
      .side = side,
      .order_type = domain::OrderType::limit,
      .time_in_force = time_in_force,
      .limit_price = domain::PriceTicks{price},
      .quantity = domain::Quantity{quantity},
  };
}

domain::CancelOrder cancel_order(std::uint64_t order_id) {
  return {
      .client_id = domain::ClientId{11U},
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
  };
}

TEST(MatchingEngineConstruction, ValidatesIdentityAndConfiguration) {
  EXPECT_TRUE(MatchingEngineConfig{}.valid());
  EXPECT_THROW(static_cast<void>(MatchingEngine{domain::InstrumentId{}}), std::invalid_argument);

  auto zero_quantity = MatchingEngineConfig{};
  zero_quantity.max_order_quantity = {};
  EXPECT_FALSE(zero_quantity.valid());
  EXPECT_THROW(static_cast<void>(MatchingEngine{instrument_id, zero_quantity}),
               std::invalid_argument);

  auto zero_tick = MatchingEngineConfig{};
  zero_tick.tick_increment = {};
  EXPECT_FALSE(zero_tick.valid());
  EXPECT_THROW(static_cast<void>(MatchingEngine{instrument_id, zero_tick}), std::invalid_argument);
}

TEST(MatchingEngineObservers, StartsEmptyWithSequenceOne) {
  const MatchingEngine engine{instrument_id};

  EXPECT_EQ(engine.instrument_id(), instrument_id);
  EXPECT_EQ(engine.active_order_count(), 0U);
  EXPECT_TRUE(engine.empty());
  EXPECT_EQ(engine.top(), BookTop{});
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{1U});
  EXPECT_FALSE(engine.sequence_exhausted());
}

TEST(MatchingEngineExecution, RestsAndCancelsThroughTypedAndVariantCommands) {
  MatchingEngine engine{instrument_id};

  auto rested = engine.execute(limit_order(1U, domain::Side::buy, 100, 5U));
  ASSERT_TRUE(rested);
  EXPECT_TRUE(rested.committed());
  EXPECT_FALSE(rested.rejected());
  ASSERT_TRUE(rested.batch.has_value());
  EXPECT_EQ(rested.batch->command_sequence(), domain::Sequence{1U});
  ASSERT_EQ(rested.batch->size(), 3U);
  EXPECT_EQ(domain::event_type((*rested.batch)[0]), domain::EventType::accepted);
  EXPECT_EQ(domain::event_type((*rested.batch)[1]), domain::EventType::rested);
  EXPECT_EQ(domain::event_type((*rested.batch)[2]), domain::EventType::book_changed);
  EXPECT_EQ(engine.active_order_count(), 1U);
  EXPECT_FALSE(engine.empty());
  EXPECT_EQ(engine.top().best_bid, (domain::TopOfBookLevel{
                                       .price = domain::PriceTicks{100},
                                       .aggregate_quantity = domain::Quantity{5U},
                                   }));
  EXPECT_FALSE(engine.top().best_ask.has_value());
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{2U});

  const domain::Command cancel = cancel_order(1U);
  auto canceled = engine.execute(cancel);
  ASSERT_TRUE(canceled);
  EXPECT_TRUE(canceled.committed());
  ASSERT_TRUE(canceled.batch.has_value());
  EXPECT_EQ(canceled.batch->command_sequence(), domain::Sequence{2U});
  ASSERT_EQ(canceled.batch->size(), 4U);
  EXPECT_EQ(domain::event_type((*canceled.batch)[0]), domain::EventType::accepted);
  EXPECT_EQ(domain::event_type((*canceled.batch)[1]), domain::EventType::canceled);
  EXPECT_EQ(domain::event_type((*canceled.batch)[2]), domain::EventType::done);
  EXPECT_EQ(domain::event_type((*canceled.batch)[3]), domain::EventType::book_changed);
  EXPECT_TRUE(engine.empty());
  EXPECT_EQ(engine.active_order_count(), 0U);
  EXPECT_EQ(engine.top(), BookTop{});
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{3U});
}

TEST(MatchingEngineExecution, ReturnsSequencedDomainRejectionsWithoutMutation) {
  MatchingEngine engine{instrument_id};
  auto invalid = limit_order(1U, domain::Side::buy, 100, 0U);

  auto result = engine.execute(invalid);

  ASSERT_TRUE(result);
  EXPECT_TRUE(result.rejected());
  EXPECT_FALSE(result.committed());
  ASSERT_TRUE(result.batch.has_value());
  ASSERT_EQ(result.batch->size(), 1U);
  EXPECT_EQ(result.batch->command_sequence(), domain::Sequence{1U});
  const auto& rejected = std::get<domain::RejectedEvent>((*result.batch)[0]);
  EXPECT_EQ(rejected.reason, domain::RejectReason::invalid_quantity);
  EXPECT_EQ(rejected.order_id, domain::OrderId{1U});
  EXPECT_TRUE(engine.empty());
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{2U});
}

TEST(MatchingEngineExecution, AppliesThePublicQuantityTickAndCapacityConfiguration) {
  const MatchingEngineConfig config{
      .max_order_quantity = domain::Quantity{10U},
      .tick_increment = domain::PriceTicks{5},
      .max_active_orders = 1U,
  };
  MatchingEngine engine{instrument_id, config};

  auto invalid_tick = engine.execute(limit_order(1U, domain::Side::buy, 101, 5U));
  ASSERT_TRUE(invalid_tick.rejected());
  EXPECT_EQ(std::get<domain::RejectedEvent>((*invalid_tick.batch)[0]).reason,
            domain::RejectReason::invalid_tick);

  auto excessive_quantity = engine.execute(limit_order(2U, domain::Side::buy, 100, 11U));
  ASSERT_TRUE(excessive_quantity.rejected());
  EXPECT_EQ(std::get<domain::RejectedEvent>((*excessive_quantity.batch)[0]).reason,
            domain::RejectReason::quantity_out_of_range);

  ASSERT_TRUE(engine.execute(limit_order(3U, domain::Side::buy, 100, 5U)).committed());
  auto over_capacity = engine.execute(limit_order(4U, domain::Side::buy, 95, 5U));
  ASSERT_TRUE(over_capacity.rejected());
  EXPECT_EQ(std::get<domain::RejectedEvent>((*over_capacity.batch)[0]).reason,
            domain::RejectReason::capacity_exceeded);

  EXPECT_EQ(engine.active_order_count(), 1U);
  EXPECT_EQ(engine.top().best_bid, (domain::TopOfBookLevel{
                                       .price = domain::PriceTicks{100},
                                       .aggregate_quantity = domain::Quantity{5U},
                                   }));
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{5U});
}

TEST(MatchingEngineExecution, MatchesMarketOrdersAndExposesTheFinalTop) {
  MatchingEngine engine{instrument_id};
  ASSERT_TRUE(engine.execute(limit_order(1U, domain::Side::sell, 100, 5U)));

  const domain::NewOrder market{
      .client_id = domain::ClientId{22U},
      .order_id = domain::OrderId{2U},
      .instrument_id = instrument_id,
      .side = domain::Side::buy,
      .order_type = domain::OrderType::market,
      .time_in_force = domain::TimeInForce::ioc,
      .limit_price = std::nullopt,
      .quantity = domain::Quantity{7U},
  };
  auto result = engine.execute(market);

  ASSERT_TRUE(result);
  EXPECT_TRUE(result.committed());
  ASSERT_TRUE(result.batch.has_value());
  ASSERT_EQ(result.batch->size(), 4U);
  EXPECT_EQ(domain::event_type((*result.batch)[0]), domain::EventType::accepted);
  EXPECT_EQ(domain::event_type((*result.batch)[1]), domain::EventType::trade);
  EXPECT_EQ(domain::event_type((*result.batch)[2]), domain::EventType::done);
  EXPECT_EQ(domain::event_type((*result.batch)[3]), domain::EventType::book_changed);
  const auto& trade = std::get<domain::TradeEvent>((*result.batch)[1]);
  EXPECT_EQ(trade.execution_price, domain::PriceTicks{100});
  EXPECT_EQ(trade.execution_quantity, domain::Quantity{5U});
  EXPECT_EQ(trade.aggressor_remaining, domain::Quantity{2U});
  EXPECT_EQ(trade.resting_remaining, domain::Quantity{});
  EXPECT_TRUE(engine.empty());
  EXPECT_EQ(engine.top(), BookTop{});
}

TEST(MatchingEngineExecution, ReplacesThroughThePublicVariant) {
  MatchingEngine engine{instrument_id};
  ASSERT_TRUE(engine.execute(limit_order(1U, domain::Side::buy, 100, 5U)));

  const domain::Command replace = domain::ReplaceOrder{
      .client_id = domain::ClientId{11U},
      .old_order_id = domain::OrderId{1U},
      .new_order_id = domain::OrderId{2U},
      .instrument_id = instrument_id,
      .new_limit_price = domain::PriceTicks{101},
      .new_quantity = domain::Quantity{7U},
  };
  auto result = engine.execute(replace);

  ASSERT_TRUE(result);
  EXPECT_TRUE(result.committed());
  ASSERT_TRUE(result.batch.has_value());
  ASSERT_EQ(result.batch->size(), 6U);
  const std::array expected_types{
      domain::EventType::accepted, domain::EventType::replaced, domain::EventType::canceled,
      domain::EventType::done,     domain::EventType::rested,   domain::EventType::book_changed,
  };
  for (std::size_t index = 0U; index < expected_types.size(); ++index) {
    EXPECT_EQ(domain::event_type((*result.batch)[index]), expected_types[index]);
    EXPECT_EQ(domain::event_header((*result.batch)[index]),
              (domain::EventHeader{
                  .command_sequence = domain::Sequence{2U},
                  .event_index = static_cast<std::uint32_t>(index),
                  .instrument_id = instrument_id,
              }));
  }
  EXPECT_EQ(std::get<domain::ReplacedEvent>((*result.batch)[1]),
            (domain::ReplacedEvent{
                .header =
                    {
                        .command_sequence = domain::Sequence{2U},
                        .event_index = 1U,
                        .instrument_id = instrument_id,
                    },
                .old_order_id = domain::OrderId{1U},
                .new_order_id = domain::OrderId{2U},
            }));
  EXPECT_EQ(std::get<domain::CanceledEvent>((*result.batch)[2]).canceled_quantity,
            domain::Quantity{5U});
  EXPECT_EQ(std::get<domain::DoneEvent>((*result.batch)[3]).reason, domain::DoneReason::replaced);
  EXPECT_EQ(std::get<domain::RestedEvent>((*result.batch)[4]).remaining_quantity,
            domain::Quantity{7U});
  EXPECT_EQ(engine.active_order_count(), 1U);
  EXPECT_EQ(engine.top().best_bid, (domain::TopOfBookLevel{
                                       .price = domain::PriceTicks{101},
                                       .aggregate_quantity = domain::Quantity{7U},
                                   }));

  auto old_cancel = engine.execute(cancel_order(1U));
  ASSERT_TRUE(old_cancel);
  EXPECT_TRUE(old_cancel.rejected());
  auto new_cancel = engine.execute(cancel_order(2U));
  ASSERT_TRUE(new_cancel);
  EXPECT_TRUE(new_cancel.committed());
  EXPECT_TRUE(engine.empty());
}

TEST(MatchingEngineExecution, FullyFilledReplacementPartiallyReducesItsPassive) {
  MatchingEngine engine{instrument_id};
  ASSERT_TRUE(engine.execute(limit_order(2U, domain::Side::sell, 100, 10U)));
  ASSERT_TRUE(engine.execute(limit_order(1U, domain::Side::buy, 99, 4U)));

  auto result = engine.execute(domain::ReplaceOrder{
      .client_id = domain::ClientId{11U},
      .old_order_id = domain::OrderId{1U},
      .new_order_id = domain::OrderId{3U},
      .instrument_id = instrument_id,
      .new_limit_price = domain::PriceTicks{100},
      .new_quantity = domain::Quantity{6U},
  });

  ASSERT_TRUE(result.committed());
  ASSERT_TRUE(result.batch.has_value());
  ASSERT_EQ(result.batch->size(), 7U);
  EXPECT_EQ(domain::event_type((*result.batch)[4]), domain::EventType::trade);
  const auto& trade = std::get<domain::TradeEvent>((*result.batch)[4]);
  EXPECT_EQ(trade.execution_quantity, domain::Quantity{6U});
  EXPECT_EQ(trade.aggressor_remaining, domain::Quantity{});
  EXPECT_EQ(trade.resting_remaining, domain::Quantity{4U});
  const auto& done = std::get<domain::DoneEvent>((*result.batch)[5]);
  EXPECT_EQ(done.order_id, domain::OrderId{3U});
  EXPECT_EQ(done.reason, domain::DoneReason::filled);
  EXPECT_EQ(done.remaining_quantity, domain::Quantity{});
  EXPECT_EQ(engine.active_order_count(), 1U);
  EXPECT_FALSE(engine.top().best_bid.has_value());
  EXPECT_EQ(engine.top().best_ask, (domain::TopOfBookLevel{
                                       .price = domain::PriceTicks{100},
                                       .aggregate_quantity = domain::Quantity{4U},
                                   }));
}

TEST(MatchingEngineResult, MoveInvalidatesTheSourceWithoutUnsafeObservers) {
  MatchingEngine engine{instrument_id};
  auto source = engine.execute(limit_order(1U, domain::Side::buy, 100, 5U));
  ASSERT_TRUE(source.committed());

  EngineResult moved{std::move(source)};
  EXPECT_FALSE(source.has_value());
  EXPECT_FALSE(source.committed());
  EXPECT_FALSE(source.rejected());
  EXPECT_FALSE(source.batch.has_value());
  ASSERT_TRUE(moved.committed());

  EngineResult assigned;
  assigned = std::move(moved);
  EXPECT_FALSE(moved.has_value());
  EXPECT_FALSE(moved.committed());
  EXPECT_FALSE(moved.rejected());
  EXPECT_FALSE(moved.batch.has_value());
  ASSERT_TRUE(assigned.committed());
}

TEST(MatchingEngineVocabulary, HasStableStrings) {
  EXPECT_EQ(to_string(EngineError::sequence_exhausted), "sequence_exhausted");
  EXPECT_EQ(to_string(EngineError::internal_failure), "internal_failure");
  EXPECT_EQ(to_string(static_cast<EngineError>(255U)), "unknown");
}

}  // namespace
