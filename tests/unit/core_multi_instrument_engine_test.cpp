#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "atlaslob/matching_engine.hpp"
#include "atlaslob/multi_instrument_engine.hpp"

namespace {

using namespace atlaslob;

constexpr domain::InstrumentId first_instrument{7U};
constexpr domain::InstrumentId second_instrument{9U};
constexpr domain::InstrumentId unknown_instrument{99U};
constexpr domain::ClientId first_client{11U};
constexpr domain::ClientId second_client{22U};

static_assert(!std::is_copy_constructible_v<MultiInstrumentEngine>);
static_assert(!std::is_copy_assignable_v<MultiInstrumentEngine>);
static_assert(!std::is_move_constructible_v<MultiInstrumentEngine>);
static_assert(!std::is_move_assignable_v<MultiInstrumentEngine>);
static_assert(std::is_same_v<decltype(std::declval<const MultiInstrumentEngine&>()
                                          .contains_instrument(first_instrument)),
                             bool>);
static_assert(
    std::is_same_v<decltype(std::declval<const MultiInstrumentEngine&>().active_order_count()),
                   std::size_t>);
static_assert(
    std::is_same_v<decltype(std::declval<const MultiInstrumentEngine&>().top(first_instrument)),
                   std::optional<BookTop>>);
static_assert(std::is_same_v<
              decltype(std::declval<const MultiInstrumentEngine&>().snapshot(first_instrument)),
              std::optional<InstrumentSnapshot>>);
static_assert(std::is_same_v<decltype(std::declval<const MultiInstrumentEngine&>().snapshot()),
                             EngineSnapshot>);
static_assert(std::is_same_v<decltype(std::declval<const MultiInstrumentEngine&>().state_digest()),
                             Digest256>);
static_assert(std::is_same_v<decltype(std::declval<const MultiInstrumentEngine&>().next_sequence()),
                             domain::Sequence>);
static_assert(std::is_same_v<
              decltype(std::declval<const MultiInstrumentEngine&>().sequence_exhausted()), bool>);
static_assert(
    noexcept(std::declval<const MultiInstrumentEngine&>().contains_instrument(first_instrument)));
static_assert(noexcept(std::declval<const MultiInstrumentEngine&>().active_order_count()));
static_assert(noexcept(std::declval<const MultiInstrumentEngine&>().top(first_instrument)));
static_assert(noexcept(std::declval<const MultiInstrumentEngine&>().next_sequence()));
static_assert(noexcept(std::declval<const MultiInstrumentEngine&>().sequence_exhausted()));

[[nodiscard]] InstrumentConfig instrument_config(domain::InstrumentId instrument_id,
                                                 std::size_t max_active_orders = 8U,
                                                 std::uint64_t max_order_quantity = 1'000U,
                                                 std::int64_t tick_increment = 1) {
  return {
      .instrument_id = instrument_id,
      .matching =
          {
              .max_order_quantity = domain::Quantity{max_order_quantity},
              .tick_increment = domain::PriceTicks{tick_increment},
              .max_active_orders = max_active_orders,
          },
  };
}

[[nodiscard]] std::array<InstrumentConfig, 2U> standard_catalog(
    std::size_t first_max_active_orders = 8U, std::size_t second_max_active_orders = 8U) {
  return {
      instrument_config(first_instrument, first_max_active_orders),
      instrument_config(second_instrument, second_max_active_orders),
  };
}

[[nodiscard]] domain::NewOrder limit_order(
    std::uint64_t order_id, domain::InstrumentId instrument_id, domain::Side side,
    std::int64_t price, std::uint64_t quantity, domain::ClientId client_id = first_client,
    domain::TimeInForce time_in_force = domain::TimeInForce::gtc) {
  return {
      .client_id = client_id,
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
      .side = side,
      .order_type = domain::OrderType::limit,
      .time_in_force = time_in_force,
      .limit_price = domain::PriceTicks{price},
      .quantity = domain::Quantity{quantity},
  };
}

[[nodiscard]] domain::NewOrder market_order(std::uint64_t order_id,
                                            domain::InstrumentId instrument_id, domain::Side side,
                                            std::uint64_t quantity,
                                            domain::ClientId client_id = first_client) {
  return {
      .client_id = client_id,
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
      .side = side,
      .order_type = domain::OrderType::market,
      .time_in_force = domain::TimeInForce::ioc,
      .limit_price = std::nullopt,
      .quantity = domain::Quantity{quantity},
  };
}

[[nodiscard]] domain::CancelOrder cancel_order(std::uint64_t order_id,
                                               domain::InstrumentId instrument_id,
                                               domain::ClientId client_id = first_client) {
  return {
      .client_id = client_id,
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
  };
}

[[nodiscard]] domain::ReplaceOrder replace_order(std::uint64_t old_order_id,
                                                 std::uint64_t new_order_id,
                                                 domain::InstrumentId instrument_id,
                                                 std::int64_t new_price = 101,
                                                 std::uint64_t new_quantity = 5U,
                                                 domain::ClientId client_id = first_client) {
  return {
      .client_id = client_id,
      .old_order_id = domain::OrderId{old_order_id},
      .new_order_id = domain::OrderId{new_order_id},
      .instrument_id = instrument_id,
      .new_limit_price = domain::PriceTicks{new_price},
      .new_quantity = domain::Quantity{new_quantity},
  };
}

[[nodiscard]] EngineSnapshot representative_engine_snapshot() {
  return {
      .semantics_version = atlaslob_semantics_version,
      .engine_config =
          {
              .max_total_active_orders = 10U,
          },
      .catalog =
          {
              instrument_config(first_instrument, 4U, 100U, 5),
              instrument_config(second_instrument, 6U, 200U, 10),
          },
      .last_sequence = domain::Sequence{3U},
      .sequence_exhausted = false,
      .active_order_count = 2U,
      .instruments =
          {
              InstrumentSnapshot{
                  .instrument_id = first_instrument,
                  .active_order_count = 1U,
                  .bids =
                      {
                          PriceLevelSnapshot{
                              .price = domain::PriceTicks{100},
                              .aggregate_quantity = domain::Quantity{5U},
                              .orders =
                                  {
                                      OrderSnapshot{
                                          .order_id = domain::OrderId{11U},
                                          .client_id = domain::ClientId{1U},
                                          .instrument_id = first_instrument,
                                          .side = domain::Side::buy,
                                          .price = domain::PriceTicks{100},
                                          .remaining_quantity = domain::Quantity{5U},
                                          .priority_sequence = domain::Sequence{1U},
                                      },
                                  },
                          },
                      },
                  .asks = {},
              },
              InstrumentSnapshot{
                  .instrument_id = second_instrument,
                  .active_order_count = 1U,
                  .bids = {},
                  .asks =
                      {
                          PriceLevelSnapshot{
                              .price = domain::PriceTicks{110},
                              .aggregate_quantity = domain::Quantity{7U},
                              .orders =
                                  {
                                      OrderSnapshot{
                                          .order_id = domain::OrderId{22U},
                                          .client_id = domain::ClientId{2U},
                                          .instrument_id = second_instrument,
                                          .side = domain::Side::sell,
                                          .price = domain::PriceTicks{110},
                                          .remaining_quantity = domain::Quantity{7U},
                                          .priority_sequence = domain::Sequence{3U},
                                      },
                                  },
                          },
                      },
              },
          },
  };
}

void expect_batch_identity(const EngineResult& result, domain::Sequence sequence,
                           domain::InstrumentId instrument_id) {
  ASSERT_NE(result.batch(), nullptr);
  EXPECT_EQ(result.batch()->command_sequence(), sequence);
  for (std::size_t index = 0U; index < result.batch()->size(); ++index) {
    EXPECT_EQ(domain::event_header((*result.batch())[index]),
              (domain::EventHeader{
                  .command_sequence = sequence,
                  .event_index = static_cast<std::uint32_t>(index),
                  .instrument_id = instrument_id,
              }))
        << "event_index=" << index;
  }
}

[[nodiscard]] const domain::RejectedEvent& rejection(const EngineResult& result) {
  return std::get<domain::RejectedEvent>((*result.batch())[0]);
}

void throw_bad_alloc() { throw std::bad_alloc{}; }

TEST(MultiInstrumentEngineConstruction, ValidatesAndCanonicalizesTheCatalog) {
  const std::array<InstrumentConfig, 0U> empty_catalog{};
  EXPECT_THROW(
      static_cast<void>(MultiInstrumentEngine{std::span<const InstrumentConfig>{empty_catalog}}),
      std::invalid_argument);

  const std::array zero_id_catalog{instrument_config(domain::InstrumentId{})};
  EXPECT_THROW(
      static_cast<void>(MultiInstrumentEngine{std::span<const InstrumentConfig>{zero_id_catalog}}),
      std::invalid_argument);

  const std::array duplicate_catalog{
      instrument_config(first_instrument),
      instrument_config(first_instrument),
  };
  EXPECT_THROW(static_cast<void>(
                   MultiInstrumentEngine{std::span<const InstrumentConfig>{duplicate_catalog}}),
               std::invalid_argument);

  auto invalid_quantity = instrument_config(first_instrument);
  invalid_quantity.matching.max_order_quantity = {};
  const std::array invalid_quantity_catalog{invalid_quantity};
  EXPECT_THROW(static_cast<void>(MultiInstrumentEngine{
                   std::span<const InstrumentConfig>{invalid_quantity_catalog}}),
               std::invalid_argument);

  auto zero_tick = instrument_config(first_instrument);
  zero_tick.matching.tick_increment = {};
  const std::array zero_tick_catalog{zero_tick};
  EXPECT_THROW(static_cast<void>(
                   MultiInstrumentEngine{std::span<const InstrumentConfig>{zero_tick_catalog}}),
               std::invalid_argument);

  auto negative_tick = instrument_config(first_instrument);
  negative_tick.matching.tick_increment = domain::PriceTicks{-5};
  const std::array negative_tick_catalog{negative_tick};
  EXPECT_THROW(static_cast<void>(
                   MultiInstrumentEngine{std::span<const InstrumentConfig>{negative_tick_catalog}}),
               std::invalid_argument);

  const std::array reverse_catalog{
      instrument_config(second_instrument, 3U, 200U, 5),
      instrument_config(first_instrument, 2U, 100U, 1),
  };
  const MultiInstrumentEngine engine{std::span<const InstrumentConfig>{reverse_catalog}};

  EXPECT_TRUE(engine.contains_instrument(first_instrument));
  EXPECT_TRUE(engine.contains_instrument(second_instrument));
  EXPECT_FALSE(engine.contains_instrument(unknown_instrument));
  const auto snapshot = engine.snapshot();
  ASSERT_EQ(snapshot.catalog.size(), 2U);
  EXPECT_EQ(snapshot.catalog[0].instrument_id, first_instrument);
  EXPECT_EQ(snapshot.catalog[0].matching.max_active_orders, 2U);
  EXPECT_EQ(snapshot.catalog[1].instrument_id, second_instrument);
  EXPECT_EQ(snapshot.catalog[1].matching.max_active_orders, 3U);
}

TEST(MultiInstrumentEngineObservers, FreshEngineContainsEveryCanonicalEmptyBook) {
  const auto catalog = standard_catalog();
  const MultiInstrumentEngine engine{std::span<const InstrumentConfig>{catalog}};

  EXPECT_EQ(engine.active_order_count(), 0U);
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{1U});
  EXPECT_FALSE(engine.sequence_exhausted());

