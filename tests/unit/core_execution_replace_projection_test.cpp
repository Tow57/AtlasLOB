#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>

#include "execution_projection.hpp"
#include "instrument_book.hpp"
#include "match_plan.hpp"

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

domain::NewOrder replacement(std::uint64_t order_id, domain::Side side, std::int64_t price,
                             std::uint64_t quantity, std::uint32_t client_id = 11U) {
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

ProjectedRestingResidual residual_for(const domain::NewOrder& order, const MatchPlan& plan) {
  return {
      .price = order.limit_price.value(),
      .quantity = plan.residual_quantity,
  };
}

TEST(ExecutionProjectionReplace, SamePriceAndQuantityLeavesTheVisibleBookUnchanged) {
  InstrumentBook book{instrument_id};
  const auto old = book.rest(resting(1U, 11U, domain::Side::buy, 100, 5U, 1U));
  ASSERT_TRUE(old);
  ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::buy, 100, 7U, 2U)));
  ASSERT_TRUE(book.rest(resting(3U, 13U, domain::Side::sell, 110, 9U, 3U)));
  const auto before = snapshot_top_of_book(book);
  const auto order = replacement(10U, domain::Side::buy, 100, 5U);
  const auto planned = plan_matches(order, book);
  ASSERT_TRUE(planned);

  const auto projected =
      project_replace_top_of_book(book, make_cancel_projection_target(*old.node), order,
                                  planned.plan, residual_for(order, planned.plan));

  ASSERT_TRUE(projected);
  EXPECT_EQ(projected.snapshot, before);
  EXPECT_EQ(snapshot_top_of_book(book), before);
  EXPECT_EQ(old.node->remaining_quantity(), domain::Quantity{5U});
}

TEST(ExecutionProjectionReplace, FullyFilledBuyAndSellExposeTheNextSameSideLevel) {
  {
    InstrumentBook book{instrument_id};
    const auto old = book.rest(resting(1U, 11U, domain::Side::buy, 95, 4U, 1U));
    ASSERT_TRUE(old);
    ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::buy, 94, 6U, 2U)));
    ASSERT_TRUE(book.rest(resting(3U, 13U, domain::Side::sell, 100, 5U, 3U)));
    const auto order = replacement(10U, domain::Side::buy, 100, 5U);
    const auto planned = plan_matches(order, book);
    ASSERT_TRUE(planned);
    ASSERT_EQ(planned.plan.residual_disposition, ResidualDisposition::filled);

    const auto projected = project_replace_top_of_book(
        book, make_cancel_projection_target(*old.node), order, planned.plan);

    ASSERT_TRUE(projected);
    EXPECT_EQ(projected.snapshot.best_bid,
              (domain::TopOfBookLevel{domain::PriceTicks{94}, domain::Quantity{6U}}));
    EXPECT_FALSE(projected.snapshot.best_ask.has_value());
  }

  {
    InstrumentBook book{instrument_id};
    const auto old = book.rest(resting(1U, 11U, domain::Side::sell, 105, 4U, 1U));
    ASSERT_TRUE(old);
    ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::sell, 106, 6U, 2U)));
    ASSERT_TRUE(book.rest(resting(3U, 13U, domain::Side::buy, 100, 5U, 3U)));
    const auto order = replacement(10U, domain::Side::sell, 100, 5U);
    const auto planned = plan_matches(order, book);
    ASSERT_TRUE(planned);
    ASSERT_EQ(planned.plan.residual_disposition, ResidualDisposition::filled);

    const auto projected = project_replace_top_of_book(
        book, make_cancel_projection_target(*old.node), order, planned.plan);

    ASSERT_TRUE(projected);
    EXPECT_FALSE(projected.snapshot.best_bid.has_value());
    EXPECT_EQ(projected.snapshot.best_ask,
              (domain::TopOfBookLevel{domain::PriceTicks{106}, domain::Quantity{6U}}));
  }
}

TEST(ExecutionProjectionReplace, RemovingAndRestingAtNonbestPricesLeavesBboUnchanged) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::buy, 105, 3U, 1U)));
  const auto old = book.rest(resting(2U, 12U, domain::Side::buy, 100, 7U, 2U));
  ASSERT_TRUE(old);
  ASSERT_TRUE(book.rest(resting(3U, 13U, domain::Side::sell, 110, 8U, 3U)));
  const auto before = snapshot_top_of_book(book);
  const auto order = replacement(10U, domain::Side::buy, 99, 2U, 12U);
  const auto planned = plan_matches(order, book);
  ASSERT_TRUE(planned);

  const auto projected =
      project_replace_top_of_book(book, make_cancel_projection_target(*old.node), order,
                                  planned.plan, residual_for(order, planned.plan));

  ASSERT_TRUE(projected);
  EXPECT_EQ(projected.snapshot, before);
}

