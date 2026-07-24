#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "command_executor.hpp"
#include "top_of_book.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::core;

constexpr domain::InstrumentId instrument_id{1U};

OrderNodeSpec resting(std::uint64_t order_id, std::uint32_t client_id, domain::Side side,
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

domain::NewOrder incoming(std::uint64_t order_id, std::uint32_t client_id, domain::Side side,
                          domain::OrderType order_type, domain::TimeInForce time_in_force,
                          std::optional<domain::PriceTicks> price, std::uint64_t quantity) {
  return {
      .client_id = domain::ClientId{client_id},
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
      .side = side,
      .order_type = order_type,
      .time_in_force = time_in_force,
      .limit_price = price,
      .quantity = domain::Quantity{quantity},
  };
}

std::vector<domain::EventType> event_types(const domain::EventBatch& batch) {
  std::vector<domain::EventType> result;
  result.reserve(batch.size());
  for (const auto& event : batch.events()) {
    result.push_back(domain::event_type(event));
  }
  return result;
}

void expect_headers(const domain::EventBatch& batch, domain::Sequence sequence,
                    domain::InstrumentId submitted_instrument) {
  EXPECT_EQ(batch.command_sequence(), sequence);
  EXPECT_EQ(batch.instrument_id(), submitted_instrument);
  for (std::size_t index = 0U; index < batch.size(); ++index) {
    const auto& header = domain::event_header(batch[index]);
    EXPECT_EQ(header.command_sequence, sequence);
    EXPECT_EQ(header.event_index, index);
    EXPECT_EQ(header.instrument_id, submitted_instrument);
  }
}

static_assert(!std::is_copy_constructible_v<CommandExecutor>);
static_assert(!std::is_move_constructible_v<CommandExecutor>);
static_assert(!std::is_copy_constructible_v<CommandExecutionResult>);
static_assert(std::is_nothrow_move_constructible_v<CommandExecutionResult>);

TEST(CommandExecutorSmoke, SequencesAndPreservesAZeroInstrumentPureRejection) {
  InstrumentBook book{instrument_id};
  CommandExecutor executor{book};
  auto invalid = incoming(9U, 0U, domain::Side::buy, domain::OrderType::limit,
                          domain::TimeInForce::gtc, domain::PriceTicks{100}, 5U);
  invalid.instrument_id = {};

  auto result = executor.execute(invalid);

  ASSERT_TRUE(result);
  ASSERT_TRUE(result.batch.has_value());
  expect_headers(*result.batch, domain::Sequence{1U}, domain::InstrumentId{});
  ASSERT_EQ(result.batch->size(), 1U);
  const auto& rejected = std::get<domain::RejectedEvent>((*result.batch)[0]);
  EXPECT_EQ(rejected.command_type, domain::CommandType::new_order);
  EXPECT_EQ(rejected.reason, domain::RejectReason::invalid_client_id);
  EXPECT_EQ(rejected.order_id, domain::OrderId{9U});
  EXPECT_TRUE(book.empty());
  EXPECT_EQ(executor.next_sequence(), domain::Sequence{2U});
}

TEST(CommandExecutorSmoke, RestsAtEmptyEqualAndNonbestPricesWithCoalescedBboEvents) {
  InstrumentBook book{instrument_id};
  CommandExecutor executor{book};

  auto first = executor.execute(incoming(1U, 7U, domain::Side::buy, domain::OrderType::limit,
                                         domain::TimeInForce::gtc, domain::PriceTicks{100}, 5U));
  ASSERT_TRUE(first);
  EXPECT_EQ(event_types(*first.batch),
            (std::vector{domain::EventType::accepted, domain::EventType::rested,
                         domain::EventType::book_changed}));
  const auto* first_node = book.find(domain::OrderId{1U});
  ASSERT_NE(first_node, nullptr);
  EXPECT_EQ(first_node->priority_sequence(), domain::Sequence{1U});
  EXPECT_EQ(book.best_bid()->aggregate_quantity(), domain::Quantity{5U});

  auto same_best =
      executor.execute(incoming(2U, 8U, domain::Side::buy, domain::OrderType::limit,
                                domain::TimeInForce::gtc, domain::PriceTicks{100}, 3U));
  ASSERT_TRUE(same_best);
  EXPECT_EQ(event_types(*same_best.batch),
            (std::vector{domain::EventType::accepted, domain::EventType::rested,
                         domain::EventType::book_changed}));
  EXPECT_EQ(book.best_bid()->aggregate_quantity(), domain::Quantity{8U});
  ASSERT_NE(book.best_bid()->tail(), nullptr);
  EXPECT_EQ(book.best_bid()->tail()->order_id(), domain::OrderId{2U});

  const auto top_before = snapshot_top_of_book(book);
  auto nonbest = executor.execute(incoming(3U, 9U, domain::Side::buy, domain::OrderType::limit,
                                           domain::TimeInForce::gtc, domain::PriceTicks{99}, 4U));
  ASSERT_TRUE(nonbest);
  EXPECT_EQ(event_types(*nonbest.batch),
            (std::vector{domain::EventType::accepted, domain::EventType::rested}));
  EXPECT_EQ(snapshot_top_of_book(book), top_before);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(CommandExecutorSmoke, SweepsPriceTimePriorityAndPartiallyFillsLastPassive) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::sell, 100, 5U, 1U)));
  ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::sell, 100, 7U, 2U)));
  ASSERT_TRUE(book.rest(resting(3U, 13U, domain::Side::sell, 101, 11U, 3U)));
  CommandExecutor executor{book, {}, domain::Sequence{10U}};

  auto result = executor.execute(incoming(100U, 99U, domain::Side::buy, domain::OrderType::limit,
                                          domain::TimeInForce::gtc, domain::PriceTicks{101}, 20U));

  ASSERT_TRUE(result);
  expect_headers(*result.batch, domain::Sequence{10U}, instrument_id);
  EXPECT_EQ(event_types(*result.batch),
            (std::vector{domain::EventType::accepted, domain::EventType::trade,
                         domain::EventType::trade, domain::EventType::trade,
                         domain::EventType::done, domain::EventType::book_changed}));
  const auto& first_trade = std::get<domain::TradeEvent>((*result.batch)[1]);
  const auto& second_trade = std::get<domain::TradeEvent>((*result.batch)[2]);
  const auto& third_trade = std::get<domain::TradeEvent>((*result.batch)[3]);
  EXPECT_EQ(first_trade.resting_order_id, domain::OrderId{1U});
  EXPECT_EQ(first_trade.execution_quantity, domain::Quantity{5U});
  EXPECT_EQ(first_trade.aggressor_remaining, domain::Quantity{15U});
  EXPECT_EQ(second_trade.resting_order_id, domain::OrderId{2U});
  EXPECT_EQ(second_trade.aggressor_remaining, domain::Quantity{8U});
  EXPECT_EQ(third_trade.resting_order_id, domain::OrderId{3U});
  EXPECT_EQ(third_trade.execution_quantity, domain::Quantity{8U});
  EXPECT_EQ(third_trade.resting_remaining, domain::Quantity{3U});
  const auto& done = std::get<domain::DoneEvent>((*result.batch)[4]);
  EXPECT_EQ(done.reason, domain::DoneReason::filled);
  EXPECT_EQ(done.remaining_quantity, domain::Quantity{});

  EXPECT_EQ(book.find(domain::OrderId{1U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  ASSERT_NE(book.find(domain::OrderId{3U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{3U})->remaining_quantity(), domain::Quantity{3U});
  EXPECT_EQ(book.best_ask()->price(), domain::PriceTicks{101});
  EXPECT_EQ(book.best_ask()->aggregate_quantity(), domain::Quantity{3U});
  EXPECT_TRUE(book.validate_invariants());
}

TEST(CommandExecutorSmoke, AppliesGtcResidualAndChecksCapacityAgainstFinalState) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::sell, 100, 5U, 1U)));
  const ExecutionPolicy policy{
      .max_order_quantity = domain::Quantity{std::numeric_limits<std::uint64_t>::max()},
      .tick_increment = domain::PriceTicks{1},
      .max_active_orders = 1U,
  };
  CommandExecutor executor{book, policy, domain::Sequence{10U}};

  auto result = executor.execute(incoming(100U, 99U, domain::Side::buy, domain::OrderType::limit,
                                          domain::TimeInForce::gtc, domain::PriceTicks{100}, 8U));

  ASSERT_TRUE(result);
  EXPECT_EQ(event_types(*result.batch),
            (std::vector{domain::EventType::accepted, domain::EventType::trade,
                         domain::EventType::rested, domain::EventType::book_changed}));
  EXPECT_EQ(book.find(domain::OrderId{1U}), nullptr);
  const auto* residual = book.find(domain::OrderId{100U});
  ASSERT_NE(residual, nullptr);
  EXPECT_EQ(residual->side(), domain::Side::buy);
  EXPECT_EQ(residual->price(), domain::PriceTicks{100});
  EXPECT_EQ(residual->remaining_quantity(), domain::Quantity{3U});
  EXPECT_EQ(residual->priority_sequence(), domain::Sequence{10U});
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(CommandExecutorSmoke, RejectsOnlyWhenThePlannedFinalActiveCountExceedsPolicy) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::buy, 100, 5U, 1U)));
  const ExecutionPolicy policy{
      .max_order_quantity = domain::Quantity{std::numeric_limits<std::uint64_t>::max()},
      .tick_increment = domain::PriceTicks{1},
      .max_active_orders = 1U,
  };
  CommandExecutor executor{book, policy, domain::Sequence{10U}};
  const auto* original = book.find(domain::OrderId{1U});

  auto rejected = executor.execute(incoming(2U, 12U, domain::Side::buy, domain::OrderType::limit,
                                            domain::TimeInForce::gtc, domain::PriceTicks{99}, 4U));

  ASSERT_TRUE(rejected);
  ASSERT_EQ(rejected.batch->size(), 1U);
  const auto& event = std::get<domain::RejectedEvent>((*rejected.batch)[0]);
  EXPECT_EQ(event.reason, domain::RejectReason::capacity_exceeded);
  EXPECT_EQ(event.order_id, domain::OrderId{2U});
  EXPECT_EQ(book.find(domain::OrderId{1U}), original);
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_TRUE(book.validate_invariants());

  InstrumentBook terminal_book{instrument_id};
  ASSERT_TRUE(terminal_book.rest(resting(3U, 13U, domain::Side::sell, 100, 5U, 1U)));
  ExecutionPolicy zero_capacity = policy;
  zero_capacity.max_active_orders = 0U;
  CommandExecutor terminal_executor{terminal_book, zero_capacity, domain::Sequence{10U}};
  auto fully_filled =
      terminal_executor.execute(incoming(4U, 14U, domain::Side::buy, domain::OrderType::limit,
                                         domain::TimeInForce::gtc, domain::PriceTicks{100}, 5U));
  ASSERT_TRUE(fully_filled);
  EXPECT_EQ(std::get<domain::DoneEvent>((*fully_filled.batch)[2]).reason,
            domain::DoneReason::filled);
  EXPECT_TRUE(terminal_book.empty());
}