  const auto first_top = engine.top(first_instrument);
  ASSERT_TRUE(first_top.has_value());
  EXPECT_EQ(*first_top, BookTop{});
  const auto second_top = engine.top(second_instrument);
  ASSERT_TRUE(second_top.has_value());
  EXPECT_EQ(*second_top, BookTop{});
  EXPECT_EQ(engine.top(unknown_instrument), std::nullopt);

  const auto first = engine.snapshot(first_instrument);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->instrument_id, first_instrument);
  EXPECT_EQ(first->active_order_count, 0U);
  EXPECT_TRUE(first->bids.empty());
  EXPECT_TRUE(first->asks.empty());
  EXPECT_EQ(engine.snapshot(unknown_instrument), std::nullopt);

  const auto all = engine.snapshot();
  EXPECT_EQ(all.semantics_version, atlaslob_semantics_version);
  EXPECT_EQ(all.last_sequence, domain::Sequence{});
  EXPECT_FALSE(all.sequence_exhausted);
  EXPECT_EQ(all.active_order_count, 0U);
  ASSERT_EQ(all.catalog.size(), 2U);
  EXPECT_EQ(all.catalog[0].instrument_id, first_instrument);
  EXPECT_EQ(all.catalog[1].instrument_id, second_instrument);
  ASSERT_EQ(all.instruments.size(), 2U);
  EXPECT_EQ(all.instruments[0].instrument_id, first_instrument);
  EXPECT_EQ(all.instruments[1].instrument_id, second_instrument);
}

