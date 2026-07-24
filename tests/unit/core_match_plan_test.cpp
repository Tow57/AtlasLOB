#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "instrument_book.hpp"
#include "match_plan.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::core;

constexpr domain::InstrumentId instrument_id{1U};

OrderNodeSpec resting_spec(std::uint64_t order_id, std::uint32_t client_id, domain::Side side,
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

TEST(MatchPlan, SweepsBuySideByBestPriceThenFifoAndPartiallyFillsTheLastOrder) {
  InstrumentBook book{instrument_id};
  const auto first = book.rest(resting_spec(1U, 11U, domain::Side::sell, 100, 5U, 1U));
  const auto second = book.rest(resting_spec(2U, 12U, domain::Side::sell, 100, 7U, 2U));
  const auto third = book.rest(resting_spec(3U, 13U, domain::Side::sell, 101, 11U, 3U));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(third);

  const auto result = plan_matches(incoming(100U, domain::Side::buy, domain::OrderType::limit,
                                            domain::TimeInForce::gtc, domain::PriceTicks{101}, 20U),
                                   book);

  ASSERT_TRUE(result);
  EXPECT_EQ(result.plan.trades.size(), 3U);
  EXPECT_EQ(result.plan.trades[0], (PlannedTrade{
                                       .resting_order_id = domain::OrderId{1U},
                                       .resting_client_id = domain::ClientId{11U},
                                       .execution_price = domain::PriceTicks{100},
                                       .execution_quantity = domain::Quantity{5U},
                                       .aggressor_remaining = domain::Quantity{15U},
                                       .resting_remaining_before = domain::Quantity{5U},
                                       .resting_remaining_after = domain::Quantity{},
                                       .expected_resting_priority = domain::Sequence{1U},
                                   }));
  EXPECT_EQ(result.plan.trades[1].resting_order_id, domain::OrderId{2U});
  EXPECT_EQ(result.plan.trades[1].execution_quantity, domain::Quantity{7U});
  EXPECT_EQ(result.plan.trades[1].aggressor_remaining, domain::Quantity{8U});
  EXPECT_EQ(result.plan.trades[2].resting_order_id, domain::OrderId{3U});
  EXPECT_EQ(result.plan.trades[2].execution_quantity, domain::Quantity{8U});
  EXPECT_EQ(result.plan.trades[2].resting_remaining_after, domain::Quantity{3U});
  EXPECT_EQ(result.plan.residual_quantity, domain::Quantity{});
  EXPECT_EQ(result.plan.residual_disposition, ResidualDisposition::filled);

  EXPECT_EQ(first.node->remaining_quantity(), domain::Quantity{5U});
  EXPECT_EQ(second.node->remaining_quantity(), domain::Quantity{7U});
  EXPECT_EQ(third.node->remaining_quantity(), domain::Quantity{11U});
  EXPECT_EQ(book.active_order_count(), 3U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(MatchPlan, SellLimitHonorsTheInclusiveLimitAndStopsBeforeTheNextPrice) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting_spec(1U, 11U, domain::Side::buy, 101, 4U, 1U)));
  ASSERT_TRUE(book.rest(resting_spec(2U, 12U, domain::Side::buy, 100, 6U, 2U)));
  ASSERT_TRUE(book.rest(resting_spec(3U, 13U, domain::Side::buy, 99, 8U, 3U)));

  const auto result = plan_matches(incoming(100U, domain::Side::sell, domain::OrderType::limit,
                                            domain::TimeInForce::gtc, domain::PriceTicks{100}, 15U),
                                   book);

  ASSERT_TRUE(result);
  ASSERT_EQ(result.plan.trades.size(), 2U);
  EXPECT_EQ(result.plan.trades[0].execution_price, domain::PriceTicks{101});
  EXPECT_EQ(result.plan.trades[1].execution_price, domain::PriceTicks{100});
  EXPECT_EQ(result.plan.residual_quantity, domain::Quantity{5U});
  EXPECT_EQ(result.plan.residual_disposition, ResidualDisposition::rest);
  EXPECT_EQ(book.active_order_count(), 3U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(MatchPlan, DistinguishesIocCancellationAndMarketExhaustion) {
  InstrumentBook ioc_book{instrument_id};
  ASSERT_TRUE(ioc_book.rest(resting_spec(1U, 11U, domain::Side::sell, 100, 3U, 1U)));

  const auto ioc = plan_matches(incoming(100U, domain::Side::buy, domain::OrderType::limit,
                                         domain::TimeInForce::ioc, domain::PriceTicks{100}, 8U),
                                ioc_book);
  ASSERT_TRUE(ioc);
  EXPECT_EQ(ioc.plan.trades.size(), 1U);
  EXPECT_EQ(ioc.plan.residual_quantity, domain::Quantity{5U});
  EXPECT_EQ(ioc.plan.residual_disposition, ResidualDisposition::ioc_canceled);

  InstrumentBook market_book{instrument_id};
  ASSERT_TRUE(market_book.rest(resting_spec(2U, 12U, domain::Side::buy, 99, 2U, 1U)));
  const auto market = plan_matches(incoming(101U, domain::Side::sell, domain::OrderType::market,
                                            domain::TimeInForce::ioc, std::nullopt, 7U),
                                   market_book);
  ASSERT_TRUE(market);
  EXPECT_EQ(market.plan.trades.size(), 1U);
  EXPECT_EQ(market.plan.residual_quantity, domain::Quantity{5U});
  EXPECT_EQ(market.plan.residual_disposition, ResidualDisposition::market_exhausted);

  const InstrumentBook empty_book{instrument_id};
  const auto empty_market =
      plan_matches(incoming(102U, domain::Side::buy, domain::OrderType::market,
                            domain::TimeInForce::ioc, std::nullopt, 7U),
                   empty_book);
  ASSERT_TRUE(empty_market);
  EXPECT_TRUE(empty_market.plan.trades.empty());
  EXPECT_EQ(empty_market.plan.residual_quantity, domain::Quantity{7U});
  EXPECT_EQ(empty_market.plan.residual_disposition, ResidualDisposition::market_exhausted);
}

TEST(MatchPlan, NonmarketableGtcLimitProducesARestingResidualWithoutMutation) {
  InstrumentBook book{instrument_id};
  const auto ask = book.rest(resting_spec(1U, 11U, domain::Side::sell, 105, 9U, 1U));
  ASSERT_TRUE(ask);
  const auto* const best_before = book.best_ask();

  const auto result = plan_matches(incoming(100U, domain::Side::buy, domain::OrderType::limit,
                                            domain::TimeInForce::gtc, domain::PriceTicks{104}, 6U),
                                   book);

  ASSERT_TRUE(result);
  EXPECT_TRUE(result.plan.trades.empty());
  EXPECT_EQ(result.plan.residual_quantity, domain::Quantity{6U});
  EXPECT_EQ(result.plan.residual_disposition, ResidualDisposition::rest);
  EXPECT_EQ(book.best_ask(), best_before);
  EXPECT_EQ(ask.node->remaining_quantity(), domain::Quantity{9U});
  EXPECT_TRUE(book.validate_invariants());
}

TEST(MatchPlan, RejectsInvalidInstrumentDuplicateAndUnsupportedFokWithoutMutation) {
  InstrumentBook book{instrument_id};
  const auto rested = book.rest(resting_spec(1U, 11U, domain::Side::buy, 100, 5U, 1U));
  ASSERT_TRUE(rested);
  const auto* const best_before = book.best_bid();

  auto invalid = incoming(100U, domain::Side::sell, domain::OrderType::limit,
                          domain::TimeInForce::gtc, domain::PriceTicks{100}, 0U);
  const auto invalid_result = plan_matches(invalid, book);
  EXPECT_FALSE(invalid_result);
  EXPECT_EQ(invalid_result.error, MatchPlanError::invalid_request);
  EXPECT_EQ(invalid_result.validation_reason, domain::RejectReason::invalid_quantity);

  auto wrong_instrument = incoming(101U, domain::Side::sell, domain::OrderType::limit,
                                   domain::TimeInForce::gtc, domain::PriceTicks{100}, 1U);
  wrong_instrument.instrument_id = domain::InstrumentId{2U};
  const auto instrument_result = plan_matches(wrong_instrument, book);
  EXPECT_FALSE(instrument_result);
  EXPECT_EQ(instrument_result.error, MatchPlanError::instrument_mismatch);

  const auto duplicate =
      plan_matches(incoming(1U, domain::Side::sell, domain::OrderType::limit,
                            domain::TimeInForce::gtc, domain::PriceTicks{100}, 1U),
                   book);
  EXPECT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error, MatchPlanError::duplicate_order_id);

  const auto fok = plan_matches(incoming(102U, domain::Side::sell, domain::OrderType::limit,
                                         domain::TimeInForce::fok, domain::PriceTicks{100}, 1U),
                                book);
  EXPECT_FALSE(fok);
  EXPECT_EQ(fok.error, MatchPlanError::invalid_request);
  EXPECT_EQ(fok.validation_reason, domain::RejectReason::unsupported_time_in_force);

  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_EQ(book.best_bid(), best_before);
  EXPECT_EQ(rested.node->remaining_quantity(), domain::Quantity{5U});
  EXPECT_TRUE(book.validate_invariants());
}

TEST(MatchPlanCapacity, ProjectsTerminalPassivesResidualsAndReplacementRemoval) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(resting_spec(1U, 11U, domain::Side::sell, 100, 5U, 1U)));
  ASSERT_TRUE(book.rest(resting_spec(2U, 12U, domain::Side::sell, 100, 7U, 2U)));
  ASSERT_TRUE(book.rest(resting_spec(3U, 13U, domain::Side::sell, 101, 11U, 3U)));
  ASSERT_TRUE(book.rest(resting_spec(4U, 14U, domain::Side::buy, 90, 9U, 4U)));

  const auto filled = plan_matches(incoming(100U, domain::Side::buy, domain::OrderType::limit,
                                            domain::TimeInForce::gtc, domain::PriceTicks{101}, 20U),
                                   book);
  ASSERT_TRUE(filled);
  const auto filled_projection =
      project_active_order_count(filled.plan, book.active_order_count(), false);
  ASSERT_TRUE(filled_projection);
  EXPECT_EQ(filled_projection.final_active_order_count, 2U);
  EXPECT_TRUE(filled_projection.within_limit(2U));

  const auto residual =
      plan_matches(incoming(101U, domain::Side::buy, domain::OrderType::limit,
                            domain::TimeInForce::gtc, domain::PriceTicks{100}, 20U),
                   book);
  ASSERT_TRUE(residual);
  const auto residual_projection =
      project_active_order_count(residual.plan, book.active_order_count(), false);
  ASSERT_TRUE(residual_projection);
  EXPECT_EQ(residual_projection.final_active_order_count, 3U);
  EXPECT_FALSE(residual_projection.within_limit(2U));

  const auto replacement_projection =
      project_active_order_count(residual.plan, book.active_order_count(), true);
  ASSERT_TRUE(replacement_projection);
  EXPECT_EQ(replacement_projection.final_active_order_count, 2U);
  EXPECT_TRUE(replacement_projection.within_limit(2U));
}

