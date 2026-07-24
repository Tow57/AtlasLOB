#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <type_traits>
#include <vector>

#include "command_executor.hpp"
#include "top_of_book.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::core;

constexpr domain::InstrumentId instrument_id{11U};

static_assert(!std::is_copy_constructible_v<CommandExecutor>);
static_assert(!std::is_copy_assignable_v<CommandExecutor>);
static_assert(!std::is_move_constructible_v<CommandExecutor>);
static_assert(!std::is_move_assignable_v<CommandExecutor>);
static_assert(!std::is_copy_constructible_v<CommandExecutionResult>);
static_assert(!std::is_copy_assignable_v<CommandExecutionResult>);
static_assert(std::is_nothrow_move_constructible_v<CommandExecutionResult>);
static_assert(std::is_nothrow_move_assignable_v<CommandExecutionResult>);

struct OrderImage final {
  const OrderNode* address{};
  const OrderNode* previous{};
  const OrderNode* next{};
  const PriceLevel* level{};
  domain::OrderId order_id{};
  domain::ClientId client_id{};
  domain::InstrumentId instrument{};
  domain::Side side{};
  domain::PriceTicks price{};
  domain::Quantity remaining{};
  domain::Sequence priority{};

  bool operator==(const OrderImage&) const = default;
};

struct BookImage final {
  std::size_t active_order_count{};
  std::size_t bid_level_count{};
  std::size_t ask_level_count{};
  TopOfBookSnapshot top{};
  std::vector<OrderImage> orders{};

  bool operator==(const BookImage&) const = default;
};

template <domain::Side RestingSide>
void append_side_image(const BookSide<RestingSide>& side, std::vector<OrderImage>& orders) {
  for (const PriceLevel& level : side) {
    for (const OrderNode* node = level.head(); node != nullptr; node = node->next()) {
      orders.push_back({
          .address = node,
          .previous = node->previous(),
          .next = node->next(),
          .level = node->price_level(),
          .order_id = node->order_id(),
          .client_id = node->client_id(),
          .instrument = node->instrument_id(),
          .side = node->side(),
          .price = node->price(),
          .remaining = node->remaining_quantity(),
          .priority = node->priority_sequence(),
      });
    }
  }
}

BookImage capture(const InstrumentBook& book) {
  BookImage result{
      .active_order_count = book.active_order_count(),
      .bid_level_count = book.bids().level_count(),
      .ask_level_count = book.asks().level_count(),
      .top = snapshot_top_of_book(book),
  };
  result.orders.reserve(book.active_order_count());
  append_side_image(book.bids(), result.orders);
  append_side_image(book.asks(), result.orders);
  return result;
}

OrderNodeSpec resting_order(std::uint64_t order_id, std::uint32_t client_id, domain::Side side,
                            std::int64_t price, std::uint64_t quantity, std::uint64_t priority) {
  return {
      .order_id = domain::OrderId{order_id},
      .client_id = domain::ClientId{client_id},
      .instrument_id = instrument_id,
      .side = side,
      .price = domain::PriceTicks{price},
      .remaining_quantity = domain::Quantity{quantity},
      .priority_sequence = domain::Sequence{priority},
  };
}

domain::NewOrder limit_order(std::uint64_t order_id, std::uint32_t client_id, domain::Side side,
                             std::int64_t price, std::uint64_t quantity,
                             domain::TimeInForce time_in_force = domain::TimeInForce::gtc) {
  return {
      .client_id = domain::ClientId{client_id},
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
      .side = side,
      .order_type = domain::OrderType::limit,
      .time_in_force = time_in_force,
      .limit_price = domain::PriceTicks{price},
      .quantity = domain::Quantity{quantity},
  };
}

domain::NewOrder market_order(std::uint64_t order_id, std::uint32_t client_id, domain::Side side,
                              std::uint64_t quantity) {
  return {
      .client_id = domain::ClientId{client_id},
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
      .side = side,
      .order_type = domain::OrderType::market,
      .time_in_force = domain::TimeInForce::ioc,
      .limit_price = std::nullopt,
      .quantity = domain::Quantity{quantity},
  };
}

domain::CancelOrder cancel_order(std::uint64_t order_id, std::uint32_t client_id) {
  return {
      .client_id = domain::ClientId{client_id},
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
  };
}