TEST(ExecutionProjectionReplace, SameNonbestLevelGetsAggregateReliefBeforeCheckedAddition) {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::buy, 101, 1U, 1U)));
  const auto old = book.rest(resting(2U, 12U, domain::Side::buy, 100, 10U, 2U));
  ASSERT_TRUE(old);
  ASSERT_TRUE(book.rest(resting(3U, 13U, domain::Side::buy, 100, maximum - 10U, 3U)));
  ASSERT_TRUE(book.rest(resting(4U, 14U, domain::Side::sell, 110, 1U, 4U)));
  const auto before = snapshot_top_of_book(book);

  const auto equal_order = replacement(10U, domain::Side::buy, 100, 10U, 12U);
  const auto equal_plan = plan_matches(equal_order, book);
  ASSERT_TRUE(equal_plan);
  const auto relieved =
      project_replace_top_of_book(book, make_cancel_projection_target(*old.node), equal_order,
                                  equal_plan.plan, residual_for(equal_order, equal_plan.plan));
  ASSERT_TRUE(relieved);
  EXPECT_EQ(relieved.snapshot, before);

  const auto overflowing_order = replacement(11U, domain::Side::buy, 100, 11U, 12U);
  const auto overflowing_plan = plan_matches(overflowing_order, book);
  ASSERT_TRUE(overflowing_plan);
  const auto overflowed = project_replace_top_of_book(
      book, make_cancel_projection_target(*old.node), overflowing_order, overflowing_plan.plan,
      residual_for(overflowing_order, overflowing_plan.plan));
  EXPECT_EQ(overflowed.error, ExecutionProjectionError::aggregate_overflow);
  EXPECT_EQ(snapshot_top_of_book(book), before);
}

TEST(ExecutionProjectionReplace, RejectsStaleTargetsPlansAndMalformedReplacementOrders) {
  InstrumentBook book{instrument_id};
  const auto old = book.rest(resting(1U, 11U, domain::Side::buy, 95, 4U, 1U));
  ASSERT_TRUE(old);
  ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::sell, 100, 5U, 2U)));
  const auto before = snapshot_top_of_book(book);
  const auto order = replacement(10U, domain::Side::buy, 100, 3U);
  const auto planned = plan_matches(order, book);
  ASSERT_TRUE(planned);

  auto stale_target = make_cancel_projection_target(*old.node);
  stale_target.remaining_quantity = domain::Quantity{3U};
  EXPECT_EQ(project_replace_top_of_book(book, stale_target, order, planned.plan).error,
            ExecutionProjectionError::cancel_target_mismatch);

  auto stale_plan = planned.plan;
  stale_plan.trades.front().resting_order_id = domain::OrderId{999U};
  EXPECT_EQ(
      project_replace_top_of_book(book, make_cancel_projection_target(*old.node), order, stale_plan)
          .error,
      ExecutionProjectionError::plan_book_mismatch);

  auto malformed_plan = planned.plan;
  malformed_plan.trades.front().aggressor_remaining = domain::Quantity{1U};
  EXPECT_EQ(project_replace_top_of_book(book, make_cancel_projection_target(*old.node), order,
                                        malformed_plan)
                .error,
            ExecutionProjectionError::invalid_plan);

  auto reused_old_id = order;
  reused_old_id.order_id = old.node->order_id();
  EXPECT_EQ(project_replace_top_of_book(book, make_cancel_projection_target(*old.node),
                                        reused_old_id, planned.plan)
                .error,
            ExecutionProjectionError::replacement_order_mismatch);

  auto wrong_client = order;
  wrong_client.client_id = domain::ClientId{99U};
  EXPECT_EQ(project_replace_top_of_book(book, make_cancel_projection_target(*old.node),
                                        wrong_client, planned.plan)
                .error,
            ExecutionProjectionError::replacement_order_mismatch);

  EXPECT_EQ(snapshot_top_of_book(book), before);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(ExecutionProjectionReplace, RejectsAPlanThatWouldLeaveTheBookCrossed) {
  InstrumentBook book{instrument_id};
  const auto old = book.rest(resting(1U, 11U, domain::Side::buy, 90, 1U, 1U));
  ASSERT_TRUE(old);
  ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::sell, 100, 5U, 2U)));
  const auto order = replacement(10U, domain::Side::buy, 100, 2U);
  const MatchPlan incomplete_plan{
      .trades = {},
      .residual_quantity = domain::Quantity{2U},
      .residual_disposition = ResidualDisposition::rest,
  };

  const auto projected =
      project_replace_top_of_book(book, make_cancel_projection_target(*old.node), order,
                                  incomplete_plan, residual_for(order, incomplete_plan));

  EXPECT_EQ(projected.error, ExecutionProjectionError::crossed_book);
}

TEST(ExecutionProjectionReplace, RejectsAnUnexpectedResidualAndWrongSide) {
  InstrumentBook book{instrument_id};
  const auto old = book.rest(resting(1U, 11U, domain::Side::sell, 105, 4U, 1U));
  ASSERT_TRUE(old);
  const auto order = replacement(10U, domain::Side::sell, 106, 3U);
  const auto planned = plan_matches(order, book);
  ASSERT_TRUE(planned);

  auto wrong_price = residual_for(order, planned.plan);
  wrong_price.price = domain::PriceTicks{107};
  EXPECT_EQ(project_replace_top_of_book(book, make_cancel_projection_target(*old.node), order,
                                        planned.plan, wrong_price)
                .error,
            ExecutionProjectionError::invalid_resting_residual);

  auto wrong_side = order;
  wrong_side.side = domain::Side::buy;
  EXPECT_EQ(project_replace_top_of_book(book, make_cancel_projection_target(*old.node), wrong_side,
                                        planned.plan)
                .error,
            ExecutionProjectionError::replacement_order_mismatch);
}

}  // namespace