TEST(MultiInstrumentEngineConfiguration, AppliesQuantityAndTickPoliciesFromTheRoutedInstrument) {
  const std::array catalog{
      instrument_config(first_instrument, 8U, 10U, 5),
      instrument_config(second_instrument, 8U, 100U, 1),
  };
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{catalog}};

  auto quantity_before_tick =
      engine.execute(limit_order(1U, first_instrument, domain::Side::buy, 101, 11U));
  ASSERT_TRUE(quantity_before_tick.rejected());
  EXPECT_EQ(rejection(quantity_before_tick).reason, domain::RejectReason::quantity_out_of_range);

  auto invalid_tick = engine.execute(limit_order(2U, first_instrument, domain::Side::buy, 101, 5U));
  ASSERT_TRUE(invalid_tick.rejected());
  EXPECT_EQ(rejection(invalid_tick).reason, domain::RejectReason::invalid_tick);

  auto valid_elsewhere =
      engine.execute(limit_order(3U, second_instrument, domain::Side::buy, 101, 11U));
  ASSERT_TRUE(valid_elsewhere.committed());
  EXPECT_EQ(engine.active_order_count(), 1U);
  EXPECT_EQ(engine.snapshot(first_instrument)->active_order_count, 0U);
  EXPECT_EQ(engine.snapshot(second_instrument)->active_order_count, 1U);
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{4U});
}

TEST(MultiInstrumentEngineSequencing,
     UsesOneGlobalSequenceAcrossCommitsDomainRejectsAndUnknownInstruments) {
  const auto catalog = standard_catalog();
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{catalog}};

  auto first = engine.execute(limit_order(1U, first_instrument, domain::Side::buy, 100, 5U));
  ASSERT_TRUE(first.committed());
  expect_batch_identity(first, domain::Sequence{1U}, first_instrument);

  auto invalid = limit_order(2U, second_instrument, domain::Side::buy, 100, 0U);
  auto pure_reject = engine.execute(invalid);
  ASSERT_TRUE(pure_reject.rejected());
  expect_batch_identity(pure_reject, domain::Sequence{2U}, second_instrument);
  EXPECT_EQ(rejection(pure_reject).reason, domain::RejectReason::invalid_quantity);

  auto unknown = engine.execute(limit_order(3U, unknown_instrument, domain::Side::buy, 100, 5U));
  ASSERT_TRUE(unknown.rejected());
  expect_batch_identity(unknown, domain::Sequence{3U}, unknown_instrument);
  EXPECT_EQ(rejection(unknown).reason, domain::RejectReason::unknown_instrument);
  EXPECT_EQ(rejection(unknown).order_id, domain::OrderId{3U});

  auto second = engine.execute(limit_order(4U, second_instrument, domain::Side::sell, 110, 7U));
  ASSERT_TRUE(second.committed());
  expect_batch_identity(second, domain::Sequence{4U}, second_instrument);

  EXPECT_EQ(engine.next_sequence(), domain::Sequence{5U});
  const auto snapshot = engine.snapshot();
  EXPECT_EQ(snapshot.last_sequence, domain::Sequence{4U});
  EXPECT_EQ(snapshot.active_order_count, 2U);
  ASSERT_EQ(snapshot.instruments.size(), 2U);
  EXPECT_EQ(snapshot.instruments[0].bids[0].orders[0].priority_sequence, domain::Sequence{1U});
  EXPECT_EQ(snapshot.instruments[1].asks[0].orders[0].priority_sequence, domain::Sequence{4U});
}

TEST(MultiInstrumentEngineSequencing,
     IssuesTheMaximumSequenceOnceThenReportsStickyGlobalExhaustion) {
  constexpr auto maximum_sequence = std::numeric_limits<std::uint64_t>::max();
  const auto catalog = standard_catalog();
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{catalog}};
  engine.set_next_sequence_for_testing(domain::Sequence{maximum_sequence});

  auto maximum = engine.execute(limit_order(1U, second_instrument, domain::Side::buy, 100, 5U));
  ASSERT_TRUE(maximum.committed());
  expect_batch_identity(maximum, domain::Sequence{maximum_sequence}, second_instrument);
  EXPECT_TRUE(engine.sequence_exhausted());
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{});
  EXPECT_EQ(engine.snapshot().last_sequence, domain::Sequence{maximum_sequence});
  EXPECT_TRUE(engine.snapshot().sequence_exhausted);

  const auto before = engine.snapshot();
  auto first_exhaustion = engine.execute(cancel_order(1U, second_instrument));
  auto sticky_exhaustion =
      engine.execute(limit_order(2U, first_instrument, domain::Side::buy, 90, 3U));

  EXPECT_FALSE(first_exhaustion.has_value());
  EXPECT_EQ(first_exhaustion.batch(), nullptr);
  EXPECT_EQ(first_exhaustion.error(), EngineError::sequence_exhausted);
  EXPECT_FALSE(sticky_exhaustion.has_value());
  EXPECT_EQ(sticky_exhaustion.batch(), nullptr);
  EXPECT_EQ(sticky_exhaustion.error(), EngineError::sequence_exhausted);
  EXPECT_EQ(engine.snapshot(), before);
  EXPECT_TRUE(engine.validate_invariants_for_testing());
}