TEST(CommandExecutorSmoke, RejectsRestingAggregateOverflowWithoutMutation) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(
      resting(1U, 11U, domain::Side::buy, 100, std::numeric_limits<std::uint64_t>::max(), 1U)));
  CommandExecutor executor{book, {}, domain::Sequence{10U}};
  const auto* original = book.find(domain::OrderId{1U});
  const auto before = snapshot_top_of_book(book);

  auto result = executor.execute(incoming(2U, 12U, domain::Side::buy, domain::OrderType::limit,
                                          domain::TimeInForce::gtc, domain::PriceTicks{100}, 1U));

  ASSERT_TRUE(result);
  ASSERT_EQ(result.batch->size(), 1U);
  const auto& rejected = std::get<domain::RejectedEvent>((*result.batch)[0]);
  EXPECT_EQ(rejected.reason, domain::RejectReason::capacity_exceeded);
  EXPECT_EQ(rejected.order_id, domain::OrderId{2U});
  EXPECT_EQ(book.find(domain::OrderId{1U}), original);
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_EQ(snapshot_top_of_book(book), before);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(CommandExecutorSmoke, DistinguishesIocAndMarketTerminalResiduals) {
  InstrumentBook ioc_book{instrument_id};
  ASSERT_TRUE(ioc_book.rest(resting(1U, 11U, domain::Side::sell, 100, 3U, 1U)));
  CommandExecutor ioc_executor{ioc_book, {}, domain::Sequence{10U}};
  auto ioc = ioc_executor.execute(incoming(100U, 99U, domain::Side::buy, domain::OrderType::limit,
                                           domain::TimeInForce::ioc, domain::PriceTicks{100}, 8U));
  ASSERT_TRUE(ioc);
  EXPECT_EQ(event_types(*ioc.batch),
            (std::vector{domain::EventType::accepted, domain::EventType::trade,
                         domain::EventType::done, domain::EventType::book_changed}));
  const auto& ioc_done = std::get<domain::DoneEvent>((*ioc.batch)[2]);
  EXPECT_EQ(ioc_done.reason, domain::DoneReason::ioc_residual_canceled);
  EXPECT_EQ(ioc_done.remaining_quantity, domain::Quantity{5U});
  EXPECT_EQ(ioc_book.find(domain::OrderId{100U}), nullptr);

  InstrumentBook empty_book{instrument_id};
  CommandExecutor market_executor{empty_book};
  auto market =
      market_executor.execute(incoming(101U, 98U, domain::Side::sell, domain::OrderType::market,
                                       domain::TimeInForce::ioc, std::nullopt, 7U));
  ASSERT_TRUE(market);
  EXPECT_EQ(event_types(*market.batch),
            (std::vector{domain::EventType::accepted, domain::EventType::done}));
  const auto& market_done = std::get<domain::DoneEvent>((*market.batch)[1]);
  EXPECT_EQ(market_done.reason, domain::DoneReason::market_exhausted);
  EXPECT_EQ(market_done.remaining_quantity, domain::Quantity{7U});
  EXPECT_TRUE(empty_book.empty());
}

