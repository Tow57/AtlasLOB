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

domain::NewOrder incoming(std::uint64_t order_id, domain::Side side, domain::OrderType type,
                          domain::TimeInForce time_in_force,
                          std::optional<domain::PriceTicks> price, std::uint64_t quantity) {
  return {
      .client_id = domain::ClientId{99U},
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
      .side = side,
      .order_type = type,
      .time_in_force = time_in_force,
      .limit_price = price,
      .quantity = domain::Quantity{quantity},
  };
}

ProjectedRestingResidual residual_at(std::int64_t price, const MatchPlan& plan) {
  return {
      .price = domain::PriceTicks{price},
      .quantity = plan.residual_quantity,
  };
}

TEST(ExecutionProjectionNew, SweepsMultipleAskLevelsAndLeavesAPartialNewBest) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::buy, 90, 17U, 1U)));
  ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::sell, 100, 5U, 2U)));
  ASSERT_TRUE(book.rest(resting(3U, 13U, domain::Side::sell, 100, 7U, 3U)));
  ASSERT_TRUE(book.rest(resting(4U, 14U, domain::Side::sell, 101, 11U, 4U)));
  ASSERT_TRUE(book.rest(resting(5U, 15U, domain::Side::sell, 102, 13U, 5U)));
  const auto before = snapshot_top_of_book(book);
  const auto order = incoming(100U, domain::Side::buy, domain::OrderType::limit,
                              domain::TimeInForce::gtc, domain::PriceTicks{101}, 20U);
  const auto plan = plan_matches(order, book);
  ASSERT_TRUE(plan);

  const auto projected = project_new_top_of_book(book, order.side, plan.plan);

  ASSERT_TRUE(projected);
  EXPECT_EQ(projected.snapshot.best_bid, before.best_bid);
  EXPECT_EQ(projected.snapshot.best_ask, (domain::TopOfBookLevel{
                                             .price = domain::PriceTicks{101},
                                             .aggregate_quantity = domain::Quantity{3U},
                                         }));
  EXPECT_EQ(snapshot_top_of_book(book), before);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(ExecutionProjectionNew, SweepsBidLevelsInDescendingPriceOrder) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::buy, 102, 4U, 1U)));
  ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::buy, 101, 6U, 2U)));
  ASSERT_TRUE(book.rest(resting(3U, 13U, domain::Side::buy, 100, 8U, 3U)));
  ASSERT_TRUE(book.rest(resting(4U, 14U, domain::Side::sell, 110, 9U, 4U)));
  const auto order = incoming(100U, domain::Side::sell, domain::OrderType::limit,
                              domain::TimeInForce::gtc, domain::PriceTicks{101}, 7U);
  const auto plan = plan_matches(order, book);
  ASSERT_TRUE(plan);

  const auto projected = project_new_top_of_book(book, order.side, plan.plan);

  ASSERT_TRUE(projected);
  EXPECT_EQ(projected.snapshot.best_bid, (domain::TopOfBookLevel{
                                             .price = domain::PriceTicks{101},
                                             .aggregate_quantity = domain::Quantity{3U},
                                         }));
  EXPECT_EQ(projected.snapshot.best_ask, (domain::TopOfBookLevel{
                                             .price = domain::PriceTicks{110},
                                             .aggregate_quantity = domain::Quantity{9U},
                                         }));
}

TEST(ExecutionProjectionNew, RemovesTheEntireOppositeBook) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::sell, 100, 5U, 1U)));
  ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::sell, 101, 7U, 2U)));
  const auto order = incoming(100U, domain::Side::buy, domain::OrderType::market,
                              domain::TimeInForce::ioc, std::nullopt, 20U);
  const auto plan = plan_matches(order, book);
  ASSERT_TRUE(plan);

  const auto projected = project_new_top_of_book(book, order.side, plan.plan);

  ASSERT_TRUE(projected);
  EXPECT_FALSE(projected.snapshot.best_bid.has_value());
  EXPECT_FALSE(projected.snapshot.best_ask.has_value());
}

