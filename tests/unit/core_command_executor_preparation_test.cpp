#include <gtest/gtest.h>

#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "command_executor.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::core;

constexpr domain::InstrumentId instrument_id{11U};

static_assert(!std::is_copy_constructible_v<PreparedCommandExecution>);
static_assert(!std::is_copy_assignable_v<PreparedCommandExecution>);
static_assert(std::is_nothrow_move_constructible_v<PreparedCommandExecution>);
static_assert(std::is_nothrow_move_assignable_v<PreparedCommandExecution>);
static_assert(std::is_nothrow_destructible_v<PreparedCommandExecution>);
static_assert(noexcept(std::declval<PreparedCommandExecution&>().commit()));

[[nodiscard]] OrderNodeSpec resting_order(std::uint64_t order_id, std::uint32_t client_id,
                                          domain::Side side, std::int64_t price,
                                          std::uint64_t quantity, std::uint64_t priority) {
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

[[nodiscard]] domain::NewOrder limit_order(std::uint64_t order_id, std::uint32_t client_id,
                                           domain::Side side, std::int64_t price,
                                           std::uint64_t quantity) {
  return {
      .client_id = domain::ClientId{client_id},
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
      .side = side,
      .order_type = domain::OrderType::limit,
      .time_in_force = domain::TimeInForce::gtc,
      .limit_price = domain::PriceTicks{price},
      .quantity = domain::Quantity{quantity},
  };
}

[[nodiscard]] domain::CancelOrder cancel_order(std::uint64_t order_id, std::uint32_t client_id) {
  return {
      .client_id = domain::ClientId{client_id},
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
  };
}

TEST(CommandExecutorPreparation, AbandoningAStagedResidualRestoresTheExactVisibleBook) {
  InstrumentBook book{instrument_id};
  CommandExecutor executor{book};

  {
    auto prepared =
        executor.prepare_at(limit_order(1U, 7U, domain::Side::buy, 100, 5U), domain::Sequence{20U});

    ASSERT_TRUE(prepared);
    EXPECT_TRUE(prepared.committed());
    EXPECT_FALSE(prepared.rejected());
    ASSERT_NE(prepared.batch(), nullptr);
    EXPECT_EQ(prepared.batch()->command_sequence(), domain::Sequence{20U});
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.find(domain::OrderId{1U}), nullptr);
    EXPECT_TRUE(book.has_pending_preparation());
    EXPECT_TRUE(book.validate_invariants());
  }

  EXPECT_TRUE(book.empty());
  EXPECT_EQ(book.find(domain::OrderId{1U}), nullptr);
  EXPECT_FALSE(book.has_pending_preparation());
  EXPECT_TRUE(book.validate_invariants());
}

TEST(CommandExecutorPreparation, AbandoningACancelLeavesItsBoundOrderUntouched) {
  InstrumentBook book{instrument_id};
  const auto rested = book.rest(resting_order(1U, 7U, domain::Side::buy, 100, 5U, 1U));
  ASSERT_TRUE(rested);
  CommandExecutor executor{book, {}, domain::Sequence{10U}};

  {
    auto prepared = executor.prepare_at(cancel_order(1U, 7U), domain::Sequence{20U});
    ASSERT_TRUE(prepared);
    EXPECT_TRUE(prepared.committed());
    EXPECT_FALSE(book.has_pending_preparation());
    EXPECT_EQ(book.find(domain::OrderId{1U}), rested.node);
    EXPECT_EQ(rested.node->remaining_quantity(), domain::Quantity{5U});

    auto overlap = executor.prepare_at(cancel_order(1U, 7U), domain::Sequence{21U});
    EXPECT_FALSE(overlap);
    EXPECT_EQ(overlap.result().error, CommandExecutionError::preparation_in_progress);
    EXPECT_EQ(overlap.batch(), nullptr);
    auto overlap_result = overlap.commit();
    EXPECT_FALSE(overlap_result);
    EXPECT_EQ(overlap_result.error, CommandExecutionError::preparation_in_progress);
    EXPECT_TRUE(prepared);

    auto execute_overlap = executor.execute(cancel_order(1U, 7U));
    EXPECT_FALSE(execute_overlap);
    EXPECT_EQ(execute_overlap.error, CommandExecutionError::preparation_in_progress);
    EXPECT_EQ(executor.next_sequence(), domain::Sequence{10U});
  }

  EXPECT_EQ(book.find(domain::OrderId{1U}), rested.node);
  EXPECT_EQ(rested.node->remaining_quantity(), domain::Quantity{5U});
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_TRUE(book.validate_invariants());

  auto after_abandon = executor.prepare_at(cancel_order(1U, 7U), domain::Sequence{22U});
  EXPECT_TRUE(after_abandon);
}