domain::EventHeader header(std::uint64_t sequence, std::uint32_t index,
                           domain::InstrumentId event_instrument = instrument_id) {
  return {
      .command_sequence = domain::Sequence{sequence},
      .event_index = index,
      .instrument_id = event_instrument,
  };
}

std::optional<domain::TopOfBookLevel> top_level(std::int64_t price, std::uint64_t quantity) {
  return domain::TopOfBookLevel{
      .price = domain::PriceTicks{price},
      .aggregate_quantity = domain::Quantity{quantity},
  };
}

void expect_batch_shape(const domain::EventBatch& batch, std::uint64_t sequence,
                        domain::InstrumentId event_instrument,
                        std::initializer_list<domain::EventType> expected_types) {
  ASSERT_EQ(batch.command_sequence(), domain::Sequence{sequence});
  ASSERT_EQ(batch.instrument_id(), event_instrument);
  ASSERT_EQ(batch.size(), expected_types.size());

  std::size_t index = 0U;
  for (const auto expected_type : expected_types) {
    SCOPED_TRACE(index);
    EXPECT_EQ(domain::event_type(batch[index]), expected_type);
    EXPECT_EQ(domain::event_header(batch[index]),
              header(sequence, static_cast<std::uint32_t>(index), event_instrument));
    ++index;
  }
}

void expect_rejection(const CommandExecutionResult& result, std::uint64_t sequence,
                      domain::InstrumentId event_instrument, domain::CommandType command_type,
                      domain::RejectReason reason,
                      std::optional<domain::OrderId> relevant_order_id) {
  ASSERT_TRUE(result);
  ASSERT_TRUE(result.batch.has_value());
  EXPECT_EQ(result.error, CommandExecutionError::none);
  expect_batch_shape(*result.batch, sequence, event_instrument, {domain::EventType::rejected});
  EXPECT_EQ(std::get<domain::RejectedEvent>((*result.batch)[0]),
            (domain::RejectedEvent{
                .header = header(sequence, 0U, event_instrument),
                .command_type = command_type,
                .reason = reason,
                .order_id = relevant_order_id,
            }));
}

void expect_valid(const InstrumentBook& book) {
  const auto invariant = book.validate_invariants();
  EXPECT_TRUE(invariant) << to_string(invariant.error);
  EXPECT_EQ(invariant.observed_order_count, book.active_order_count());
}