TEST(ExecutionProjectionNew, ProjectsBetterEqualAndWorseRestingBuyResiduals) {
  {
    InstrumentBook book{instrument_id};
    ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::buy, 100, 10U, 1U)));
    ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::sell, 110, 10U, 2U)));
    const auto order = incoming(100U, domain::Side::buy, domain::OrderType::limit,
                                domain::TimeInForce::gtc, domain::PriceTicks{105}, 3U);
    const auto plan = plan_matches(order, book);
    ASSERT_TRUE(plan);
    const auto projected =
        project_new_top_of_book(book, order.side, plan.plan, residual_at(105, plan.plan));
    ASSERT_TRUE(projected);
    EXPECT_EQ(projected.snapshot.best_bid,
              (domain::TopOfBookLevel{domain::PriceTicks{105}, domain::Quantity{3U}}));
  }
  {
    InstrumentBook book{instrument_id};
    ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::buy, 100, 10U, 1U)));
    const auto order = incoming(100U, domain::Side::buy, domain::OrderType::limit,
                                domain::TimeInForce::gtc, domain::PriceTicks{100}, 3U);
    const auto plan = plan_matches(order, book);
    ASSERT_TRUE(plan);
    const auto projected =
        project_new_top_of_book(book, order.side, plan.plan, residual_at(100, plan.plan));
    ASSERT_TRUE(projected);
    EXPECT_EQ(projected.snapshot.best_bid,
              (domain::TopOfBookLevel{domain::PriceTicks{100}, domain::Quantity{13U}}));
  }
  {
    InstrumentBook book{instrument_id};
    ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::buy, 100, 10U, 1U)));
    ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::buy, 98, 20U, 2U)));
    const auto order = incoming(100U, domain::Side::buy, domain::OrderType::limit,
                                domain::TimeInForce::gtc, domain::PriceTicks{98}, 3U);
    const auto plan = plan_matches(order, book);
    ASSERT_TRUE(plan);
    const auto projected =
        project_new_top_of_book(book, order.side, plan.plan, residual_at(98, plan.plan));
    ASSERT_TRUE(projected);
    EXPECT_EQ(projected.snapshot.best_bid,
              (domain::TopOfBookLevel{domain::PriceTicks{100}, domain::Quantity{10U}}));
  }
  {
    InstrumentBook book{instrument_id};
    const auto order = incoming(100U, domain::Side::buy, domain::OrderType::limit,
                                domain::TimeInForce::gtc, domain::PriceTicks{99}, 3U);
    const auto plan = plan_matches(order, book);
    ASSERT_TRUE(plan);
    const auto projected =
        project_new_top_of_book(book, order.side, plan.plan, residual_at(99, plan.plan));
    ASSERT_TRUE(projected);
    EXPECT_EQ(projected.snapshot.best_bid,
              (domain::TopOfBookLevel{domain::PriceTicks{99}, domain::Quantity{3U}}));
  }
}

TEST(ExecutionProjectionNew, ProjectsEqualAndWorseRestingSellResiduals) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::sell, 105, 10U, 1U)));
  ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::sell, 110, 20U, 2U)));

  const auto equal_order = incoming(100U, domain::Side::sell, domain::OrderType::limit,
                                    domain::TimeInForce::gtc, domain::PriceTicks{105}, 3U);
  const auto equal_plan = plan_matches(equal_order, book);
  ASSERT_TRUE(equal_plan);
  const auto equal = project_new_top_of_book(book, equal_order.side, equal_plan.plan,
                                             residual_at(105, equal_plan.plan));
  ASSERT_TRUE(equal);
  EXPECT_EQ(equal.snapshot.best_ask,
            (domain::TopOfBookLevel{domain::PriceTicks{105}, domain::Quantity{13U}}));

  const auto worse_order = incoming(101U, domain::Side::sell, domain::OrderType::limit,
                                    domain::TimeInForce::gtc, domain::PriceTicks{110}, 3U);
  const auto worse_plan = plan_matches(worse_order, book);
  ASSERT_TRUE(worse_plan);
  const auto worse = project_new_top_of_book(book, worse_order.side, worse_plan.plan,
                                             residual_at(110, worse_plan.plan));
  ASSERT_TRUE(worse);
  EXPECT_EQ(worse.snapshot.best_ask,
            (domain::TopOfBookLevel{domain::PriceTicks{105}, domain::Quantity{10U}}));
}

TEST(ExecutionProjectionNew, DetectsAggregateOverflowAtBestAndNonbestResidualLevels) {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  {
    InstrumentBook book{instrument_id};
    ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::buy, 100, maximum, 1U)));
    const auto order = incoming(100U, domain::Side::buy, domain::OrderType::limit,
                                domain::TimeInForce::gtc, domain::PriceTicks{100}, 1U);
    const auto plan = plan_matches(order, book);
    ASSERT_TRUE(plan);
    const auto projected =
        project_new_top_of_book(book, order.side, plan.plan, residual_at(100, plan.plan));
    EXPECT_EQ(projected.error, ExecutionProjectionError::aggregate_overflow);
  }
  {
    InstrumentBook book{instrument_id};
    ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::buy, 101, 1U, 1U)));
    ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::buy, 100, maximum, 2U)));
    const auto order = incoming(100U, domain::Side::buy, domain::OrderType::limit,
                                domain::TimeInForce::gtc, domain::PriceTicks{100}, 1U);
    const auto plan = plan_matches(order, book);
    ASSERT_TRUE(plan);
    const auto projected =
        project_new_top_of_book(book, order.side, plan.plan, residual_at(100, plan.plan));
    EXPECT_EQ(projected.error, ExecutionProjectionError::aggregate_overflow);
  }
}