TEST(CommandExecutorPreparation, FullFillPreparationOwnsTheLeaseWithoutAStagedResidual) {
  InstrumentBook book{instrument_id};
  const auto passive = book.rest(resting_order(1U, 7U, domain::Side::sell, 100, 5U, 1U));
  ASSERT_TRUE(passive);
  CommandExecutor executor{book, {}, domain::Sequence{10U}};

  {
    auto prepared =
        executor.prepare_at(limit_order(2U, 8U, domain::Side::buy, 100, 5U), domain::Sequence{20U});
    ASSERT_TRUE(prepared);
    EXPECT_TRUE(prepared.committed());
    EXPECT_FALSE(book.has_pending_preparation());
    EXPECT_EQ(book.find(domain::OrderId{1U}), passive.node);

    auto overlap = executor.prepare_at(cancel_order(1U, 7U), domain::Sequence{21U});
    EXPECT_FALSE(overlap);
    EXPECT_EQ(overlap.result().error, CommandExecutionError::preparation_in_progress);
  }

  EXPECT_EQ(book.find(domain::OrderId{1U}), passive.node);
  EXPECT_EQ(passive.node->remaining_quantity(), domain::Quantity{5U});
  EXPECT_TRUE(book.validate_invariants());

  auto retry =
      executor.prepare_at(limit_order(2U, 8U, domain::Side::buy, 100, 5U), domain::Sequence{22U});
  ASSERT_TRUE(retry);
  auto result = retry.commit();
  EXPECT_TRUE(result);
  EXPECT_TRUE(book.empty());
  EXPECT_TRUE(book.validate_invariants());
}

TEST(CommandExecutorPreparation, CommitPublishesThePrebuiltBatchAndMutationTogether) {
  InstrumentBook book{instrument_id};
  const auto passive = book.rest(resting_order(1U, 7U, domain::Side::sell, 100, 5U, 1U));
  ASSERT_TRUE(passive);
  CommandExecutor executor{book, {}, domain::Sequence{10U}};

  auto prepared =
      executor.prepare_at(limit_order(2U, 8U, domain::Side::buy, 100, 8U), domain::Sequence{20U});

  ASSERT_TRUE(prepared);
  ASSERT_NE(prepared.batch(), nullptr);
  EXPECT_EQ(prepared.batch()->size(), 4U);
  EXPECT_EQ(book.find(domain::OrderId{1U}), passive.node);
  EXPECT_EQ(passive.node->remaining_quantity(), domain::Quantity{5U});
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_TRUE(book.has_pending_preparation());

  auto result = prepared.commit();

  ASSERT_TRUE(result);
  ASSERT_TRUE(result.batch.has_value());
  EXPECT_EQ(result.batch->command_sequence(), domain::Sequence{20U});
  EXPECT_EQ(book.find(domain::OrderId{1U}), nullptr);
  const auto* residual = book.find(domain::OrderId{2U});
  ASSERT_NE(residual, nullptr);
  EXPECT_EQ(residual->remaining_quantity(), domain::Quantity{3U});
  EXPECT_EQ(residual->priority_sequence(), domain::Sequence{20U});
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_FALSE(book.has_pending_preparation());
  EXPECT_TRUE(book.validate_invariants());
}