TEST(CommandExecutorRejection,
     PreservesSubmittedInstrumentAndPureBeforeStatePrecedenceWithoutMutation) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting_order(1U, 10U, domain::Side::sell, 100, 5U, 1U)));
  const auto original = capture(book);
  const ExecutionPolicy policy{
      .max_order_quantity = domain::Quantity{10U},
      .tick_increment = domain::PriceTicks{5},
      .max_active_orders = 8U,
  };
  CommandExecutor executor{book, policy, domain::Sequence{50U}};

  auto invalid_client = limit_order(2U, 20U, domain::Side::buy, 95, 1U);
  invalid_client.client_id = {};
  invalid_client.order_id = {};
  invalid_client.instrument_id = {};
  const auto invalid_client_result = executor.execute(invalid_client);
  expect_rejection(invalid_client_result, 50U, {}, domain::CommandType::new_order,
                   domain::RejectReason::invalid_client_id, std::nullopt);
  EXPECT_EQ(capture(book), original);

  auto unknown_instrument = limit_order(2U, 20U, domain::Side::buy, 95, 1U);
  unknown_instrument.instrument_id = domain::InstrumentId{12U};
  const auto unknown_instrument_result = executor.execute(unknown_instrument);
  expect_rejection(unknown_instrument_result, 51U, domain::InstrumentId{12U},
                   domain::CommandType::new_order, domain::RejectReason::unknown_instrument,
                   domain::OrderId{2U});
  EXPECT_EQ(capture(book), original);

  const auto quantity_result = executor.execute(limit_order(3U, 20U, domain::Side::buy, 95, 11U));
  expect_rejection(quantity_result, 52U, instrument_id, domain::CommandType::new_order,
                   domain::RejectReason::quantity_out_of_range, domain::OrderId{3U});
  EXPECT_EQ(capture(book), original);

  const auto tick_result = executor.execute(limit_order(4U, 20U, domain::Side::buy, 97, 1U));
  expect_rejection(tick_result, 53U, instrument_id, domain::CommandType::new_order,
                   domain::RejectReason::invalid_tick, domain::OrderId{4U});
  EXPECT_EQ(capture(book), original);

  const auto duplicate_result = executor.execute(limit_order(1U, 20U, domain::Side::buy, 95, 1U));
  expect_rejection(duplicate_result, 54U, instrument_id, domain::CommandType::new_order,
                   domain::RejectReason::duplicate_order_id, domain::OrderId{1U});
  EXPECT_EQ(capture(book), original);

  auto invalid_cancel = cancel_order(1U, 10U);
  invalid_cancel.client_id = {};
  invalid_cancel.order_id = {};
  invalid_cancel.instrument_id = {};
  const auto invalid_cancel_result = executor.execute(invalid_cancel);
  expect_rejection(invalid_cancel_result, 55U, {}, domain::CommandType::cancel,
                   domain::RejectReason::invalid_client_id, std::nullopt);
  EXPECT_EQ(capture(book), original);

  auto wrong_route = cancel_order(1U, 10U);
  wrong_route.instrument_id = domain::InstrumentId{12U};
  const auto wrong_route_result = executor.execute(wrong_route);
  expect_rejection(wrong_route_result, 56U, domain::InstrumentId{12U}, domain::CommandType::cancel,
                   domain::RejectReason::unknown_instrument, domain::OrderId{1U});
  EXPECT_EQ(capture(book), original);

  const auto unknown_order_result = executor.execute(cancel_order(999U, 10U));
  expect_rejection(unknown_order_result, 57U, instrument_id, domain::CommandType::cancel,
                   domain::RejectReason::unknown_order_id, domain::OrderId{999U});
  EXPECT_EQ(capture(book), original);

  const auto ownership_result = executor.execute(cancel_order(1U, 99U));
  expect_rejection(ownership_result, 58U, instrument_id, domain::CommandType::cancel,
                   domain::RejectReason::ownership_mismatch, domain::OrderId{1U});
  EXPECT_EQ(capture(book), original);
  EXPECT_EQ(executor.next_sequence(), domain::Sequence{59U});
  expect_valid(book);
}

TEST(CommandExecutorSequencing, AdvancesAcrossAcceptedDomainAndCapacityRejectedCommands) {
  InstrumentBook book{instrument_id};
  const ExecutionPolicy policy{
      .max_order_quantity = domain::Quantity{100U},
      .tick_increment = domain::PriceTicks{1},
      .max_active_orders = 1U,
  };
  CommandExecutor executor{book, policy};

  const auto accepted = executor.execute(limit_order(1U, 10U, domain::Side::buy, 100, 5U));
  ASSERT_TRUE(accepted);
  expect_batch_shape(
      *accepted.batch, 1U, instrument_id,
      {domain::EventType::accepted, domain::EventType::rested, domain::EventType::book_changed});

  const auto duplicate = executor.execute(limit_order(1U, 10U, domain::Side::buy, 99, 5U));
  expect_rejection(duplicate, 2U, instrument_id, domain::CommandType::new_order,
                   domain::RejectReason::duplicate_order_id, domain::OrderId{1U});

  const auto before_capacity = capture(book);
  const auto capacity = executor.execute(limit_order(2U, 10U, domain::Side::buy, 99, 5U));
  expect_rejection(capacity, 3U, instrument_id, domain::CommandType::new_order,
                   domain::RejectReason::capacity_exceeded, domain::OrderId{2U});
  EXPECT_EQ(capture(book), before_capacity);

  const auto canceled = executor.execute(cancel_order(1U, 10U));
  ASSERT_TRUE(canceled);
  expect_batch_shape(*canceled.batch, 4U, instrument_id,
                     {domain::EventType::accepted, domain::EventType::canceled,
                      domain::EventType::done, domain::EventType::book_changed});
  EXPECT_EQ(executor.next_sequence(), domain::Sequence{5U});
  EXPECT_TRUE(book.empty());
  expect_valid(book);
}