TEST(ExecutionProjectionNew, RejectsAResidualThatWouldLeaveTheBookCrossed) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::sell, 100, 5U, 1U)));
  const MatchPlan invalid_plan{
      .trades = {},
      .residual_quantity = domain::Quantity{1U},
      .residual_disposition = ResidualDisposition::rest,
  };

  const auto projected = project_new_top_of_book(book, domain::Side::buy, invalid_plan,
                                                 ProjectedRestingResidual{
                                                     .price = domain::PriceTicks{100},
                                                     .quantity = domain::Quantity{1U},
                                                 });

  EXPECT_EQ(projected.error, ExecutionProjectionError::crossed_book);
}

TEST(ExecutionProjectionNew, RejectsMalformedAndStalePlansWithoutMutation) {
  InstrumentBook book{instrument_id};
  const auto rested = book.rest(resting(1U, 11U, domain::Side::sell, 100, 5U, 1U));
  ASSERT_TRUE(rested);
  const auto order = incoming(100U, domain::Side::buy, domain::OrderType::limit,
                              domain::TimeInForce::gtc, domain::PriceTicks{100}, 3U);
  const auto planned = plan_matches(order, book);
  ASSERT_TRUE(planned);
  const auto before = snapshot_top_of_book(book);

  auto malformed = planned.plan;
  malformed.trades[0].resting_remaining_after = domain::Quantity{1U};
  EXPECT_EQ(project_new_top_of_book(book, order.side, malformed).error,
            ExecutionProjectionError::invalid_plan);

  auto stale = planned.plan;
  stale.trades[0].resting_order_id = domain::OrderId{999U};
  EXPECT_EQ(project_new_top_of_book(book, order.side, stale).error,
            ExecutionProjectionError::plan_book_mismatch);

  EXPECT_EQ(project_new_top_of_book(book, static_cast<domain::Side>(0U), planned.plan).error,
            ExecutionProjectionError::invalid_side);
  EXPECT_EQ(snapshot_top_of_book(book), before);
  EXPECT_EQ(rested.node->remaining_quantity(), domain::Quantity{5U});
}

TEST(ExecutionProjectionNew, RejectsMissingUnexpectedAndMismatchedRestingResiduals) {
  InstrumentBook book{instrument_id};
  const auto order = incoming(100U, domain::Side::buy, domain::OrderType::limit,
                              domain::TimeInForce::gtc, domain::PriceTicks{100}, 3U);
  const auto planned = plan_matches(order, book);
  ASSERT_TRUE(planned);

  EXPECT_EQ(project_new_top_of_book(book, order.side, planned.plan).error,
            ExecutionProjectionError::invalid_resting_residual);
  EXPECT_EQ(project_new_top_of_book(book, order.side, planned.plan,
                                    ProjectedRestingResidual{
                                        .price = domain::PriceTicks{100},
                                        .quantity = domain::Quantity{2U},
                                    })
                .error,
            ExecutionProjectionError::invalid_resting_residual);

  MatchPlan no_residual{
      .trades = {PlannedTrade{
          .resting_order_id = domain::OrderId{1U},
          .resting_client_id = domain::ClientId{1U},
          .execution_price = domain::PriceTicks{100},
          .execution_quantity = domain::Quantity{1U},
          .aggressor_remaining = domain::Quantity{},
          .resting_remaining_before = domain::Quantity{1U},
          .resting_remaining_after = domain::Quantity{},
          .expected_resting_priority = domain::Sequence{1U},
      }},
      .residual_quantity = domain::Quantity{},
      .residual_disposition = ResidualDisposition::filled,
  };
  EXPECT_EQ(project_new_top_of_book(book, order.side, no_residual,
                                    ProjectedRestingResidual{
                                        .price = domain::PriceTicks{100},
                                        .quantity = domain::Quantity{1U},
                                    })
                .error,
            ExecutionProjectionError::invalid_resting_residual);
}

