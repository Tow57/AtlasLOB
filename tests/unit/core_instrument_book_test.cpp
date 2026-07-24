#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "core_test_access.hpp"
#include "instrument_book.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::core;

constexpr domain::InstrumentId instrument_id{11U};

static_assert(!std::is_copy_constructible_v<InstrumentBook>);
static_assert(!std::is_copy_assignable_v<InstrumentBook>);
static_assert(!std::is_move_constructible_v<InstrumentBook>);
static_assert(!std::is_move_assignable_v<InstrumentBook>);
static_assert(std::is_same_v<decltype(std::declval<InstrumentBook&>().best_bid()), PriceLevel*>);
static_assert(
    std::is_same_v<decltype(std::declval<const InstrumentBook&>().best_bid()), const PriceLevel*>);

OrderNodeSpec order_spec(std::uint64_t order_id, domain::Side side, std::int64_t price,
                         std::uint64_t quantity, std::uint64_t priority,
                         domain::ClientId client_id = domain::ClientId{7U},
                         domain::InstrumentId instrument = instrument_id) {
  return {
      .order_id = domain::OrderId{order_id},
      .client_id = client_id,
      .instrument_id = instrument,
      .side = side,
      .price = domain::PriceTicks{price},
      .remaining_quantity = domain::Quantity{quantity},
      .priority_sequence = domain::Sequence{priority},
  };
}

std::vector<std::int64_t> prices(const BidBookSide& side) {
  std::vector<std::int64_t> result;
  result.reserve(side.level_count());
  for (const PriceLevel& level : side) {
    result.push_back(level.price().value());
  }
  return result;
}

std::vector<std::int64_t> prices(const AskBookSide& side) {
  std::vector<std::int64_t> result;
  result.reserve(side.level_count());
  for (const PriceLevel& level : side) {
    result.push_back(level.price().value());
  }
  return result;
}

void expect_empty_valid_book(const InstrumentBook& book) {
  EXPECT_TRUE(book.empty());
  EXPECT_EQ(book.active_order_count(), 0U);
  EXPECT_EQ(book.best_bid(), nullptr);
  EXPECT_EQ(book.best_ask(), nullptr);
  EXPECT_TRUE(book.bids().empty());
  EXPECT_TRUE(book.asks().empty());
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBook, RequiresANonzeroInstrumentAndStartsEmpty) {
  EXPECT_THROW(static_cast<void>(InstrumentBook{domain::InstrumentId{}}), std::invalid_argument);

  const InstrumentBook book{instrument_id};
  EXPECT_EQ(book.instrument_id(), instrument_id);
  EXPECT_EQ(book.find(domain::OrderId{1U}), nullptr);
  expect_empty_valid_book(book);
}