TEST(CommandExecutorRest, BetterGtcOrderMovesTopAndUsesCommandPriority) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting_order(1U, 10U, domain::Side::buy, 100, 4U, 1U)));
  CommandExecutor executor{book, ExecutionPolicy{}, domain::Sequence{10U}};

  const auto result = executor.execute(limit_order(2U, 20U, domain::Side::buy, 101, 5U));

  ASSERT_TRUE(result);
  expect_batch_shape(
      *result.batch, 10U, instrument_id,
      {domain::EventType::accepted, domain::EventType::rested, domain::EventType::book_changed});
  EXPECT_EQ(std::get<domain::RestedEvent>((*result.batch)[1]),
            (domain::RestedEvent{
                .header = header(10U, 1U),
                .order_id = domain::OrderId{2U},
                .client_id = domain::ClientId{20U},
                .side = domain::Side::buy,
                .price = domain::PriceTicks{101},
                .remaining_quantity = domain::Quantity{5U},
            }));
  EXPECT_EQ(std::get<domain::BookChangedEvent>((*result.batch)[2]).best_bid, top_level(101, 5U));
  ASSERT_NE(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{2U})->priority_sequence(), domain::Sequence{10U});
  expect_valid(book);
}

TEST(CommandExecutorMatch, SellWalksBestPriceFirstThroughInclusiveLimitThenRestsResidual) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting_order(1U, 11U, domain::Side::buy, 102, 2U, 1U)));
  ASSERT_TRUE(book.rest(resting_order(2U, 12U, domain::Side::buy, 101, 3U, 2U)));
  ASSERT_TRUE(book.rest(resting_order(3U, 13U, domain::Side::buy, 100, 7U, 3U)));
  CommandExecutor executor{book, ExecutionPolicy{}, domain::Sequence{20U}};

  const auto result = executor.execute(limit_order(100U, 20U, domain::Side::sell, 101, 8U));

  ASSERT_TRUE(result);
  expect_batch_shape(
      *result.batch, 20U, instrument_id,
      {domain::EventType::accepted, domain::EventType::trade, domain::EventType::trade,
       domain::EventType::rested, domain::EventType::book_changed});
  EXPECT_EQ(std::get<domain::TradeEvent>((*result.batch)[1]),
            (domain::TradeEvent{
                .header = header(20U, 1U),
                .aggressor_order_id = domain::OrderId{100U},
                .resting_order_id = domain::OrderId{1U},
                .aggressor_client_id = domain::ClientId{20U},
                .resting_client_id = domain::ClientId{11U},
                .aggressor_side = domain::Side::sell,
                .execution_price = domain::PriceTicks{102},
                .execution_quantity = domain::Quantity{2U},
                .aggressor_remaining = domain::Quantity{6U},
                .resting_remaining = domain::Quantity{},
            }));
  EXPECT_EQ(std::get<domain::TradeEvent>((*result.batch)[2]),
            (domain::TradeEvent{
                .header = header(20U, 2U),
                .aggressor_order_id = domain::OrderId{100U},
                .resting_order_id = domain::OrderId{2U},
                .aggressor_client_id = domain::ClientId{20U},
                .resting_client_id = domain::ClientId{12U},
                .aggressor_side = domain::Side::sell,
                .execution_price = domain::PriceTicks{101},
                .execution_quantity = domain::Quantity{3U},
                .aggressor_remaining = domain::Quantity{3U},
                .resting_remaining = domain::Quantity{},
            }));
  EXPECT_EQ(std::get<domain::RestedEvent>((*result.batch)[3]),
            (domain::RestedEvent{
                .header = header(20U, 3U),
                .order_id = domain::OrderId{100U},
                .client_id = domain::ClientId{20U},
                .side = domain::Side::sell,
                .price = domain::PriceTicks{101},
                .remaining_quantity = domain::Quantity{3U},
            }));
  EXPECT_EQ(std::get<domain::BookChangedEvent>((*result.batch)[4]),
            (domain::BookChangedEvent{
                .header = header(20U, 4U),
                .best_bid = top_level(100, 7U),
                .best_ask = top_level(101, 3U),
            }));
  EXPECT_EQ(book.find(domain::OrderId{1U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  ASSERT_NE(book.find(domain::OrderId{3U}), nullptr);
  ASSERT_NE(book.find(domain::OrderId{100U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{3U})->remaining_quantity(), domain::Quantity{7U});
  EXPECT_EQ(book.find(domain::OrderId{100U})->priority_sequence(), domain::Sequence{20U});
  EXPECT_EQ(book.active_order_count(), 2U);
  expect_valid(book);
}

TEST(CommandExecutorMatch, MatchesSameClientOrdersWithoutSpecialCase) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting_order(1U, 20U, domain::Side::sell, 100, 5U, 1U)));
  CommandExecutor executor{book, ExecutionPolicy{}, domain::Sequence{10U}};

  const auto result = executor.execute(limit_order(100U, 20U, domain::Side::buy, 100, 5U));

  ASSERT_TRUE(result);
  expect_batch_shape(*result.batch, 10U, instrument_id,
                     {domain::EventType::accepted, domain::EventType::trade,
                      domain::EventType::done, domain::EventType::book_changed});
  EXPECT_EQ(std::get<domain::TradeEvent>((*result.batch)[1]),
            (domain::TradeEvent{
                .header = header(10U, 1U),
                .aggressor_order_id = domain::OrderId{100U},
                .resting_order_id = domain::OrderId{1U},
                .aggressor_client_id = domain::ClientId{20U},
                .resting_client_id = domain::ClientId{20U},
                .aggressor_side = domain::Side::buy,
                .execution_price = domain::PriceTicks{100},
                .execution_quantity = domain::Quantity{5U},
                .aggressor_remaining = domain::Quantity{},
                .resting_remaining = domain::Quantity{},
            }));
  EXPECT_TRUE(book.empty());
  expect_valid(book);
}