TEST(MultiInstrumentEngineSequencing, ARejectionMayConsumeTheMaximumSequenceExactlyOnce) {
  constexpr auto maximum_sequence = std::numeric_limits<std::uint64_t>::max();
  const auto catalog = standard_catalog();
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{catalog}};
  engine.set_next_sequence_for_testing(domain::Sequence{maximum_sequence});

  auto maximum = engine.execute(limit_order(1U, unknown_instrument, domain::Side::buy, 100, 5U));

  ASSERT_TRUE(maximum.rejected());
  expect_batch_identity(maximum, domain::Sequence{maximum_sequence}, unknown_instrument);
  EXPECT_EQ(rejection(maximum).reason, domain::RejectReason::unknown_instrument);
  EXPECT_TRUE(engine.sequence_exhausted());
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{});
  EXPECT_EQ(engine.snapshot().last_sequence, domain::Sequence{maximum_sequence});
  EXPECT_EQ(engine.active_order_count(), 0U);

  auto exhausted = engine.execute(limit_order(2U, first_instrument, domain::Side::buy, 100, 5U));
  EXPECT_EQ(exhausted.error(), EngineError::sequence_exhausted);
  EXPECT_EQ(exhausted.batch(), nullptr);
  EXPECT_TRUE(engine.validate_invariants_for_testing());
}

TEST(MultiInstrumentEngineIdentity, RejectsAnActiveOrderIdGloballyWithoutMutatingEitherBook) {
  const auto catalog = standard_catalog();
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{catalog}};
  ASSERT_TRUE(
      engine.execute(limit_order(1U, first_instrument, domain::Side::buy, 100, 5U)).committed());
  const auto before = engine.snapshot();

  auto duplicate = engine.execute(
      limit_order(1U, second_instrument, domain::Side::sell, 110, 9U, second_client));

  ASSERT_TRUE(duplicate.rejected());
  expect_batch_identity(duplicate, domain::Sequence{2U}, second_instrument);
  EXPECT_EQ(rejection(duplicate).reason, domain::RejectReason::duplicate_order_id);
  EXPECT_EQ(rejection(duplicate).order_id, domain::OrderId{1U});
  EXPECT_EQ(engine.active_order_count(), 1U);
  EXPECT_EQ(engine.top(second_instrument), std::optional<BookTop>{BookTop{}});

  auto expected = before;
  expected.last_sequence = domain::Sequence{2U};
  EXPECT_EQ(engine.snapshot(), expected);
}

TEST(MultiInstrumentEnginePreparation,
     AllocationFailureRollsBackProvisionalIdentityAndReservedSequence) {
  const auto catalog = standard_catalog();
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{catalog}};
  const auto before = engine.snapshot();
  engine.set_before_event_allocation_hook_for_testing(first_instrument, &throw_bad_alloc);

  EXPECT_THROW(static_cast<void>(
                   engine.execute(limit_order(1U, first_instrument, domain::Side::buy, 100, 5U))),
               std::bad_alloc);

  engine.set_before_event_allocation_hook_for_testing(first_instrument, nullptr);
  EXPECT_EQ(engine.snapshot(), before);
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{1U});
  EXPECT_EQ(engine.active_order_count(), 0U);
  EXPECT_TRUE(engine.validate_invariants_for_testing());

  auto retry = engine.execute(limit_order(1U, second_instrument, domain::Side::buy, 90, 5U));
  ASSERT_TRUE(retry.committed());
  expect_batch_identity(retry, domain::Sequence{1U}, second_instrument);
}

TEST(MultiInstrumentEnginePreparation,
     ReplacementAllocationFailureRollsBackOldAndProvisionalIdentities) {
  const auto catalog = standard_catalog();
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{catalog}};
  ASSERT_TRUE(
      engine.execute(limit_order(1U, first_instrument, domain::Side::buy, 100, 5U)).committed());
  const auto before = engine.snapshot();
  engine.set_before_event_allocation_hook_for_testing(first_instrument, &throw_bad_alloc);

  EXPECT_THROW(static_cast<void>(engine.execute(replace_order(1U, 2U, first_instrument, 101, 7U))),
               std::bad_alloc);

  engine.set_before_event_allocation_hook_for_testing(first_instrument, nullptr);
  EXPECT_EQ(engine.snapshot(), before);
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{2U});
  EXPECT_EQ(engine.active_order_count(), 1U);
  EXPECT_TRUE(engine.validate_invariants_for_testing());

  auto retry = engine.execute(
      limit_order(2U, second_instrument, domain::Side::sell, 110, 4U, second_client));
  ASSERT_TRUE(retry.committed());
  expect_batch_identity(retry, domain::Sequence{2U}, second_instrument);
}