TEST(CommandExecutorSmoke, CancelsBestAndNonbestOrdersWithExactCurrentQuantity) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::buy, 100, 5U, 1U)));
  ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::buy, 100, 7U, 2U)));
  ASSERT_TRUE(book.rest(resting(3U, 13U, domain::Side::buy, 99, 9U, 3U)));
  ASSERT_TRUE(book.reduce_remaining(*book.find(domain::OrderId{1U}), domain::Quantity{2U}));
  CommandExecutor executor{book, {}, domain::Sequence{10U}};

  auto best = executor.execute(domain::CancelOrder{
      .client_id = domain::ClientId{11U},
      .order_id = domain::OrderId{1U},
      .instrument_id = instrument_id,
  });
  ASSERT_TRUE(best);
  EXPECT_EQ(event_types(*best.batch),
            (std::vector{domain::EventType::accepted, domain::EventType::canceled,
                         domain::EventType::done, domain::EventType::book_changed}));
  EXPECT_EQ(std::get<domain::CanceledEvent>((*best.batch)[1]).canceled_quantity,
            domain::Quantity{3U});
  EXPECT_EQ(std::get<domain::DoneEvent>((*best.batch)[2]).remaining_quantity, domain::Quantity{3U});
  EXPECT_EQ(book.best_bid()->aggregate_quantity(), domain::Quantity{7U});

  const auto top_before = snapshot_top_of_book(book);
  auto nonbest = executor.execute(domain::CancelOrder{
      .client_id = domain::ClientId{13U},
      .order_id = domain::OrderId{3U},
      .instrument_id = instrument_id,
  });
  ASSERT_TRUE(nonbest);
  EXPECT_EQ(event_types(*nonbest.batch),
            (std::vector{domain::EventType::accepted, domain::EventType::canceled,
                         domain::EventType::done}));
  EXPECT_EQ(snapshot_top_of_book(book), top_before);
  EXPECT_EQ(book.find(domain::OrderId{3U}), nullptr);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(CommandExecutorSmoke, IssuesMaximumSequenceOnceThenFailsWithoutMutation) {
  InstrumentBook book{instrument_id};
  CommandExecutor executor{book, {}, domain::Sequence{std::numeric_limits<std::uint64_t>::max()}};

  auto accepted = executor.execute(incoming(1U, 7U, domain::Side::buy, domain::OrderType::limit,
                                            domain::TimeInForce::gtc, domain::PriceTicks{100}, 5U));
  ASSERT_TRUE(accepted);
  EXPECT_EQ(accepted.batch->command_sequence(),
            domain::Sequence{std::numeric_limits<std::uint64_t>::max()});
  const auto* node = book.find(domain::OrderId{1U});
  ASSERT_NE(node, nullptr);

  auto exhausted = executor.execute(domain::CancelOrder{
      .client_id = domain::ClientId{7U},
      .order_id = domain::OrderId{1U},
      .instrument_id = instrument_id,
  });
  EXPECT_FALSE(exhausted);
  EXPECT_FALSE(exhausted.batch.has_value());
  EXPECT_EQ(exhausted.error, CommandExecutionError::admission_failure);
  EXPECT_EQ(exhausted.admission_error, CommandAdmissionError::sequence_exhausted);
  EXPECT_EQ(book.find(domain::OrderId{1U}), node);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(CommandExecutorSmoke, RequiresRestoredSequencingToExceedEveryActivePriority) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting(1U, 7U, domain::Side::buy, 100, 5U, 4U)));
  ASSERT_TRUE(book.rest(resting(2U, 8U, domain::Side::sell, 101, 6U, 9U)));
  const auto* first = book.find(domain::OrderId{1U});
  const auto* second = book.find(domain::OrderId{2U});

  EXPECT_THROW(static_cast<void>(CommandExecutor{book}), std::invalid_argument);
  EXPECT_THROW(static_cast<void>(CommandExecutor{book, {}, domain::Sequence{9U}}),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(CommandExecutor{book, {}, domain::Sequence{8U}}),
               std::invalid_argument);

  EXPECT_NO_THROW(static_cast<void>(CommandExecutor{book, {}, domain::Sequence{10U}}));
  EXPECT_EQ(book.find(domain::OrderId{1U}), first);
  EXPECT_EQ(book.find(domain::OrderId{2U}), second);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(CommandExecutorSmoke, RejectsAttachmentDuringPendingResidualPreparation) {
  InstrumentBook book{instrument_id};
  {
    auto pending = book.prepare_rest(resting(1U, 7U, domain::Side::buy, 100, 5U, 4U));
    ASSERT_TRUE(pending);
    EXPECT_TRUE(book.empty());
    EXPECT_TRUE(book.has_pending_preparation());
    EXPECT_THROW(static_cast<void>(CommandExecutor{book}), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(CommandExecutor{book, {}, domain::Sequence{5U}}),
                 std::invalid_argument);
    EXPECT_TRUE(book.validate_invariants());
  }

  EXPECT_FALSE(book.has_pending_preparation());
  EXPECT_TRUE(book.empty());
  EXPECT_NO_THROW(static_cast<void>(CommandExecutor{book}));
  EXPECT_TRUE(book.validate_invariants());
}