TEST(CommandExecutorIoc, DistinguishesExactResidualAndEmptyLimitOutcomes) {
  {
    InstrumentBook book{instrument_id};
    ASSERT_TRUE(book.rest(resting_order(1U, 11U, domain::Side::sell, 100, 5U, 1U)));
    CommandExecutor executor{book, ExecutionPolicy{}, domain::Sequence{10U}};
    const auto result = executor.execute(
        limit_order(100U, 20U, domain::Side::buy, 100, 5U, domain::TimeInForce::ioc));
    ASSERT_TRUE(result);
    expect_batch_shape(*result.batch, 10U, instrument_id,
                       {domain::EventType::accepted, domain::EventType::trade,
                        domain::EventType::done, domain::EventType::book_changed});
    const auto& done = std::get<domain::DoneEvent>((*result.batch)[2]);
    EXPECT_EQ(done.reason, domain::DoneReason::filled);
    EXPECT_EQ(done.remaining_quantity, domain::Quantity{});
    EXPECT_TRUE(book.empty());
    expect_valid(book);
  }

  {
    InstrumentBook book{instrument_id};
    ASSERT_TRUE(book.rest(resting_order(1U, 11U, domain::Side::sell, 100, 3U, 1U)));
    CommandExecutor executor{book, ExecutionPolicy{}, domain::Sequence{10U}};
    const auto result = executor.execute(
        limit_order(100U, 20U, domain::Side::buy, 100, 8U, domain::TimeInForce::ioc));
    ASSERT_TRUE(result);
    expect_batch_shape(*result.batch, 10U, instrument_id,
                       {domain::EventType::accepted, domain::EventType::trade,
                        domain::EventType::done, domain::EventType::book_changed});
    const auto& done = std::get<domain::DoneEvent>((*result.batch)[2]);
    EXPECT_EQ(done.reason, domain::DoneReason::ioc_residual_canceled);
    EXPECT_EQ(done.remaining_quantity, domain::Quantity{5U});
    EXPECT_EQ(book.find(domain::OrderId{100U}), nullptr);
    EXPECT_TRUE(book.empty());
    expect_valid(book);
  }

  {
    InstrumentBook book{instrument_id};
    CommandExecutor executor{book, ExecutionPolicy{}, domain::Sequence{10U}};
    const auto result = executor.execute(
        limit_order(100U, 20U, domain::Side::buy, 100, 8U, domain::TimeInForce::ioc));
    ASSERT_TRUE(result);
    expect_batch_shape(*result.batch, 10U, instrument_id,
                       {domain::EventType::accepted, domain::EventType::done});
    EXPECT_EQ(std::get<domain::DoneEvent>((*result.batch)[1]),
              (domain::DoneEvent{
                  .header = header(10U, 1U),
                  .order_id = domain::OrderId{100U},
                  .reason = domain::DoneReason::ioc_residual_canceled,
                  .remaining_quantity = domain::Quantity{8U},
              }));
    EXPECT_TRUE(book.empty());
    expect_valid(book);
  }
}

