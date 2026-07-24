#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include "instrument_book.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::core;

constexpr domain::InstrumentId instrument_id{11U};

static_assert(!std::is_copy_constructible_v<InstrumentBook::PreparedRest>);
static_assert(!std::is_copy_assignable_v<InstrumentBook::PreparedRest>);
static_assert(std::is_nothrow_move_constructible_v<InstrumentBook::PreparedRest>);
static_assert(std::is_nothrow_move_assignable_v<InstrumentBook::PreparedRest>);
static_assert(std::is_nothrow_destructible_v<InstrumentBook::PreparedRest>);
static_assert(noexcept(std::declval<InstrumentBook::PreparedRest&>().commit()));

[[nodiscard]] OrderNodeSpec order_spec(std::uint64_t order_id, domain::Side side,
                                       std::int64_t price, std::uint64_t quantity,
                                       std::uint64_t priority) {
  return {
      .order_id = domain::OrderId{order_id},
      .client_id = domain::ClientId{7U},
      .instrument_id = instrument_id,
      .side = side,
      .price = domain::PriceTicks{price},
      .remaining_quantity = domain::Quantity{quantity},
      .priority_sequence = domain::Sequence{priority},
  };
}

TEST(InstrumentBookPreparedRest, ExistingLevelIsUnchangedUntilAllocationFreeCommit) {
  InstrumentBook book{instrument_id};
  const auto original = book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U));
  ASSERT_TRUE(original);
  auto* const level = book.best_bid();
  ASSERT_NE(level, nullptr);

  auto prepared = book.prepare_rest(order_spec(2U, domain::Side::buy, 10'000, 7U, 2U));
  ASSERT_TRUE(prepared);
  ASSERT_NE(prepared.node(), nullptr);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_EQ(book.best_bid(), level);
  EXPECT_EQ(level->head(), original.node);
  EXPECT_EQ(level->tail(), original.node);
  EXPECT_EQ(level->order_count(), 1U);
  EXPECT_EQ(level->aggregate_quantity(), domain::Quantity{5U});
  EXPECT_TRUE(book.validate_invariants());

  const auto committed = prepared.commit();
  ASSERT_TRUE(committed);
  EXPECT_EQ(committed.node, book.find(domain::OrderId{2U}));
  EXPECT_EQ(book.active_order_count(), 2U);
  EXPECT_EQ(book.best_bid(), level);
  EXPECT_EQ(level->head(), original.node);
  EXPECT_EQ(level->tail(), committed.node);
  EXPECT_EQ(original.node->next(), committed.node);
  EXPECT_EQ(committed.node->previous(), original.node);
  EXPECT_EQ(level->order_count(), 2U);
  EXPECT_EQ(level->aggregate_quantity(), domain::Quantity{12U});
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookPreparedRest, NewLevelIsDetachedUntilCommitPublishesIt) {
  InstrumentBook book{instrument_id};
  const auto original = book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U));
  ASSERT_TRUE(original);
  auto* const original_level = book.best_bid();
  ASSERT_NE(original_level, nullptr);

  auto prepared = book.prepare_rest(order_spec(2U, domain::Side::buy, 9'900, 7U, 2U));
  ASSERT_TRUE(prepared);
  EXPECT_EQ(book.bids().level_count(), 1U);
  EXPECT_EQ(book.bids().find_level(domain::PriceTicks{9'900}), nullptr);
  EXPECT_EQ(book.best_bid(), original_level);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_TRUE(book.validate_invariants());

  const auto committed = prepared.commit();
  ASSERT_TRUE(committed);
  auto* const published = book.bids().find_level(domain::PriceTicks{9'900});
  ASSERT_NE(published, nullptr);
  EXPECT_EQ(published->head(), committed.node);
  EXPECT_EQ(published->tail(), committed.node);
  EXPECT_EQ(published->order_count(), 1U);
  EXPECT_EQ(published->aggregate_quantity(), domain::Quantity{7U});
  EXPECT_EQ(book.bids().level_count(), 2U);
  EXPECT_EQ(book.best_bid(), original_level);
  EXPECT_EQ(book.find(domain::OrderId{2U}), committed.node);
  EXPECT_EQ(book.active_order_count(), 2U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookPreparedRest, AbandonmentRestoresExactExistingAndNewLevelState) {
  InstrumentBook book{instrument_id};
  const auto original = book.rest(order_spec(1U, domain::Side::sell, 10'100, 5U, 1U));
  ASSERT_TRUE(original);
  auto* const level = book.best_ask();
  ASSERT_NE(level, nullptr);

  {
    auto existing = book.prepare_rest(order_spec(2U, domain::Side::sell, 10'100, 7U, 2U));
    ASSERT_TRUE(existing);
    EXPECT_EQ(book.active_order_count(), 1U);
    EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
    EXPECT_EQ(level->head(), original.node);
    EXPECT_EQ(level->tail(), original.node);
    EXPECT_EQ(level->aggregate_quantity(), domain::Quantity{5U});
    EXPECT_TRUE(book.validate_invariants());
  }

  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_EQ(book.best_ask(), level);
  EXPECT_EQ(level->head(), original.node);
  EXPECT_EQ(level->tail(), original.node);
  EXPECT_EQ(level->order_count(), 1U);
  EXPECT_EQ(level->aggregate_quantity(), domain::Quantity{5U});
  EXPECT_TRUE(book.validate_invariants());

  {
    auto detached = book.prepare_rest(order_spec(3U, domain::Side::sell, 10'200, 11U, 3U));
    ASSERT_TRUE(detached);
    EXPECT_EQ(book.asks().find_level(domain::PriceTicks{10'200}), nullptr);
    EXPECT_EQ(book.asks().level_count(), 1U);
    EXPECT_EQ(book.active_order_count(), 1U);
    EXPECT_TRUE(book.validate_invariants());
  }

  EXPECT_EQ(book.asks().find_level(domain::PriceTicks{10'200}), nullptr);
  EXPECT_EQ(book.asks().level_count(), 1U);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_EQ(book.find(domain::OrderId{3U}), nullptr);
  EXPECT_EQ(book.best_ask(), level);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookPreparedRest, PreallocatedFallbackSurvivesOriginalLevelRemoval) {
  InstrumentBook book{instrument_id};
  const auto original = book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U));
  ASSERT_TRUE(original);

  auto prepared = book.prepare_rest(order_spec(2U, domain::Side::buy, 10'000, 7U, 2U));
  ASSERT_TRUE(prepared);
  ASSERT_TRUE(book.cancel(domain::OrderId{1U}));
  EXPECT_TRUE(book.empty());
  EXPECT_EQ(book.best_bid(), nullptr);
  EXPECT_EQ(book.bids().find_level(domain::PriceTicks{10'000}), nullptr);
  EXPECT_TRUE(book.validate_invariants());

  const auto committed = prepared.commit();
  ASSERT_TRUE(committed);
  ASSERT_NE(book.best_bid(), nullptr);
  EXPECT_EQ(book.best_bid()->price(), domain::PriceTicks{10'000});
  EXPECT_EQ(book.best_bid()->head(), committed.node);
  EXPECT_EQ(book.best_bid()->tail(), committed.node);
  EXPECT_EQ(book.best_bid()->aggregate_quantity(), domain::Quantity{7U});
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_EQ(book.find(domain::OrderId{2U}), committed.node);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookPreparedRest, CrossingResidualCanBePreparedThenCommittedAfterPlannedRemoval) {
  InstrumentBook book{instrument_id};
  const auto resting_ask = book.rest(order_spec(1U, domain::Side::sell, 10'100, 5U, 1U));
  ASSERT_TRUE(resting_ask);

  auto prepared = book.prepare_rest(order_spec(2U, domain::Side::buy, 10'200, 7U, 2U));
  ASSERT_TRUE(prepared);
  EXPECT_EQ(book.best_bid(), nullptr);
  EXPECT_EQ(book.best_ask(), resting_ask.node->price_level());
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_TRUE(book.validate_invariants());

  ASSERT_TRUE(book.cancel(domain::OrderId{1U}));
  EXPECT_TRUE(book.empty());
  EXPECT_EQ(book.best_ask(), nullptr);
  EXPECT_TRUE(book.validate_invariants());

  const auto committed = prepared.commit();
  ASSERT_TRUE(committed);
  EXPECT_EQ(book.best_bid(), committed.node->price_level());
  EXPECT_EQ(book.best_bid()->price(), domain::PriceTicks{10'200});
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookPreparedRest, MoveTransfersRollbackAndCommitResponsibility) {
  InstrumentBook book{instrument_id};
  auto original = book.prepare_rest(order_spec(1U, domain::Side::sell, 10'100, 5U, 1U));
  ASSERT_TRUE(original);

  auto moved = std::move(original);
  EXPECT_FALSE(original);
  EXPECT_TRUE(moved);
  EXPECT_EQ(book.active_order_count(), 0U);
  EXPECT_EQ(book.find(domain::OrderId{1U}), nullptr);
  EXPECT_TRUE(book.validate_invariants());

  const auto committed = moved.commit();
  ASSERT_TRUE(committed);
  EXPECT_EQ(book.find(domain::OrderId{1U}), committed.node);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookPreparedRest, DuplicateAndAppendPreconditionsFailWithoutMutation) {
  InstrumentBook book{instrument_id};
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto original = book.rest(order_spec(1U, domain::Side::sell, 10'100, maximum, 10U));
  ASSERT_TRUE(original);
  auto* const level = book.best_ask();
  ASSERT_NE(level, nullptr);

  const auto duplicate = book.prepare_rest(order_spec(1U, domain::Side::sell, 10'200, 1U, 11U));
  EXPECT_FALSE(duplicate);
  EXPECT_EQ(duplicate.status().error, InstrumentBookError::duplicate_order_id);

  const auto nonmonotonic = book.prepare_rest(order_spec(2U, domain::Side::sell, 10'100, 1U, 9U));
  EXPECT_FALSE(nonmonotonic);
  EXPECT_EQ(nonmonotonic.status().error, InstrumentBookError::price_level_failure);
  EXPECT_EQ(nonmonotonic.status().level_error, PriceLevelError::nonmonotonic_priority);

  const auto overflow = book.prepare_rest(order_spec(3U, domain::Side::sell, 10'100, 1U, 11U));
  EXPECT_FALSE(overflow);
  EXPECT_EQ(overflow.status().error, InstrumentBookError::price_level_failure);
  EXPECT_EQ(overflow.status().level_error, PriceLevelError::aggregate_overflow);

  const auto invalid = book.prepare_rest(order_spec(4U, domain::Side::sell, 10'200, 0U, 12U));
  EXPECT_FALSE(invalid);
  EXPECT_EQ(invalid.status().storage_error, StorageError::invalid_remaining_quantity);

  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_EQ(book.best_ask(), level);
  EXPECT_EQ(level->head(), original.node);
  EXPECT_EQ(level->tail(), original.node);
  EXPECT_EQ(level->order_count(), 1U);
  EXPECT_EQ(level->aggregate_quantity(), domain::Quantity{maximum});
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{3U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{4U}), nullptr);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookPreparedRest, SecondPreparationAndOrdinaryCrossingRestFailAtomically) {
  InstrumentBook book{instrument_id};
  const auto ask = book.rest(order_spec(1U, domain::Side::sell, 10'100, 5U, 1U));
  ASSERT_TRUE(ask);

  auto pending = book.prepare_rest(order_spec(2U, domain::Side::buy, 10'200, 7U, 2U));
  ASSERT_TRUE(pending);
  auto second = book.prepare_rest(order_spec(3U, domain::Side::buy, 9'900, 11U, 3U));
  EXPECT_FALSE(second);
  EXPECT_EQ(second.status().error, InstrumentBookError::preparation_in_progress);
  EXPECT_EQ(book.find(domain::OrderId{3U}), nullptr);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_TRUE(book.validate_invariants());

  // Abandon the aggressive plan before exercising ordinary resting semantics.
  pending = std::move(second);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_TRUE(book.validate_invariants());

  const auto crossing = book.rest(order_spec(4U, domain::Side::buy, 10'100, 13U, 4U));
  EXPECT_FALSE(crossing);
  EXPECT_EQ(crossing.status.error, InstrumentBookError::would_cross_book);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_EQ(book.best_bid(), nullptr);
  EXPECT_EQ(book.best_ask(), ask.node->price_level());
  EXPECT_EQ(book.find(domain::OrderId{4U}), nullptr);
  EXPECT_TRUE(book.validate_invariants());
}

}  // namespace