TEST(ExecutionProjectionCancel, ReducesBestAggregateWithoutChangingItsPrice) {
  InstrumentBook book{instrument_id};
  const auto first = book.rest(resting(1U, 11U, domain::Side::buy, 100, 5U, 1U));
  const auto second = book.rest(resting(2U, 12U, domain::Side::buy, 100, 7U, 2U));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);

  const auto projected =
      project_cancel_top_of_book(book, make_cancel_projection_target(*first.node));

  ASSERT_TRUE(projected);
  EXPECT_EQ(projected.snapshot.best_bid,
            (domain::TopOfBookLevel{domain::PriceTicks{100}, domain::Quantity{7U}}));
  EXPECT_EQ(book.best_bid()->aggregate_quantity(), domain::Quantity{12U});
}

TEST(ExecutionProjectionCancel, AdvancesToTheNextBestLevelForBothSides) {
  {
    InstrumentBook book{instrument_id};
    const auto best = book.rest(resting(1U, 11U, domain::Side::buy, 101, 5U, 1U));
    ASSERT_TRUE(best);
    ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::buy, 100, 7U, 2U)));
    const auto projected =
        project_cancel_top_of_book(book, make_cancel_projection_target(*best.node));
    ASSERT_TRUE(projected);
    EXPECT_EQ(projected.snapshot.best_bid,
              (domain::TopOfBookLevel{domain::PriceTicks{100}, domain::Quantity{7U}}));
  }
  {
    InstrumentBook book{instrument_id};
    const auto best = book.rest(resting(1U, 11U, domain::Side::sell, 100, 5U, 1U));
    ASSERT_TRUE(best);
    ASSERT_TRUE(book.rest(resting(2U, 12U, domain::Side::sell, 101, 7U, 2U)));
    const auto projected =
        project_cancel_top_of_book(book, make_cancel_projection_target(*best.node));
    ASSERT_TRUE(projected);
    EXPECT_EQ(projected.snapshot.best_ask,
              (domain::TopOfBookLevel{domain::PriceTicks{101}, domain::Quantity{7U}}));
  }
}

TEST(ExecutionProjectionCancel, LeavesTopUnchangedForANonbestTarget) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting(1U, 11U, domain::Side::buy, 101, 5U, 1U)));
  const auto nonbest = book.rest(resting(2U, 12U, domain::Side::buy, 100, 7U, 2U));
  ASSERT_TRUE(nonbest);
  const auto before = snapshot_top_of_book(book);

  const auto projected =
      project_cancel_top_of_book(book, make_cancel_projection_target(*nonbest.node));

  ASSERT_TRUE(projected);
  EXPECT_EQ(projected.snapshot, before);
}

TEST(ExecutionProjectionCancel, ClearsTheCanceledSidesOnlyLevel) {
  InstrumentBook book{instrument_id};
  const auto only_ask = book.rest(resting(1U, 11U, domain::Side::sell, 100, 5U, 1U));
  ASSERT_TRUE(only_ask);

  const auto projected =
      project_cancel_top_of_book(book, make_cancel_projection_target(*only_ask.node));

  ASSERT_TRUE(projected);
  EXPECT_FALSE(projected.snapshot.best_bid.has_value());
  EXPECT_FALSE(projected.snapshot.best_ask.has_value());
}

TEST(ExecutionProjectionCancel, RejectsInvalidAndStaleTargetsWithoutMutation) {
  InstrumentBook book{instrument_id};
  const auto rested = book.rest(resting(1U, 11U, domain::Side::buy, 100, 5U, 1U));
  ASSERT_TRUE(rested);
  const auto before = snapshot_top_of_book(book);

  auto stale_quantity = make_cancel_projection_target(*rested.node);
  stale_quantity.remaining_quantity = domain::Quantity{4U};
  EXPECT_EQ(project_cancel_top_of_book(book, stale_quantity).error,
            ExecutionProjectionError::cancel_target_mismatch);

  auto wrong_side = make_cancel_projection_target(*rested.node);
  wrong_side.side = domain::Side::sell;
  EXPECT_EQ(project_cancel_top_of_book(book, wrong_side).error,
            ExecutionProjectionError::cancel_target_mismatch);

  auto invalid_side = make_cancel_projection_target(*rested.node);
  invalid_side.side = static_cast<domain::Side>(0U);
  EXPECT_EQ(project_cancel_top_of_book(book, invalid_side).error,
            ExecutionProjectionError::invalid_side);

  EXPECT_EQ(snapshot_top_of_book(book), before);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(ExecutionProjectionVocabulary, HasStableStringsAndUnknownFallback) {
  EXPECT_EQ(to_string(ExecutionProjectionError::invalid_plan), "invalid_plan");
  EXPECT_EQ(to_string(ExecutionProjectionError::aggregate_overflow), "aggregate_overflow");
  EXPECT_EQ(to_string(static_cast<ExecutionProjectionError>(255U)), "unknown");
}

}  // namespace