TEST(CommandExecutorMarket, DistinguishesExactResidualAndEmptyMarketOutcomes) {
  {
    InstrumentBook book{instrument_id};
    ASSERT_TRUE(book.rest(resting_order(1U, 11U, domain::Side::buy, 100, 5U, 1U)));
    CommandExecutor executor{book, ExecutionPolicy{}, domain::Sequence{10U}};
    const auto result = executor.execute(market_order(100U, 20U, domain::Side::sell, 5U));
    ASSERT_TRUE(result);
    expect_batch_shape(*result.batch, 10U, instrument_id,
                       {domain::EventType::accepted, domain::EventType::trade,
                        domain::EventType::done, domain::EventType::book_changed});
    EXPECT_EQ(std::get<domain::TradeEvent>((*result.batch)[1]).execution_price,
              domain::PriceTicks{100});
    const auto& done = std::get<domain::DoneEvent>((*result.batch)[2]);
    EXPECT_EQ(done.reason, domain::DoneReason::filled);
    EXPECT_EQ(done.remaining_quantity, domain::Quantity{});
    EXPECT_TRUE(book.empty());
    expect_valid(book);
  }

  {
    InstrumentBook book{instrument_id};
    ASSERT_TRUE(book.rest(resting_order(1U, 11U, domain::Side::buy, 100, 3U, 1U)));
    CommandExecutor executor{book, ExecutionPolicy{}, domain::Sequence{10U}};
    const auto result = executor.execute(market_order(100U, 20U, domain::Side::sell, 8U));
    ASSERT_TRUE(result);
    expect_batch_shape(*result.batch, 10U, instrument_id,
                       {domain::EventType::accepted, domain::EventType::trade,
                        domain::EventType::done, domain::EventType::book_changed});
    const auto& done = std::get<domain::DoneEvent>((*result.batch)[2]);
    EXPECT_EQ(done.reason, domain::DoneReason::market_exhausted);
    EXPECT_EQ(done.remaining_quantity, domain::Quantity{5U});
    EXPECT_EQ(book.find(domain::OrderId{100U}), nullptr);
    EXPECT_TRUE(book.empty());
    expect_valid(book);
  }

  {
    InstrumentBook book{instrument_id};
    CommandExecutor executor{book, ExecutionPolicy{}, domain::Sequence{10U}};
    const auto result = executor.execute(market_order(100U, 20U, domain::Side::sell, 8U));
    ASSERT_TRUE(result);
    expect_batch_shape(*result.batch, 10U, instrument_id,
                       {domain::EventType::accepted, domain::EventType::done});
    EXPECT_EQ(std::get<domain::DoneEvent>((*result.batch)[1]),
              (domain::DoneEvent{
                  .header = header(10U, 1U),
                  .order_id = domain::OrderId{100U},
                  .reason = domain::DoneReason::market_exhausted,
                  .remaining_quantity = domain::Quantity{8U},
              }));
    EXPECT_TRUE(book.empty());
    expect_valid(book);
  }
}

TEST(CommandExecutorCapacity, ZeroCapacityRejectsRestButAllowsTerminalCommand) {
  InstrumentBook book{instrument_id};
  const ExecutionPolicy zero_capacity{
      .max_order_quantity = domain::Quantity{100U},
      .tick_increment = domain::PriceTicks{1},
      .max_active_orders = 0U,
  };
  CommandExecutor executor{book, zero_capacity};
  const auto original = capture(book);

  const auto rejected = executor.execute(limit_order(1U, 10U, domain::Side::buy, 100, 5U));
  expect_rejection(rejected, 1U, instrument_id, domain::CommandType::new_order,
                   domain::RejectReason::capacity_exceeded, domain::OrderId{1U});
  EXPECT_EQ(capture(book), original);

  const auto terminal = executor.execute(market_order(2U, 10U, domain::Side::buy, 5U));
  ASSERT_TRUE(terminal);
  expect_batch_shape(*terminal.batch, 2U, instrument_id,
                     {domain::EventType::accepted, domain::EventType::done});
  EXPECT_TRUE(book.empty());
  expect_valid(book);
}