TEST(MatchPlanCapacity, DetectsMalformedUnderflowAndOverflowWithoutMutation) {
  MatchPlan malformed{
      .trades = {},
      .residual_quantity = domain::Quantity{},
      .residual_disposition = ResidualDisposition::rest,
  };
  EXPECT_EQ(project_active_order_count(malformed, 0U, false).error,
            ActiveOrderProjectionError::invalid_plan);
  malformed.residual_quantity = domain::Quantity{1U};
  malformed.residual_disposition = static_cast<ResidualDisposition>(255U);
  EXPECT_EQ(project_active_order_count(malformed, 0U, false).error,
            ActiveOrderProjectionError::invalid_plan);

  const MatchPlan impossible_terminal{
      .trades =
          {
              PlannedTrade{
                  .execution_quantity = domain::Quantity{5U},
                  .resting_remaining_before = domain::Quantity{5U},
                  .resting_remaining_after = domain::Quantity{},
              },
          },
      .residual_quantity = {},
      .residual_disposition = ResidualDisposition::filled,
  };
  EXPECT_EQ(project_active_order_count(impossible_terminal, 0U, false).error,
            ActiveOrderProjectionError::count_underflow);

  const MatchPlan resting{
      .trades = {},
      .residual_quantity = domain::Quantity{1U},
      .residual_disposition = ResidualDisposition::rest,
  };
  EXPECT_EQ(
      project_active_order_count(resting, std::numeric_limits<std::size_t>::max(), false).error,
      ActiveOrderProjectionError::count_overflow);
}

TEST(MatchPlanVocabulary, HasStableStringRepresentations) {
  EXPECT_EQ(to_string(ResidualDisposition::market_exhausted), "market_exhausted");
  EXPECT_EQ(to_string(MatchPlanError::book_invariant_violation), "book_invariant_violation");
  EXPECT_EQ(to_string(ActiveOrderProjectionError::count_overflow), "count_overflow");
  EXPECT_EQ(to_string(static_cast<ResidualDisposition>(0U)), "unknown");
  EXPECT_EQ(to_string(static_cast<MatchPlanError>(255U)), "unknown");
  EXPECT_EQ(to_string(static_cast<ActiveOrderProjectionError>(255U)), "unknown");
}

}  // namespace