TEST(MultiInstrumentEngineIdentity,
     AppliesUnknownOwnershipInstrumentAndReplacementIdPrecedenceGlobally) {
  const auto catalog = standard_catalog();
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{catalog}};
  ASSERT_TRUE(
      engine.execute(limit_order(10U, first_instrument, domain::Side::buy, 100, 5U)).committed());
  ASSERT_TRUE(
      engine.execute(limit_order(20U, second_instrument, domain::Side::buy, 90, 5U, second_client))
          .committed());

  auto wrong_owner = engine.execute(cancel_order(10U, second_instrument, second_client));
  ASSERT_TRUE(wrong_owner.rejected());
  EXPECT_EQ(rejection(wrong_owner).reason, domain::RejectReason::ownership_mismatch);

  auto wrong_instrument = engine.execute(cancel_order(10U, second_instrument, first_client));
  ASSERT_TRUE(wrong_instrument.rejected());
  EXPECT_EQ(rejection(wrong_instrument).reason, domain::RejectReason::instrument_mismatch);

  auto unknown_route = engine.execute(cancel_order(10U, unknown_instrument, first_client));
  ASSERT_TRUE(unknown_route.rejected());
  EXPECT_EQ(rejection(unknown_route).reason, domain::RejectReason::unknown_instrument);

  auto replace_wrong_owner =
      engine.execute(replace_order(10U, 20U, second_instrument, 101, 6U, second_client));
  ASSERT_TRUE(replace_wrong_owner.rejected());
  EXPECT_EQ(rejection(replace_wrong_owner).reason, domain::RejectReason::ownership_mismatch);

  auto replace_wrong_instrument = engine.execute(replace_order(10U, 20U, second_instrument));
  ASSERT_TRUE(replace_wrong_instrument.rejected());
  EXPECT_EQ(rejection(replace_wrong_instrument).reason, domain::RejectReason::instrument_mismatch);

  auto globally_active_replacement = engine.execute(replace_order(10U, 20U, first_instrument));
  ASSERT_TRUE(globally_active_replacement.rejected());
  EXPECT_EQ(rejection(globally_active_replacement).reason,
            domain::RejectReason::invalid_replacement_id);
  EXPECT_EQ(rejection(globally_active_replacement).order_id, domain::OrderId{20U});

  EXPECT_EQ(engine.active_order_count(), 2U);
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{9U});
  const auto snapshot = engine.snapshot();
  EXPECT_EQ(snapshot.instruments[0].bids[0].orders[0].order_id, domain::OrderId{10U});
  EXPECT_EQ(snapshot.instruments[1].bids[0].orders[0].order_id, domain::OrderId{20U});
}

TEST(MultiInstrumentEngineIdentity,
     AllowsGlobalIdReuseAfterCancelAndAfterBothSidesOfAFullFillBecomeTerminal) {
  const auto catalog = standard_catalog();
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{catalog}};

  ASSERT_TRUE(
      engine.execute(limit_order(1U, first_instrument, domain::Side::buy, 100, 5U)).committed());
  ASSERT_TRUE(engine.execute(cancel_order(1U, first_instrument)).committed());
  ASSERT_EQ(engine.active_order_count(), 0U);

  auto reused_on_second =
      engine.execute(limit_order(1U, second_instrument, domain::Side::buy, 100, 5U));
  ASSERT_TRUE(reused_on_second.committed());
  auto full_fill =
      engine.execute(market_order(2U, second_instrument, domain::Side::sell, 5U, second_client));
  ASSERT_TRUE(full_fill.committed());
  ASSERT_EQ(engine.active_order_count(), 0U);

  auto passive_id_reused =
      engine.execute(limit_order(1U, first_instrument, domain::Side::sell, 110, 3U));
  auto aggressor_id_reused = engine.execute(
      limit_order(2U, second_instrument, domain::Side::sell, 120, 4U, second_client));
  EXPECT_TRUE(passive_id_reused.committed());
  EXPECT_TRUE(aggressor_id_reused.committed());
  EXPECT_EQ(engine.active_order_count(), 2U);
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{7U});
}

TEST(MultiInstrumentEngineCapacity,
     AppliesPerInstrumentLimitsWithoutBlockingCapacityOnAnotherInstrument) {
  const auto catalog = standard_catalog(1U, 2U);
  MultiInstrumentEngine engine{
      std::span<const InstrumentConfig>{catalog},
      MultiInstrumentEngineConfig{.max_total_active_orders = 5U},
  };
  ASSERT_TRUE(
      engine.execute(limit_order(1U, first_instrument, domain::Side::buy, 100, 5U)).committed());

  auto first_over_capacity =
      engine.execute(limit_order(2U, first_instrument, domain::Side::buy, 99, 5U));
  ASSERT_TRUE(first_over_capacity.rejected());
  EXPECT_EQ(rejection(first_over_capacity).reason, domain::RejectReason::capacity_exceeded);

  auto second_book = engine.execute(limit_order(3U, second_instrument, domain::Side::buy, 90, 5U));
  EXPECT_TRUE(second_book.committed());
  EXPECT_EQ(engine.active_order_count(), 2U);
  EXPECT_EQ(engine.snapshot(first_instrument)->active_order_count, 1U);
  EXPECT_EQ(engine.snapshot(second_instrument)->active_order_count, 1U);
}

TEST(MultiInstrumentEngineCapacity, UsesPredictedPostCommandStateForGlobalCapacityAndReplacement) {
  const auto catalog = standard_catalog(3U, 3U);
  MultiInstrumentEngine engine{
      std::span<const InstrumentConfig>{catalog},
      MultiInstrumentEngineConfig{.max_total_active_orders = 2U},
  };
  ASSERT_TRUE(
      engine.execute(limit_order(1U, first_instrument, domain::Side::buy, 100, 5U)).committed());
  ASSERT_TRUE(
      engine.execute(limit_order(2U, second_instrument, domain::Side::buy, 90, 5U, second_client))
          .committed());
  ASSERT_EQ(engine.active_order_count(), 2U);

  auto global_over_capacity =
      engine.execute(limit_order(3U, first_instrument, domain::Side::buy, 99, 5U));
  ASSERT_TRUE(global_over_capacity.rejected());
  EXPECT_EQ(rejection(global_over_capacity).reason, domain::RejectReason::capacity_exceeded);

  auto replacement = engine.execute(replace_order(1U, 3U, first_instrument, 101, 5U));
  ASSERT_TRUE(replacement.committed());
  EXPECT_EQ(engine.active_order_count(), 2U);

  auto terminal_at_capacity =
      engine.execute(market_order(4U, first_instrument, domain::Side::sell, 5U, second_client));
  ASSERT_TRUE(terminal_at_capacity.committed());
  EXPECT_EQ(engine.active_order_count(), 1U);
  EXPECT_EQ(engine.top(first_instrument), std::optional<BookTop>{BookTop{}});
  EXPECT_EQ(engine.snapshot(second_instrument)->active_order_count, 1U);
}