TEST(CommandExecutorCancel, CancelsCurrentResidualWithAcceptedCanceledDoneAndBookChangedPayloads) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting_order(1U, 11U, domain::Side::sell, 100, 5U, 1U)));
  ASSERT_TRUE(book.rest(resting_order(2U, 12U, domain::Side::sell, 100, 7U, 2U)));
  CommandExecutor executor{book, ExecutionPolicy{}, domain::Sequence{10U}};

  const auto partial = executor.execute(market_order(100U, 20U, domain::Side::buy, 3U));
  ASSERT_TRUE(partial);
  ASSERT_NE(book.find(domain::OrderId{1U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{1U})->remaining_quantity(), domain::Quantity{2U});

  const auto canceled = executor.execute(cancel_order(1U, 11U));
  ASSERT_TRUE(canceled);
  expect_batch_shape(*canceled.batch, 11U, instrument_id,
                     {domain::EventType::accepted, domain::EventType::canceled,
                      domain::EventType::done, domain::EventType::book_changed});
  EXPECT_EQ(std::get<domain::AcceptedEvent>((*canceled.batch)[0]),
            (domain::AcceptedEvent{
                .header = header(11U, 0U),
                .command_type = domain::CommandType::cancel,
            }));
  EXPECT_EQ(std::get<domain::CanceledEvent>((*canceled.batch)[1]),
            (domain::CanceledEvent{
                .header = header(11U, 1U),
                .order_id = domain::OrderId{1U},
                .canceled_quantity = domain::Quantity{2U},
            }));
  EXPECT_EQ(std::get<domain::DoneEvent>((*canceled.batch)[2]),
            (domain::DoneEvent{
                .header = header(11U, 2U),
                .order_id = domain::OrderId{1U},
                .reason = domain::DoneReason::canceled,
                .remaining_quantity = domain::Quantity{2U},
            }));
  EXPECT_EQ(std::get<domain::BookChangedEvent>((*canceled.batch)[3]),
            (domain::BookChangedEvent{
                .header = header(11U, 3U),
                .best_bid = std::nullopt,
                .best_ask = top_level(100, 7U),
            }));
  EXPECT_EQ(book.find(domain::OrderId{1U}), nullptr);
  ASSERT_NE(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_EQ(book.best_ask()->head(), book.find(domain::OrderId{2U}));
  EXPECT_EQ(book.active_order_count(), 1U);
  expect_valid(book);
}

TEST(CommandExecutorIdentity, AllowsOrderIdReuseOnlyAfterTerminalRemoval) {
  InstrumentBook book{instrument_id};
  CommandExecutor executor{book};

  const auto first = executor.execute(limit_order(50U, 10U, domain::Side::buy, 100, 5U));
  ASSERT_TRUE(first);
  ASSERT_NE(book.find(domain::OrderId{50U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{50U})->priority_sequence(), domain::Sequence{1U});

  const auto active_duplicate =
      executor.execute(limit_order(50U, 10U, domain::Side::sell, 101, 7U));
  expect_rejection(active_duplicate, 2U, instrument_id, domain::CommandType::new_order,
                   domain::RejectReason::duplicate_order_id, domain::OrderId{50U});

  const auto canceled = executor.execute(cancel_order(50U, 10U));
  ASSERT_TRUE(canceled);
  EXPECT_EQ(book.find(domain::OrderId{50U}), nullptr);

  const auto reused = executor.execute(limit_order(50U, 10U, domain::Side::sell, 101, 7U));
  ASSERT_TRUE(reused);
  expect_batch_shape(
      *reused.batch, 4U, instrument_id,
      {domain::EventType::accepted, domain::EventType::rested, domain::EventType::book_changed});
  ASSERT_NE(book.find(domain::OrderId{50U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{50U})->side(), domain::Side::sell);
  EXPECT_EQ(book.find(domain::OrderId{50U})->remaining_quantity(), domain::Quantity{7U});
  EXPECT_EQ(book.find(domain::OrderId{50U})->priority_sequence(), domain::Sequence{4U});
  EXPECT_EQ(book.active_order_count(), 1U);
  expect_valid(book);
}

}  // namespace