TEST(InstrumentBook, RestsBothSidesAndExposesBestPriceFirstViews) {
  InstrumentBook book{instrument_id};
  const auto bid_100 = book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U));
  const auto bid_101 = book.rest(order_spec(2U, domain::Side::buy, 10'100, 7U, 2U));
  const auto ask_103 = book.rest(order_spec(3U, domain::Side::sell, 10'300, 11U, 3U));
  const auto ask_102 = book.rest(order_spec(4U, domain::Side::sell, 10'200, 13U, 4U));

  ASSERT_TRUE(bid_100);
  ASSERT_TRUE(bid_101);
  ASSERT_TRUE(ask_103);
  ASSERT_TRUE(ask_102);
  EXPECT_EQ(book.active_order_count(), 4U);
  EXPECT_FALSE(book.empty());
  ASSERT_NE(book.best_bid(), nullptr);
  ASSERT_NE(book.best_ask(), nullptr);
  EXPECT_EQ(book.best_bid()->price(), domain::PriceTicks{10'100});
  EXPECT_EQ(book.best_bid()->aggregate_quantity(), domain::Quantity{7U});
  EXPECT_EQ(book.best_ask()->price(), domain::PriceTicks{10'200});
  EXPECT_EQ(book.best_ask()->aggregate_quantity(), domain::Quantity{13U});
  EXPECT_EQ(prices(book.bids()), (std::vector<std::int64_t>{10'100, 10'000}));
  EXPECT_EQ(prices(book.asks()), (std::vector<std::int64_t>{10'200, 10'300}));
  EXPECT_EQ(book.find(domain::OrderId{1U}), bid_100.node);
  EXPECT_EQ(std::as_const(book).find(domain::OrderId{4U}), ask_102.node);

  const auto invariant = book.validate_invariants();
  EXPECT_TRUE(invariant);
  EXPECT_EQ(invariant.observed_order_count, 4U);
}

TEST(InstrumentBookRest, RejectsInvalidSpecificationsWithoutMutation) {
  InstrumentBook book{instrument_id};
  const auto original = book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 10U));
  ASSERT_TRUE(original);
  auto* const original_level = book.best_bid();

  auto wrong_instrument = order_spec(2U, domain::Side::sell, 10'100, 7U, 11U);
  wrong_instrument.instrument_id = domain::InstrumentId{12U};
  const auto instrument_result = book.rest(wrong_instrument);
  EXPECT_FALSE(instrument_result);
  EXPECT_EQ(instrument_result.status.error, InstrumentBookError::instrument_mismatch);

  auto invalid_side = order_spec(3U, domain::Side::buy, 9'900, 7U, 12U);
  invalid_side.side = static_cast<domain::Side>(0U);
  const auto side_result = book.rest(invalid_side);
  EXPECT_FALSE(side_result);
  EXPECT_EQ(side_result.status.error, InstrumentBookError::invalid_side);

  auto invalid_client = order_spec(4U, domain::Side::buy, 9'900, 7U, 13U);
  invalid_client.client_id = {};
  const auto client_result = book.rest(invalid_client);
  EXPECT_FALSE(client_result);
  EXPECT_EQ(client_result.status.error, InstrumentBookError::storage_failure);
  EXPECT_EQ(client_result.status.storage_error, StorageError::invalid_client_id);

  auto invalid_price = order_spec(5U, domain::Side::buy, 0, 7U, 14U);
  const auto price_result = book.rest(invalid_price);
  EXPECT_FALSE(price_result);
  EXPECT_EQ(price_result.status.error, InstrumentBookError::storage_failure);
  EXPECT_EQ(price_result.status.storage_error, StorageError::invalid_price);

  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_EQ(book.best_bid(), original_level);
  EXPECT_EQ(book.best_ask(), nullptr);
  EXPECT_EQ(original_level->aggregate_quantity(), domain::Quantity{5U});
  EXPECT_EQ(original_level->order_count(), 1U);
  EXPECT_EQ(book.find(domain::OrderId{1U}), original.node);
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{3U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{4U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{5U}), nullptr);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookRest, RejectsDuplicateAndNonmonotonicPriorityAtomically) {
  InstrumentBook book{instrument_id};
  const auto original = book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 10U));
  ASSERT_TRUE(original);
  auto* const level = book.best_bid();
  ASSERT_NE(level, nullptr);

  const auto duplicate = book.rest(order_spec(1U, domain::Side::sell, 10'100, 99U, 11U));
  EXPECT_FALSE(duplicate);
  EXPECT_EQ(duplicate.status.error, InstrumentBookError::duplicate_order_id);
  EXPECT_EQ(duplicate.status.storage_error, StorageError::duplicate_order_id);

  const auto nonmonotonic = book.rest(order_spec(2U, domain::Side::buy, 10'000, 7U, 9U));
  EXPECT_FALSE(nonmonotonic);
  EXPECT_EQ(nonmonotonic.status.error, InstrumentBookError::price_level_failure);
  EXPECT_EQ(nonmonotonic.status.level_error, PriceLevelError::nonmonotonic_priority);

  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_EQ(book.best_bid(), level);
  EXPECT_EQ(level->head(), original.node);
  EXPECT_EQ(level->tail(), original.node);
  EXPECT_EQ(level->order_count(), 1U);
  EXPECT_EQ(level->aggregate_quantity(), domain::Quantity{5U});
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookRest, RollsBackAnAggregateOverflowWithoutPublishingTheOrder) {
  InstrumentBook book{instrument_id};
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto original = book.rest(order_spec(1U, domain::Side::buy, 10'000, maximum, 1U));
  ASSERT_TRUE(original);
  auto* const level = book.best_bid();
  ASSERT_NE(level, nullptr);

  const auto overflow = book.rest(order_spec(2U, domain::Side::buy, 10'000, 1U, 2U));
  EXPECT_FALSE(overflow);
  EXPECT_EQ(overflow.status.error, InstrumentBookError::price_level_failure);
  EXPECT_EQ(overflow.status.level_error, PriceLevelError::aggregate_overflow);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_EQ(book.find(domain::OrderId{1U}), original.node);
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_EQ(book.best_bid(), level);
  EXPECT_EQ(level->head(), original.node);
  EXPECT_EQ(level->tail(), original.node);
  EXPECT_EQ(level->aggregate_quantity(), domain::Quantity{maximum});
  EXPECT_EQ(level->order_count(), 1U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookRest, RejectsCrossingBuysAndSellsBeforeAnyMutation) {
  InstrumentBook book{instrument_id};
  const auto bid = book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U));
  const auto ask = book.rest(order_spec(2U, domain::Side::sell, 10'200, 7U, 2U));
  ASSERT_TRUE(bid);
  ASSERT_TRUE(ask);
  auto* const bid_level = book.best_bid();
  auto* const ask_level = book.best_ask();
  ASSERT_NE(bid_level, nullptr);
  ASSERT_NE(ask_level, nullptr);

  const auto crossing_buy = book.rest(order_spec(3U, domain::Side::buy, 10'200, 11U, 3U));
  EXPECT_FALSE(crossing_buy);
  EXPECT_EQ(crossing_buy.status.error, InstrumentBookError::would_cross_book);

  const auto strictly_crossing_buy = book.rest(order_spec(4U, domain::Side::buy, 10'300, 11U, 4U));
  EXPECT_FALSE(strictly_crossing_buy);
  EXPECT_EQ(strictly_crossing_buy.status.error, InstrumentBookError::would_cross_book);

  const auto crossing_sell = book.rest(order_spec(5U, domain::Side::sell, 10'000, 13U, 5U));
  EXPECT_FALSE(crossing_sell);
  EXPECT_EQ(crossing_sell.status.error, InstrumentBookError::would_cross_book);

  const auto strictly_crossing_sell = book.rest(order_spec(6U, domain::Side::sell, 9'999, 13U, 6U));
  EXPECT_FALSE(strictly_crossing_sell);
  EXPECT_EQ(strictly_crossing_sell.status.error, InstrumentBookError::would_cross_book);

  EXPECT_EQ(book.active_order_count(), 2U);
  EXPECT_EQ(book.bids().level_count(), 1U);
  EXPECT_EQ(book.asks().level_count(), 1U);
  EXPECT_EQ(book.best_bid(), bid_level);
  EXPECT_EQ(book.best_ask(), ask_level);
  EXPECT_EQ(bid_level->head(), bid.node);
  EXPECT_EQ(bid_level->tail(), bid.node);
  EXPECT_EQ(bid_level->aggregate_quantity(), domain::Quantity{5U});
  EXPECT_EQ(bid_level->order_count(), 1U);
  EXPECT_EQ(ask_level->head(), ask.node);
  EXPECT_EQ(ask_level->tail(), ask.node);
  EXPECT_EQ(ask_level->aggregate_quantity(), domain::Quantity{7U});
  EXPECT_EQ(ask_level->order_count(), 1U);
  EXPECT_EQ(book.find(domain::OrderId{3U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{4U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{5U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{6U}), nullptr);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookCancel, ImplementsTheCanonicalMiddleReduceHeadAndSingletonSequence) {
  InstrumentBook book{instrument_id};
  const auto first = book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U));
  const auto second = book.rest(order_spec(2U, domain::Side::buy, 10'000, 7U, 2U));
  const auto third = book.rest(order_spec(3U, domain::Side::buy, 10'000, 11U, 3U));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(third);
  auto* const level = book.best_bid();
  ASSERT_NE(level, nullptr);
  EXPECT_EQ(level->aggregate_quantity(), domain::Quantity{23U});
  EXPECT_EQ(level->order_count(), 3U);

  const auto middle = book.cancel(domain::OrderId{2U});
  ASSERT_TRUE(middle);
  EXPECT_EQ(middle.order, (RemovedOrder{
                              .order_id = domain::OrderId{2U},
                              .client_id = domain::ClientId{7U},
                              .instrument_id = instrument_id,
                              .side = domain::Side::buy,
                              .price = domain::PriceTicks{10'000},
                              .remaining_quantity = domain::Quantity{7U},
                              .priority_sequence = domain::Sequence{2U},
                          }));
  EXPECT_EQ(level->head(), first.node);
  EXPECT_EQ(level->tail(), third.node);
  EXPECT_EQ(first.node->next(), third.node);
  EXPECT_EQ(third.node->previous(), first.node);
  EXPECT_EQ(level->aggregate_quantity(), domain::Quantity{16U});
  EXPECT_EQ(level->order_count(), 2U);

  const auto reduced = book.reduce_remaining(*first.node, domain::Quantity{2U});
  EXPECT_TRUE(reduced);
  EXPECT_EQ(first.node->remaining_quantity(), domain::Quantity{3U});
  EXPECT_EQ(first.node->priority_sequence(), domain::Sequence{1U});
  EXPECT_EQ(level->aggregate_quantity(), domain::Quantity{14U});

  const auto head = book.cancel(domain::OrderId{1U});
  ASSERT_TRUE(head);
  EXPECT_EQ(head.order.remaining_quantity, domain::Quantity{3U});
  EXPECT_EQ(level->head(), third.node);
  EXPECT_EQ(level->tail(), third.node);
  EXPECT_EQ(level->aggregate_quantity(), domain::Quantity{11U});
  EXPECT_EQ(level->order_count(), 1U);

  const auto singleton = book.cancel(domain::OrderId{3U});
  ASSERT_TRUE(singleton);
  EXPECT_EQ(singleton.order.remaining_quantity, domain::Quantity{11U});
  expect_empty_valid_book(book);
}

TEST(InstrumentBookCancel, RemovesTailAndExactNonBestSingletonLevel) {
  InstrumentBook book{instrument_id};
  const auto first = book.rest(order_spec(1U, domain::Side::sell, 10'100, 5U, 1U));
  const auto second = book.rest(order_spec(2U, domain::Side::sell, 10'100, 7U, 2U));
  const auto tail = book.rest(order_spec(3U, domain::Side::sell, 10'100, 11U, 3U));
  const auto nonbest = book.rest(order_spec(4U, domain::Side::sell, 10'200, 13U, 4U));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(tail);
  ASSERT_TRUE(nonbest);
  auto* const best = book.best_ask();
  ASSERT_NE(best, nullptr);
  ASSERT_NE(book.asks().find_level(domain::PriceTicks{10'200}), nullptr);

  const auto removed_tail = book.cancel(domain::OrderId{3U});
  ASSERT_TRUE(removed_tail);
  EXPECT_EQ(best->tail(), second.node);
  EXPECT_EQ(second.node->next(), nullptr);
  EXPECT_EQ(best->aggregate_quantity(), domain::Quantity{12U});
  EXPECT_EQ(best->order_count(), 2U);

  const auto removed_nonbest = book.cancel(domain::OrderId{4U});
  ASSERT_TRUE(removed_nonbest);
  EXPECT_EQ(book.asks().find_level(domain::PriceTicks{10'200}), nullptr);
  EXPECT_EQ(book.asks().level_count(), 1U);
  EXPECT_EQ(book.best_ask(), best);
  EXPECT_EQ(book.active_order_count(), 2U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookCancel, UnknownIdAndForeignNodeFailuresAreAtomic) {
  InstrumentBook book{instrument_id};
  const auto rested = book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U));
  ASSERT_TRUE(rested);
  HeapOrderStorage foreign_storage;
  const auto foreign = foreign_storage.create(order_spec(2U, domain::Side::buy, 9'900, 7U, 2U));
  ASSERT_TRUE(foreign);

  const auto unknown = book.cancel(domain::OrderId{999U});
  EXPECT_FALSE(unknown);
  EXPECT_EQ(unknown.status.error, InstrumentBookError::unknown_order_id);

  const auto foreign_remove = book.remove(*foreign.node);
  EXPECT_FALSE(foreign_remove);
  EXPECT_EQ(foreign_remove.status.error, InstrumentBookError::node_not_owned);

  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_EQ(book.find(domain::OrderId{1U}), rested.node);
  EXPECT_EQ(book.best_bid()->aggregate_quantity(), domain::Quantity{5U});
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookCancel, ReportsCorruptBookBeforeUnknownOrderLookup) {
  InstrumentBook book{instrument_id};
  const auto rested = book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U));
  ASSERT_TRUE(rested);
  auto& index = core::test::CoreAccess::active_index(book);
  ASSERT_TRUE(core::test::CoreAccess::insert_index_entry(index, domain::OrderId{999U}, nullptr));

  const auto canceled = book.cancel(domain::OrderId{777U});
  EXPECT_FALSE(canceled);
  EXPECT_EQ(canceled.status.error, InstrumentBookError::book_invariant_violation);
  EXPECT_EQ(book.active_order_count(), 2U);
  EXPECT_EQ(book.find(domain::OrderId{1U}), rested.node);
  EXPECT_EQ(book.best_bid()->aggregate_quantity(), domain::Quantity{5U});

  ASSERT_TRUE(core::test::CoreAccess::erase_index_entry(index, domain::OrderId{999U}));
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookReduce, RejectsZeroAndFullReductionWithoutMutation) {
  InstrumentBook book{instrument_id};
  const auto rested = book.rest(order_spec(1U, domain::Side::sell, 10'100, 7U, 1U));
  ASSERT_TRUE(rested);
  auto* const level = book.best_ask();
  ASSERT_NE(level, nullptr);

  const auto zero = book.reduce_remaining(*rested.node, domain::Quantity{});
  EXPECT_EQ(zero.error, InstrumentBookError::price_level_failure);
  EXPECT_EQ(zero.level_error, PriceLevelError::invalid_reduction);

  const auto full = book.reduce_remaining(*rested.node, domain::Quantity{7U});
  EXPECT_EQ(full.error, InstrumentBookError::price_level_failure);
  EXPECT_EQ(full.level_error, PriceLevelError::invalid_reduction);

  EXPECT_EQ(rested.node->remaining_quantity(), domain::Quantity{7U});
  EXPECT_EQ(level->aggregate_quantity(), domain::Quantity{7U});
  EXPECT_EQ(level->order_count(), 1U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBook, AllowsAnOrderIdToBeReusedAfterTerminalRemoval) {
  InstrumentBook book{instrument_id};
  const auto original = book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U));
  ASSERT_TRUE(original);
  ASSERT_TRUE(book.cancel(domain::OrderId{1U}));

  const auto reused = book.rest(order_spec(1U, domain::Side::sell, 10'100, 9U, 2U));
  ASSERT_TRUE(reused);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_EQ(book.find(domain::OrderId{1U}), reused.node);
  EXPECT_EQ(reused.node->side(), domain::Side::sell);
  EXPECT_EQ(reused.node->remaining_quantity(), domain::Quantity{9U});
  EXPECT_EQ(book.best_bid(), nullptr);
  EXPECT_EQ(book.best_ask(), reused.node->price_level());
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBook, SafelyDrainsLiveOrdersAtScopeExit) {
  EXPECT_NO_FATAL_FAILURE({
    InstrumentBook book{instrument_id};
    EXPECT_TRUE(book.rest(order_spec(1U, domain::Side::buy, 9'900, 5U, 1U)));
    EXPECT_TRUE(book.rest(order_spec(2U, domain::Side::buy, 9'800, 7U, 2U)));
    EXPECT_TRUE(book.rest(order_spec(3U, domain::Side::sell, 10'100, 11U, 3U)));
    EXPECT_TRUE(book.rest(order_spec(4U, domain::Side::sell, 10'200, 13U, 4U)));
    EXPECT_EQ(book.active_order_count(), 4U);
    EXPECT_TRUE(book.validate_invariants());
  });
}

TEST(InstrumentBookInvariants, DetectsInvalidBookIdentityAndSideFailures) {
  InstrumentBook book{instrument_id};
  const auto bid = book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U));
  const auto ask = book.rest(order_spec(2U, domain::Side::sell, 10'100, 7U, 2U));
  ASSERT_TRUE(bid);
  ASSERT_TRUE(ask);

  core::test::CoreAccess::set_book_instrument_id(book, {});
  EXPECT_EQ(book.validate_invariants().error, InstrumentBookInvariantError::invalid_instrument_id);
  core::test::CoreAccess::set_book_instrument_id(book, instrument_id);

  auto* const bid_level = core::test::CoreAccess::mutable_price_level(*bid.node);
  auto* const ask_level = core::test::CoreAccess::mutable_price_level(*ask.node);
  ASSERT_NE(bid_level, nullptr);
  ASSERT_NE(ask_level, nullptr);

  core::test::CoreAccess::set_level_aggregate(*bid_level, domain::Quantity{6U});
  const auto bid_failure = book.validate_invariants();
  EXPECT_EQ(bid_failure.error, InstrumentBookInvariantError::bid_side_invariant);
  EXPECT_EQ(bid_failure.side_result.error, BookSideInvariantError::level_invariant_violation);
  EXPECT_EQ(bid_failure.side_result.level_result.error,
            PriceLevelInvariantError::aggregate_mismatch);
  core::test::CoreAccess::set_level_aggregate(*bid_level, domain::Quantity{5U});

  core::test::CoreAccess::set_level_aggregate(*ask_level, domain::Quantity{8U});
  const auto ask_failure = book.validate_invariants();
  EXPECT_EQ(ask_failure.error, InstrumentBookInvariantError::ask_side_invariant);
  EXPECT_EQ(ask_failure.side_result.level_result.error,
            PriceLevelInvariantError::aggregate_mismatch);
  core::test::CoreAccess::set_level_aggregate(*ask_level, domain::Quantity{7U});
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookInvariants, DetectsNodeSideInstrumentPriceAndBacklinkCorruption) {
  InstrumentBook book{instrument_id};
  const auto rested = book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U));
  ASSERT_TRUE(rested);
  auto* const level = core::test::CoreAccess::mutable_price_level(*rested.node);
  ASSERT_NE(level, nullptr);

  core::test::CoreAccess::set_side(*rested.node, domain::Side::sell);
  EXPECT_EQ(book.validate_invariants().error, InstrumentBookInvariantError::node_side_mismatch);
  core::test::CoreAccess::set_side(*rested.node, domain::Side::buy);

  core::test::CoreAccess::set_instrument_id(*rested.node, domain::InstrumentId{12U});
  EXPECT_EQ(book.validate_invariants().error,
            InstrumentBookInvariantError::node_instrument_mismatch);
  core::test::CoreAccess::set_instrument_id(*rested.node, instrument_id);

  core::test::CoreAccess::set_client_id(*rested.node, {});
  EXPECT_EQ(book.validate_invariants().error, InstrumentBookInvariantError::node_invalid_client_id);
  core::test::CoreAccess::set_client_id(*rested.node, domain::ClientId{7U});

  core::test::CoreAccess::set_price(*rested.node, domain::PriceTicks{9'999});
  const auto wrong_price = book.validate_invariants();
  EXPECT_EQ(wrong_price.error, InstrumentBookInvariantError::bid_side_invariant);
  EXPECT_EQ(wrong_price.side_result.level_result.error, PriceLevelInvariantError::wrong_price);
  core::test::CoreAccess::set_price(*rested.node, domain::PriceTicks{10'000});

  core::test::CoreAccess::set_price_level(*rested.node, nullptr);
  const auto wrong_backlink = book.validate_invariants();
  EXPECT_EQ(wrong_backlink.error, InstrumentBookInvariantError::bid_side_invariant);
  EXPECT_EQ(wrong_backlink.side_result.level_result.error,
            PriceLevelInvariantError::wrong_backlink);
  core::test::CoreAccess::set_price_level(*rested.node, level);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookInvariants, DetectsMissingGhostAndWrongPointerIndexEntries) {
  InstrumentBook book{instrument_id};
  const auto rested = book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U));
  ASSERT_TRUE(rested);
  auto& index = core::test::CoreAccess::active_index(book);

  ASSERT_EQ(index.erase(*rested.node), ActiveOrderIndexError::none);
  EXPECT_EQ(book.validate_invariants().error,
            InstrumentBookInvariantError::storage_index_size_mismatch);
  ASSERT_EQ(index.insert(*rested.node), ActiveOrderIndexError::none);

  HeapOrderStorage foreign_storage;
  const auto ghost = foreign_storage.create(order_spec(999U, domain::Side::sell, 10'100, 7U, 2U));
  ASSERT_TRUE(ghost);
  ASSERT_EQ(index.insert(*ghost.node), ActiveOrderIndexError::none);
  EXPECT_EQ(book.validate_invariants().error,
            InstrumentBookInvariantError::storage_index_size_mismatch);
  ASSERT_EQ(index.erase(*ghost.node), ActiveOrderIndexError::none);

  const auto wrong_pointer =
      foreign_storage.create(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U));
  ASSERT_TRUE(wrong_pointer);
  EXPECT_EQ(
      core::test::CoreAccess::replace_index_entry(index, domain::OrderId{1U}, wrong_pointer.node),
      rested.node);
  const auto wrong_pointer_result = book.validate_invariants();
  EXPECT_EQ(wrong_pointer_result.error, InstrumentBookInvariantError::node_missing_from_index);
  EXPECT_EQ(wrong_pointer_result.node, rested.node);
  EXPECT_EQ(core::test::CoreAccess::replace_index_entry(index, domain::OrderId{1U}, rested.node),
            wrong_pointer.node);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookInvariants, DetectsIndexInvariantAndUntraversedIndexedNode) {
  InstrumentBook book{instrument_id};
  const auto rested = book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U));
  ASSERT_TRUE(rested);
  auto& index = core::test::CoreAccess::active_index(book);
  auto& storage = core::test::CoreAccess::storage(book);

  ASSERT_TRUE(core::test::CoreAccess::insert_index_entry(index, domain::OrderId{999U}, nullptr));
  const auto null_index = book.validate_invariants();
  EXPECT_EQ(null_index.error, InstrumentBookInvariantError::index_invariant);
  EXPECT_EQ(null_index.index_result.error, ActiveOrderIndexInvariantError::null_node);
  EXPECT_TRUE(core::test::CoreAccess::replace_index_entry(index, domain::OrderId{999U}, nullptr) ==
              nullptr);
  ASSERT_EQ(index.size(), 2U);
  ASSERT_TRUE(core::test::CoreAccess::erase_index_entry(index, domain::OrderId{999U}));

  const auto unlinked = storage.create(order_spec(2U, domain::Side::sell, 10'100, 7U, 2U));
  ASSERT_TRUE(unlinked);
  ASSERT_EQ(index.insert(*unlinked.node), ActiveOrderIndexError::none);
  const auto traversal_mismatch = book.validate_invariants();
  EXPECT_EQ(traversal_mismatch.error, InstrumentBookInvariantError::traversal_index_size_mismatch);
  EXPECT_EQ(traversal_mismatch.observed_order_count, 1U);
  ASSERT_EQ(index.erase(*unlinked.node), ActiveOrderIndexError::none);
  ASSERT_EQ(storage.destroy(*unlinked.node), StorageError::none);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookInvariants, DetectsACrossedOtherwiseValidBook) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U)));
  ASSERT_TRUE(book.rest(order_spec(2U, domain::Side::sell, 10'100, 7U, 2U)));

  auto& asks = core::test::CoreAccess::asks(book);
  ASSERT_TRUE(core::test::CoreAccess::reprice_level(asks, domain::PriceTicks{10'100},
                                                    domain::PriceTicks{9'900}));
  EXPECT_TRUE(asks.validate_invariants());
  const auto crossed = book.validate_invariants();
  EXPECT_EQ(crossed.error, InstrumentBookInvariantError::crossed_book);
  EXPECT_EQ(crossed.price, domain::PriceTicks{10'000});

  ASSERT_TRUE(core::test::CoreAccess::reprice_level(asks, domain::PriceTicks{9'900},
                                                    domain::PriceTicks{10'100}));
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookErrorVocabulary, HasStableStringRepresentations) {
  EXPECT_EQ(to_string(InstrumentBookError::duplicate_order_id), "duplicate_order_id");
  EXPECT_EQ(to_string(InstrumentBookError::price_level_failure), "price_level_failure");
  EXPECT_EQ(to_string(InstrumentBookInvariantError::node_missing_from_index),
            "node_missing_from_index");
  EXPECT_EQ(to_string(InstrumentBookInvariantError::crossed_book), "crossed_book");
  EXPECT_EQ(to_string(static_cast<InstrumentBookError>(255U)), "unknown");
  EXPECT_EQ(to_string(static_cast<InstrumentBookInvariantError>(255U)), "unknown");
}

}  // namespace