TEST(MultiInstrumentEngineCapacity,
     AFullEngineMayFillOnePassiveOrderAndRestItsResidualInTheSameSlot) {
  const auto catalog = standard_catalog(1U, 1U);
  MultiInstrumentEngine engine{
      std::span<const InstrumentConfig>{catalog},
      MultiInstrumentEngineConfig{.max_total_active_orders = 2U},
  };
  ASSERT_TRUE(
      engine.execute(limit_order(1U, first_instrument, domain::Side::sell, 100, 3U)).committed());
  ASSERT_TRUE(
      engine.execute(limit_order(2U, second_instrument, domain::Side::buy, 90, 5U, second_client))
          .committed());
  ASSERT_EQ(engine.active_order_count(), 2U);

  auto substitute =
      engine.execute(limit_order(3U, first_instrument, domain::Side::buy, 100, 5U, second_client));

  ASSERT_TRUE(substitute.committed());
  EXPECT_EQ(engine.active_order_count(), 2U);
  const auto first = engine.snapshot(first_instrument);
  ASSERT_TRUE(first.has_value());
  ASSERT_EQ(first->active_order_count, 1U);
  ASSERT_EQ(first->bids.size(), 1U);
  ASSERT_EQ(first->bids[0].orders.size(), 1U);
  EXPECT_EQ(first->bids[0].orders[0].order_id, domain::OrderId{3U});
  EXPECT_EQ(first->bids[0].orders[0].remaining_quantity, domain::Quantity{2U});
  EXPECT_TRUE(first->asks.empty());
  EXPECT_TRUE(engine.validate_invariants_for_testing());
}

TEST(MultiInstrumentEngineCapacity, RepresentsLateLevelOverflowAsAtomicSequencedCapacityRejection) {
  constexpr auto maximum_quantity = std::numeric_limits<std::uint64_t>::max();
  const std::array catalog{
      instrument_config(first_instrument, 8U, maximum_quantity, 1),
  };
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{catalog}};
  ASSERT_TRUE(
      engine.execute(limit_order(1U, first_instrument, domain::Side::buy, 100, maximum_quantity))
          .committed());
  const auto before_new = engine.snapshot();

  auto new_overflow = engine.execute(limit_order(2U, first_instrument, domain::Side::buy, 100, 1U));

  ASSERT_TRUE(new_overflow.rejected());
  EXPECT_EQ(rejection(new_overflow).reason, domain::RejectReason::capacity_exceeded);
  EXPECT_EQ(rejection(new_overflow).order_id, domain::OrderId{2U});
  auto expected_after_new = before_new;
  expected_after_new.last_sequence = domain::Sequence{2U};
  EXPECT_EQ(engine.snapshot(), expected_after_new);

  ASSERT_TRUE(
      engine.execute(limit_order(3U, first_instrument, domain::Side::buy, 99, 1U)).committed());
  const auto before_replace = engine.snapshot();
  auto replace_overflow = engine.execute(replace_order(3U, 4U, first_instrument, 100, 1U));

  ASSERT_TRUE(replace_overflow.rejected());
  EXPECT_EQ(rejection(replace_overflow).reason, domain::RejectReason::capacity_exceeded);
  EXPECT_EQ(rejection(replace_overflow).order_id, domain::OrderId{4U});
  auto expected_after_replace = before_replace;
  expected_after_replace.last_sequence = domain::Sequence{4U};
  EXPECT_EQ(engine.snapshot(), expected_after_replace);
  EXPECT_TRUE(engine.validate_invariants_for_testing());
}

TEST(MultiInstrumentEngineRouting, NeverMatchesOrdersAcrossInstrumentBoundaries) {
  const auto catalog = standard_catalog();
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{catalog}};

  ASSERT_TRUE(
      engine.execute(limit_order(1U, first_instrument, domain::Side::sell, 100, 5U)).committed());
  auto crossing_elsewhere =
      engine.execute(limit_order(2U, second_instrument, domain::Side::buy, 110, 7U, second_client));
  ASSERT_TRUE(crossing_elsewhere.committed());
  ASSERT_NE(crossing_elsewhere.batch(), nullptr);
  EXPECT_EQ(crossing_elsewhere.batch()->size(), 3U);
  EXPECT_EQ(domain::event_type((*crossing_elsewhere.batch())[1]), domain::EventType::rested);
  EXPECT_EQ(engine.active_order_count(), 2U);

  auto local_cross =
      engine.execute(limit_order(3U, first_instrument, domain::Side::buy, 100, 5U, second_client));
  ASSERT_TRUE(local_cross.committed());
  ASSERT_NE(local_cross.batch(), nullptr);
  ASSERT_EQ(local_cross.batch()->size(), 4U);
  EXPECT_EQ(domain::event_type((*local_cross.batch())[1]), domain::EventType::trade);
  EXPECT_EQ(domain::event_header((*local_cross.batch())[1]).instrument_id, first_instrument);

  EXPECT_EQ(engine.top(first_instrument), std::optional<BookTop>{BookTop{}});
  const auto second_top = engine.top(second_instrument);
  ASSERT_TRUE(second_top.has_value());
  EXPECT_EQ(second_top->best_bid, (domain::TopOfBookLevel{
                                      .price = domain::PriceTicks{110},
                                      .aggregate_quantity = domain::Quantity{7U},
                                  }));
  EXPECT_FALSE(second_top->best_ask.has_value());
  EXPECT_EQ(engine.active_order_count(), 1U);
}