TEST(CommandExecutorSmoke, AllocationFailureAfterResidualPreparationRollsBackExactly) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::sell, 100, 5U, 1U)));
  CommandExecutor executor{book, {}, domain::Sequence{10U}};
  const auto before = snapshot_top_of_book(book);
  const auto* passive = book.find(domain::OrderId{1U});
  ASSERT_NE(passive, nullptr);

  executor.set_before_event_allocation_hook_for_testing(+[] { throw std::bad_alloc{}; });
  EXPECT_THROW(static_cast<void>(executor.execute(
                   incoming(100U, 99U, domain::Side::buy, domain::OrderType::limit,
                            domain::TimeInForce::gtc, domain::PriceTicks{100}, 8U))),
               std::bad_alloc);

  EXPECT_EQ(executor.next_sequence(), domain::Sequence{11U});
  EXPECT_EQ(book.find(domain::OrderId{1U}), passive);
  EXPECT_EQ(passive->remaining_quantity(), domain::Quantity{5U});
  EXPECT_EQ(book.find(domain::OrderId{100U}), nullptr);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_EQ(snapshot_top_of_book(book), before);
  EXPECT_TRUE(book.validate_invariants());

  executor.set_before_event_allocation_hook_for_testing(nullptr);
  auto retry = executor.execute(incoming(100U, 99U, domain::Side::buy, domain::OrderType::limit,
                                         domain::TimeInForce::gtc, domain::PriceTicks{100}, 8U));
  ASSERT_TRUE(retry);
  EXPECT_EQ(retry.batch->command_sequence(), domain::Sequence{11U});
  EXPECT_TRUE(book.validate_invariants());
}

TEST(CommandExecutorVocabulary, HasStableStrings) {
  EXPECT_EQ(to_string(CommandExecutionError::passive_binding_failure), "passive_binding_failure");
  EXPECT_EQ(to_string(PassiveBindingError::quantity_chain_mismatch), "quantity_chain_mismatch");
  EXPECT_EQ(to_string(static_cast<CommandExecutionError>(255U)), "unknown");
  EXPECT_EQ(to_string(static_cast<PassiveBindingError>(255U)), "unknown");
}

}  // namespace