TEST(CommandExecutorPreparation, RejectionHasAnInspectableBatchAndNoMutation) {
  InstrumentBook book{instrument_id};
  const auto rested = book.rest(resting_order(1U, 7U, domain::Side::buy, 100, 5U, 1U));
  ASSERT_TRUE(rested);
  CommandExecutor executor{book, {}, domain::Sequence{10U}};

  auto prepared =
      executor.prepare_at(limit_order(1U, 8U, domain::Side::sell, 101, 2U), domain::Sequence{20U});

  ASSERT_TRUE(prepared);
  EXPECT_TRUE(prepared.rejected());
  EXPECT_FALSE(prepared.committed());
  ASSERT_NE(prepared.batch(), nullptr);
  ASSERT_EQ(prepared.batch()->size(), 1U);
  const auto& rejected = std::get<domain::RejectedEvent>((*prepared.batch())[0]);
  EXPECT_EQ(rejected.reason, domain::RejectReason::duplicate_order_id);
  EXPECT_EQ(prepared.result().error, CommandExecutionError::none);
  EXPECT_EQ(book.find(domain::OrderId{1U}), rested.node);
  EXPECT_FALSE(book.has_pending_preparation());

  auto overlap = executor.prepare_at(cancel_order(1U, 7U), domain::Sequence{21U});
  EXPECT_FALSE(overlap);
  EXPECT_EQ(overlap.result().error, CommandExecutionError::preparation_in_progress);
  EXPECT_EQ(book.find(domain::OrderId{1U}), rested.node);

  auto result = prepared.commit();

  ASSERT_TRUE(result);
  ASSERT_TRUE(result.batch.has_value());
  EXPECT_EQ(domain::event_type((*result.batch)[0]), domain::EventType::rejected);
  EXPECT_EQ(book.find(domain::OrderId{1U}), rested.node);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_TRUE(book.validate_invariants());

  auto after_rejection = executor.prepare_at(cancel_order(1U, 7U), domain::Sequence{22U});
  EXPECT_TRUE(after_rejection);
}

TEST(CommandExecutorPreparation, MoveTransfersBatchAndRollbackResponsibility) {
  InstrumentBook book{instrument_id};
  CommandExecutor executor{book};
  auto original =
      executor.prepare_at(limit_order(1U, 7U, domain::Side::sell, 101, 5U), domain::Sequence{20U});
  ASSERT_TRUE(original);
  ASSERT_TRUE(book.has_pending_preparation());

  auto moved = std::move(original);

  EXPECT_FALSE(original);
  EXPECT_EQ(original.batch(), nullptr);
  ASSERT_TRUE(moved);
  ASSERT_NE(moved.batch(), nullptr);
  EXPECT_EQ(moved.batch()->command_sequence(), domain::Sequence{20U});
  EXPECT_TRUE(book.empty());
  EXPECT_TRUE(book.has_pending_preparation());

  auto result = moved.commit();

  ASSERT_TRUE(result);
  EXPECT_NE(book.find(domain::OrderId{1U}), nullptr);
  EXPECT_FALSE(book.has_pending_preparation());
  EXPECT_TRUE(book.validate_invariants());
}

TEST(CommandExecutorPreparation, AllocationFailureAfterStagingRollsBackBeforePropagation) {
  InstrumentBook book{instrument_id};
  const auto passive = book.rest(resting_order(1U, 7U, domain::Side::sell, 100, 5U, 1U));
  ASSERT_TRUE(passive);
  CommandExecutor executor{book, {}, domain::Sequence{10U}};

  executor.set_before_event_allocation_hook_for_testing(+[] { throw std::bad_alloc{}; });
  EXPECT_THROW(static_cast<void>(executor.prepare_at(
                   limit_order(2U, 8U, domain::Side::buy, 100, 8U), domain::Sequence{20U})),
               std::bad_alloc);
  executor.set_before_event_allocation_hook_for_testing(nullptr);

  EXPECT_EQ(book.find(domain::OrderId{1U}), passive.node);
  EXPECT_EQ(passive.node->remaining_quantity(), domain::Quantity{5U});
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_FALSE(book.has_pending_preparation());
  EXPECT_EQ(executor.next_sequence(), domain::Sequence{10U});
  EXPECT_TRUE(book.validate_invariants());

  auto retry =
      executor.prepare_at(limit_order(2U, 8U, domain::Side::buy, 100, 8U), domain::Sequence{20U});
  EXPECT_TRUE(retry);
}

}  // namespace