TEST(MultiInstrumentEngineSnapshot,
     SortsInstrumentsAndPreservesGlobalPriorityInsideCanonicalBooks) {
  const std::array reverse_catalog{
      instrument_config(second_instrument),
      instrument_config(first_instrument),
  };
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{reverse_catalog}};

  ASSERT_TRUE(
      engine
          .execute(limit_order(90U, second_instrument, domain::Side::sell, 110, 9U, second_client))
          .committed());
  ASSERT_TRUE(
      engine.execute(limit_order(70U, first_instrument, domain::Side::buy, 100, 5U)).committed());
  ASSERT_TRUE(
      engine.execute(limit_order(71U, first_instrument, domain::Side::buy, 101, 7U)).committed());

  const auto snapshot = engine.snapshot();
  EXPECT_EQ(snapshot.last_sequence, domain::Sequence{3U});
  EXPECT_EQ(snapshot.active_order_count, 3U);
  ASSERT_EQ(snapshot.catalog.size(), 2U);
  EXPECT_EQ(snapshot.catalog[0].instrument_id, first_instrument);
  EXPECT_EQ(snapshot.catalog[1].instrument_id, second_instrument);
  ASSERT_EQ(snapshot.instruments.size(), 2U);
  EXPECT_EQ(snapshot.instruments[0].instrument_id, first_instrument);
  EXPECT_EQ(snapshot.instruments[1].instrument_id, second_instrument);
  EXPECT_EQ(snapshot.instruments[0].active_order_count, 2U);
  ASSERT_EQ(snapshot.instruments[0].bids.size(), 2U);
  EXPECT_EQ(snapshot.instruments[0].bids[0].price, domain::PriceTicks{101});
  EXPECT_EQ(snapshot.instruments[0].bids[0].orders[0].order_id, domain::OrderId{71U});
  EXPECT_EQ(snapshot.instruments[0].bids[0].orders[0].priority_sequence, domain::Sequence{3U});
  EXPECT_EQ(snapshot.instruments[0].bids[1].price, domain::PriceTicks{100});
  EXPECT_EQ(snapshot.instruments[0].bids[1].orders[0].priority_sequence, domain::Sequence{2U});
  EXPECT_EQ(snapshot.instruments[1].active_order_count, 1U);
  EXPECT_EQ(snapshot.instruments[1].asks[0].orders[0].priority_sequence, domain::Sequence{1U});

  EXPECT_EQ(engine.snapshot(first_instrument), snapshot.instruments[0]);
  EXPECT_EQ(engine.snapshot(second_instrument), snapshot.instruments[1]);
}

TEST(MultiInstrumentEngineInvariants, DetectsGlobalDirectoryPerBookCapacityAndSequencerCorruption) {
  const auto instruments = standard_catalog();
  {
    MultiInstrumentEngine engine{std::span<const InstrumentConfig>{instruments}};
    ASSERT_TRUE(
        engine.execute(limit_order(1U, first_instrument, domain::Side::buy, 100, 5U)).committed());
    ASSERT_TRUE(engine.validate_invariants_for_testing());

    engine.erase_active_identity_for_testing(domain::OrderId{1U});

    EXPECT_FALSE(engine.validate_invariants_for_testing());
  }
  {
    MultiInstrumentEngine engine{std::span<const InstrumentConfig>{instruments}};
    ASSERT_TRUE(
        engine.execute(limit_order(1U, first_instrument, domain::Side::buy, 100, 5U)).committed());
    ASSERT_TRUE(
        engine.execute(limit_order(2U, first_instrument, domain::Side::buy, 99, 5U)).committed());
    ASSERT_TRUE(engine.validate_invariants_for_testing());

    engine.set_instrument_max_active_orders_for_testing(first_instrument, 1U);

    EXPECT_FALSE(engine.validate_invariants_for_testing());
  }
  {
    MultiInstrumentEngine engine{std::span<const InstrumentConfig>{instruments}};
    engine.set_sequence_state_for_testing(domain::Sequence{}, false);
    EXPECT_FALSE(engine.validate_invariants_for_testing());
  }
  {
    MultiInstrumentEngine engine{std::span<const InstrumentConfig>{instruments}};
    engine.set_sequence_state_for_testing(domain::Sequence{1U}, true);
    EXPECT_FALSE(engine.validate_invariants_for_testing());
  }
}

TEST(MultiInstrumentEngineCompatibility,
     AOneInstrumentCatalogPreservesExistingEventsAndBookSnapshotShape) {
  const std::array catalog{instrument_config(first_instrument)};
  const auto config = catalog[0].matching;
  MatchingEngine single{first_instrument, config};
  MultiInstrumentEngine multi{std::span<const InstrumentConfig>{catalog}};
  const std::array<domain::Command, 4U> commands{
      limit_order(1U, first_instrument, domain::Side::buy, 100, 5U),
      limit_order(2U, first_instrument, domain::Side::sell, 110, 7U, second_client),
      cancel_order(999U, first_instrument),
      replace_order(1U, 3U, first_instrument, 101, 9U),
  };

  for (const auto& command : commands) {
    auto single_result = single.execute(command);
    auto multi_result = multi.execute(command);
    ASSERT_EQ(single_result.error(), multi_result.error());
    ASSERT_EQ(single_result.has_value(), multi_result.has_value());
    ASSERT_NE(single_result.batch(), nullptr);
    ASSERT_NE(multi_result.batch(), nullptr);
    EXPECT_EQ(single_result.rejected(), multi_result.rejected());
    EXPECT_EQ(single_result.committed(), multi_result.committed());
    ASSERT_EQ(single_result.batch()->size(), multi_result.batch()->size());
    for (std::size_t event_index = 0U; event_index < single_result.batch()->size(); ++event_index) {
      EXPECT_EQ((*single_result.batch())[event_index], (*multi_result.batch())[event_index])
          << "event_index=" << event_index;
    }
  }

  const auto single_snapshot = single.snapshot();
  const auto multi_snapshot = multi.snapshot();
  ASSERT_EQ(multi_snapshot.instruments.size(), 1U);
  const auto& instrument = multi_snapshot.instruments[0];
  EXPECT_EQ(instrument.instrument_id, single_snapshot.instrument_id);
  EXPECT_EQ(instrument.active_order_count, single_snapshot.active_order_count);
  EXPECT_EQ(instrument.bids, single_snapshot.bids);
  EXPECT_EQ(instrument.asks, single_snapshot.asks);
  EXPECT_EQ(multi_snapshot.last_sequence, single_snapshot.last_sequence);
  EXPECT_EQ(multi_snapshot.sequence_exhausted, single_snapshot.sequence_exhausted);

  // ATLSST01 is frozen evidence and must remain unchanged by the new facade.
  const BookSnapshot empty_single{
      .semantics_version = atlaslob_semantics_version,
      .instrument_id = first_instrument,
      .last_sequence = {},
      .sequence_exhausted = false,
      .active_order_count = 0U,
      .bids = {},
      .asks = {},
  };
  EXPECT_EQ(state_digest(empty_single).hex(),
            "19a8ffaeb1bee1b8aa87123c3508af1b"
            "fa87e3d634a09ba491e1b85fe597b219");
}

TEST(MultiInstrumentEngineDigest, IsCanonicalAcrossCatalogInputOrderAndCoversGlobalPriority) {
  const std::array forward_catalog{
      instrument_config(first_instrument),
      instrument_config(second_instrument),
  };
  const std::array reverse_catalog{
      instrument_config(second_instrument),
      instrument_config(first_instrument),
  };
  MultiInstrumentEngine first{std::span<const InstrumentConfig>{forward_catalog}};
  MultiInstrumentEngine second{std::span<const InstrumentConfig>{reverse_catalog}};

  const std::array<domain::Command, 3U> commands{
      limit_order(1U, first_instrument, domain::Side::buy, 100, 5U),
      limit_order(2U, second_instrument, domain::Side::sell, 110, 7U, second_client),
      limit_order(3U, first_instrument, domain::Side::buy, 99, 9U),
  };
  for (const auto& command : commands) {
    ASSERT_TRUE(first.execute(command).committed());
    ASSERT_TRUE(second.execute(command).committed());
  }

  EXPECT_EQ(first.snapshot(), second.snapshot());
  EXPECT_EQ(first.state_digest(), second.state_digest());
  EXPECT_EQ(first.state_digest(), state_digest(first.snapshot()));
  EXPECT_EQ(first.state_digest().hex().size(), 64U);

  MultiInstrumentEngine different_interleaving{std::span<const InstrumentConfig>{forward_catalog}};
  ASSERT_TRUE(
      different_interleaving
          .execute(limit_order(2U, second_instrument, domain::Side::sell, 110, 7U, second_client))
          .committed());
  ASSERT_TRUE(
      different_interleaving.execute(limit_order(1U, first_instrument, domain::Side::buy, 100, 5U))
          .committed());
  ASSERT_TRUE(
      different_interleaving.execute(limit_order(3U, first_instrument, domain::Side::buy, 99, 9U))
          .committed());

  // The economic books match, but global priority is authoritative evidence.
  EXPECT_EQ(different_interleaving.snapshot(first_instrument)->bids[0].price,
            first.snapshot(first_instrument)->bids[0].price);
  EXPECT_EQ(different_interleaving.snapshot(second_instrument)->asks[0].price,
            first.snapshot(second_instrument)->asks[0].price);
  EXPECT_NE(different_interleaving.snapshot(), first.snapshot());
  EXPECT_NE(different_interleaving.state_digest(), first.state_digest());
}

TEST(MultiInstrumentEngineDigest, MatchesTheIndependentAtlsme01GoldenAndCoversEveryTopLevelField) {
  const auto original = representative_engine_snapshot();
  const auto digest = state_digest(original);

  // Independently generated from the ADR 0012 ATLSME01 byte layout with
  // Node big-endian Buffer writes and SHA-256.
  EXPECT_EQ(digest.hex(),
            "e0799da2e8fb3148fea7c985e5bf0d3c"
            "49238a39b8344608d5c85d00c82bcfe3");

  const auto expect_changed = [&digest](EngineSnapshot changed) {
    EXPECT_NE(state_digest(changed), digest);
  };

  auto changed = original;
  ++changed.semantics_version;
  expect_changed(changed);
  changed = original;
  ++changed.engine_config.max_total_active_orders;
  expect_changed(changed);
  changed = original;
  changed.catalog[0].instrument_id = domain::InstrumentId{8U};
  expect_changed(changed);
  changed = original;
  changed.catalog[0].matching.max_order_quantity = domain::Quantity{101U};
  expect_changed(changed);
  changed = original;
  changed.catalog[0].matching.tick_increment = domain::PriceTicks{6};
  expect_changed(changed);
  changed = original;
  ++changed.catalog[0].matching.max_active_orders;
  expect_changed(changed);
  changed = original;
  changed.last_sequence = domain::Sequence{4U};
  expect_changed(changed);
  changed = original;
  changed.sequence_exhausted = true;
  expect_changed(changed);
  changed = original;
  ++changed.active_order_count;
  expect_changed(changed);
  changed = original;
  changed.instruments[0].instrument_id = domain::InstrumentId{8U};
  expect_changed(changed);
  changed = original;
  ++changed.instruments[0].active_order_count;
  expect_changed(changed);
  changed = original;
  changed.instruments[0].bids[0].orders[0].priority_sequence = domain::Sequence{2U};
  expect_changed(changed);
  changed = original;
  changed.instruments.push_back(InstrumentSnapshot{
      .instrument_id = domain::InstrumentId{10U},
      .active_order_count = 0U,
      .bids = {},
      .asks = {},
  });
  expect_changed(changed);
}

TEST(MultiInstrumentEngineDigest, CanonicalizesHostUnboundedCapacitySentinelsToUnsigned64Maximum) {
  const std::array catalog{
      instrument_config(first_instrument, std::numeric_limits<std::size_t>::max())};
  const MultiInstrumentEngine engine{std::span<const InstrumentConfig>{catalog}};

  // Independently encoded with both unbounded capacity fields represented as
  // UINT64_MAX. This stays identical on 32-bit and 64-bit hosts even though
  // the in-memory sentinel is SIZE_MAX.
  EXPECT_EQ(engine.state_digest().hex(),
            "3b56e36fd1ec9647a69f4312de138587"
            "3439cd468eec9d966a75a79ae2b5952a");
}

}  // namespace
