#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "book_side.hpp"
#include "core_test_access.hpp"
#include "order_storage.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::core;

static_assert(BidBookSide::side() == domain::Side::buy);
static_assert(AskBookSide::side() == domain::Side::sell);
static_assert(!std::is_copy_constructible_v<BidBookSide>);
static_assert(!std::is_copy_assignable_v<BidBookSide>);
static_assert(!std::is_move_constructible_v<BidBookSide>);
static_assert(!std::is_move_assignable_v<BidBookSide>);
static_assert(!std::is_copy_constructible_v<AskBookSide>);
static_assert(!std::is_copy_assignable_v<AskBookSide>);
static_assert(!std::is_move_constructible_v<AskBookSide>);
static_assert(!std::is_move_assignable_v<AskBookSide>);
static_assert(!std::is_copy_constructible_v<BidBookSide::PreparedLevel>);
static_assert(!std::is_copy_assignable_v<BidBookSide::PreparedLevel>);
static_assert(std::is_move_constructible_v<BidBookSide::PreparedLevel>);
static_assert(std::is_move_assignable_v<BidBookSide::PreparedLevel>);
static_assert(!std::is_copy_constructible_v<BidBookSide::DetachedLevel>);
static_assert(!std::is_copy_assignable_v<BidBookSide::DetachedLevel>);
static_assert(std::is_move_constructible_v<BidBookSide::DetachedLevel>);
static_assert(std::is_move_assignable_v<BidBookSide::DetachedLevel>);
static_assert(std::is_same_v<decltype(*std::declval<BidBookSide&>().begin()), const PriceLevel&>);
static_assert(
    std::is_same_v<decltype(*std::declval<const AskBookSide&>().begin()), const PriceLevel&>);
static_assert(std::is_same_v<decltype(std::declval<BidBookSide&>().best_level()), PriceLevel*>);
static_assert(
    std::is_same_v<decltype(std::declval<const BidBookSide&>().best_level()), const PriceLevel*>);

struct RestingEntry final {
  PriceLevel* level{};
  OrderNode* node{};
};

template <typename Side>
class RestingCleanup final {
 public:
  RestingCleanup(Side& side, std::vector<RestingEntry>& entries) noexcept
      : side_{side}, entries_{entries} {}

  RestingCleanup(const RestingCleanup&) = delete;
  RestingCleanup& operator=(const RestingCleanup&) = delete;

  ~RestingCleanup() noexcept {
    for (auto position = entries_.rbegin(); position != entries_.rend(); ++position) {
      if (position->level == nullptr || position->node == nullptr) {
        continue;
      }
      if (position->node->price_level() == position->level) {
        (void)position->level->erase(*position->node);
      }
      if (position->level->empty() &&
          side_.find_level(position->level->price()) == position->level) {
        (void)side_.erase_level(*position->level);
      }
    }
  }

 private:
  Side& side_;
  std::vector<RestingEntry>& entries_;
};

template <typename Side>
std::vector<std::int64_t> level_prices(const Side& side) {
  std::vector<std::int64_t> result;
  result.reserve(side.level_count());
  for (const PriceLevel& level : side) {
    result.push_back(level.price().value());
  }
  return result;
}

OrderNodeSpec node_spec(std::uint64_t order_id, std::uint64_t quantity, std::uint64_t priority,
                        domain::Side side, std::int64_t price) {
  return {
      .order_id = domain::OrderId{order_id},
      .client_id = domain::ClientId{7U},
      .instrument_id = domain::InstrumentId{11U},
      .side = side,
      .price = domain::PriceTicks{price},
      .remaining_quantity = domain::Quantity{quantity},
      .priority_sequence = domain::Sequence{priority},
  };
}

template <typename Side>
RestingEntry rest_order(Side& side, HeapOrderStorage& storage, std::vector<RestingEntry>& tracked,
                        std::uint64_t order_id, std::int64_t price, std::uint64_t quantity = 1U,
                        std::uint64_t priority = 1U) {
  const auto created = storage.create(node_spec(order_id, quantity, priority, Side::side(), price));
  if (!created) {
    ADD_FAILURE() << "storage create failed: " << to_string(created.error);
    return {};
  }

  auto prepared = side.prepare_level(domain::PriceTicks{price});
  if (!prepared) {
    ADD_FAILURE() << "level preparation failed: " << to_string(prepared.error());
    return {};
  }

  auto* const level = prepared.level();
  const auto append_error = level->append(*created.node);
  if (append_error != PriceLevelError::none) {
    ADD_FAILURE() << "append failed: " << to_string(append_error);
    return {};
  }

  const auto commit_error = prepared.commit();
  if (commit_error != BookSideError::none) {
    ADD_FAILURE() << "commit failed: " << to_string(commit_error);
    return {};
  }

  const RestingEntry result{.level = level, .node = created.node};
  tracked.push_back(result);
  return result;
}

void release_tracked(std::vector<RestingEntry>& tracked, const RestingEntry& released) noexcept {
  for (auto& entry : tracked) {
    if (entry.node == released.node) {
      entry = {};
      return;
    }
  }
}

TEST(BookSide, EmptySideHasNoBestOrFindResults) {
  BidBookSide bids;
  const auto& const_bids = std::as_const(bids);

  EXPECT_TRUE(bids.empty());
  EXPECT_EQ(bids.level_count(), 0U);
  EXPECT_EQ(bids.best_level(), nullptr);
  EXPECT_EQ(const_bids.best_level(), nullptr);
  EXPECT_EQ(bids.find_level(domain::PriceTicks{10'000}), nullptr);
  EXPECT_EQ(const_bids.find_level(domain::PriceTicks{10'000}), nullptr);
  EXPECT_EQ(bids.begin(), bids.end());
  EXPECT_EQ(const_bids.cbegin(), const_bids.cend());
  EXPECT_TRUE(bids.validate_invariants());
}

TEST(BookSidePreparedLevel, RollsBackAnUncommittedNewEmptyLevel) {
  AskBookSide asks;

  {
    auto prepared = asks.prepare_level(domain::PriceTicks{10'250});
    ASSERT_TRUE(prepared);
    EXPECT_TRUE(prepared.has_value());
    EXPECT_TRUE(prepared.created());
    EXPECT_EQ(prepared.error(), BookSideError::none);
    ASSERT_NE(prepared.level(), nullptr);
    EXPECT_EQ(asks.level_count(), 1U);
    EXPECT_EQ(asks.find_level(domain::PriceTicks{10'250}), prepared.level());
    EXPECT_EQ(prepared.commit(), BookSideError::prepared_level_empty);
    EXPECT_EQ(asks.validate_invariants().error, BookSideInvariantError::empty_level);
  }

  EXPECT_TRUE(asks.empty());
  EXPECT_EQ(asks.find_level(domain::PriceTicks{10'250}), nullptr);
  EXPECT_TRUE(asks.validate_invariants());
}

TEST(BookSidePreparedLevel, AppendAndCommitPublishAValidLevel) {
  HeapOrderStorage storage;
  BidBookSide bids;
  std::vector<RestingEntry> tracked;
  RestingCleanup cleanup{bids, tracked};
  const auto created = storage.create(node_spec(1U, 7U, 1U, domain::Side::buy, 10'250));
  ASSERT_TRUE(created);

  auto prepared = bids.prepare_level(domain::PriceTicks{10'250});
  ASSERT_TRUE(prepared);
  EXPECT_TRUE(prepared.created());
  auto* const level = prepared.level();
  ASSERT_NE(level, nullptr);
  ASSERT_EQ(level->append(*created.node), PriceLevelError::none);
  ASSERT_EQ(prepared.commit(), BookSideError::none);
  tracked.push_back({.level = level, .node = created.node});

  EXPECT_FALSE(prepared.created());
  EXPECT_EQ(bids.find_level(domain::PriceTicks{10'250}), level);
  EXPECT_EQ(bids.best_level(), level);
  EXPECT_TRUE(bids.validate_invariants());
}

TEST(BookSidePreparedLevel, MoveConstructionTransfersCommitResponsibility) {
  HeapOrderStorage storage;
  BidBookSide bids;
  std::vector<RestingEntry> tracked;
  RestingCleanup cleanup{bids, tracked};
  const auto created = storage.create(node_spec(1U, 7U, 1U, domain::Side::buy, 10'250));
  ASSERT_TRUE(created);

  auto original = bids.prepare_level(domain::PriceTicks{10'250});
  ASSERT_TRUE(original);
  auto* const level = original.level();
  auto moved = std::move(original);

  EXPECT_FALSE(original);
  EXPECT_EQ(original.level(), nullptr);
  ASSERT_TRUE(moved);
  EXPECT_TRUE(moved.created());
  EXPECT_EQ(moved.level(), level);
  EXPECT_EQ(bids.find_level(domain::PriceTicks{10'250}), level);

  ASSERT_EQ(level->append(*created.node), PriceLevelError::none);
  ASSERT_EQ(moved.commit(), BookSideError::none);
  tracked.push_back({.level = level, .node = created.node});
  EXPECT_EQ(bids.find_level(domain::PriceTicks{10'250}), level);
  EXPECT_TRUE(bids.validate_invariants());
}

TEST(BookSidePreparedLevel, MoveAssignmentRollsBackTheReplacedGuardOnly) {
  AskBookSide asks;

  {
    auto first = asks.prepare_level(domain::PriceTicks{10'100});
    auto second = asks.prepare_level(domain::PriceTicks{10'200});
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    auto* const second_level = second.level();

    first = std::move(second);

    EXPECT_FALSE(second);
    EXPECT_EQ(second.level(), nullptr);
    EXPECT_EQ(asks.find_level(domain::PriceTicks{10'100}), nullptr);
    EXPECT_EQ(asks.find_level(domain::PriceTicks{10'200}), second_level);
    EXPECT_EQ(asks.level_count(), 1U);
  }

  EXPECT_TRUE(asks.empty());
  EXPECT_TRUE(asks.validate_invariants());
}

TEST(BookSidePreparedLevel, DroppingExistingLevelPreparationDoesNotEraseIt) {
  HeapOrderStorage storage;
  AskBookSide asks;
  std::vector<RestingEntry> tracked;
  RestingCleanup cleanup{asks, tracked};
  const auto original = rest_order(asks, storage, tracked, 1U, 10'250, 7U, 1U);
  ASSERT_NE(original.level, nullptr);

  {
    auto existing = asks.prepare_level(domain::PriceTicks{10'250});
    ASSERT_TRUE(existing);
    EXPECT_FALSE(existing.created());
    EXPECT_EQ(existing.level(), original.level);
  }

  EXPECT_EQ(asks.level_count(), 1U);
  EXPECT_EQ(asks.find_level(domain::PriceTicks{10'250}), original.level);
  EXPECT_TRUE(asks.validate_invariants());
}

TEST(BookSideDetachedLevel, RejectsInvalidPriceAndEmptyPublishWithoutVisibleMutation) {
  BidBookSide bids;

  auto invalid = bids.prepare_detached_level(domain::PriceTicks{});
  EXPECT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), BookSideError::invalid_price);
  EXPECT_TRUE(bids.empty());

  auto empty = bids.prepare_detached_level(domain::PriceTicks{10'000});
  ASSERT_TRUE(empty);
  ASSERT_NE(empty.level(), nullptr);
  EXPECT_EQ(empty.publish(), BookSideError::prepared_level_empty);
  EXPECT_TRUE(bids.empty());
  EXPECT_TRUE(bids.validate_invariants());
}

TEST(BookSideDetachedLevel, ConflictPreservesTheVisibleAndDetachedLevels) {
  HeapOrderStorage storage;
  BidBookSide bids;
  std::vector<RestingEntry> tracked;
  RestingCleanup cleanup{bids, tracked};
  const auto existing = rest_order(bids, storage, tracked, 1U, 10'000, 5U, 1U);
  ASSERT_NE(existing.level, nullptr);
  const auto staged = storage.create(node_spec(2U, 7U, 2U, domain::Side::buy, 10'000));
  ASSERT_TRUE(staged);

  auto detached = bids.prepare_detached_level(domain::PriceTicks{10'000});
  ASSERT_TRUE(detached);
  auto* const detached_level = detached.level();
  ASSERT_NE(detached_level, nullptr);
  ASSERT_EQ(detached_level->append(*staged.node), PriceLevelError::none);

  EXPECT_EQ(detached.publish(), BookSideError::prepared_level_conflict);
  EXPECT_EQ(bids.find_level(domain::PriceTicks{10'000}), existing.level);
  EXPECT_EQ(existing.level->aggregate_quantity(), domain::Quantity{5U});
  EXPECT_EQ(detached.level(), detached_level);
  EXPECT_EQ(detached_level->aggregate_quantity(), domain::Quantity{7U});
  EXPECT_TRUE(bids.validate_invariants());

  ASSERT_EQ(detached_level->erase(*staged.node), PriceLevelError::none);
  ASSERT_EQ(storage.destroy(*staged.node), StorageError::none);
}

TEST(BookSideDetachedLevel, MoveConstructionAndAssignmentTransferTheMapNode) {
  AskBookSide asks;
  auto first = asks.prepare_detached_level(domain::PriceTicks{10'100});
  ASSERT_TRUE(first);
  auto* const first_level = first.level();

  AskBookSide::DetachedLevel second{std::move(first)};
  EXPECT_FALSE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(second.level(), first_level);

  auto third = asks.prepare_detached_level(domain::PriceTicks{10'200});
  ASSERT_TRUE(third);
  third = std::move(second);
  EXPECT_FALSE(second);
  ASSERT_TRUE(third);
  EXPECT_EQ(third.level(), first_level);
  EXPECT_TRUE(asks.empty());
  EXPECT_TRUE(asks.validate_invariants());
}

TEST(BookSide, BidsIterateFromHighestToLowestPrice) {
  HeapOrderStorage storage;
  BidBookSide bids;
  std::vector<RestingEntry> tracked;
  RestingCleanup cleanup{bids, tracked};
  std::uint64_t order_id = 1U;
  for (const auto price : {10'100, 9'900, 10'500, 10'000}) {
    ASSERT_NE(rest_order(bids, storage, tracked, order_id++, price).level, nullptr);
  }

  EXPECT_FALSE(bids.empty());
  EXPECT_EQ(bids.level_count(), 4U);
  ASSERT_NE(bids.best_level(), nullptr);
  EXPECT_EQ(bids.best_level()->price(), domain::PriceTicks{10'500});
  EXPECT_EQ(level_prices(bids), (std::vector<std::int64_t>{10'500, 10'100, 10'000, 9'900}));
  EXPECT_TRUE(bids.validate_invariants());
}

TEST(BookSide, AsksIterateFromLowestToHighestPrice) {
  HeapOrderStorage storage;
  AskBookSide asks;
  std::vector<RestingEntry> tracked;
  RestingCleanup cleanup{asks, tracked};
  std::uint64_t order_id = 1U;
  for (const auto price : {10'100, 9'900, 10'500, 10'000}) {
    ASSERT_NE(rest_order(asks, storage, tracked, order_id++, price).level, nullptr);
  }

  EXPECT_FALSE(asks.empty());
  EXPECT_EQ(asks.level_count(), 4U);
  ASSERT_NE(asks.best_level(), nullptr);
  EXPECT_EQ(asks.best_level()->price(), domain::PriceTicks{9'900});
  EXPECT_EQ(level_prices(asks), (std::vector<std::int64_t>{9'900, 10'000, 10'100, 10'500}));
  EXPECT_TRUE(asks.validate_invariants());
}

TEST(BookSide, PrepareAndFindExposeTheSameStableExistingLevel) {
  HeapOrderStorage storage;
  BidBookSide bids;
  std::vector<RestingEntry> tracked;
  RestingCleanup cleanup{bids, tracked};
  const auto first = rest_order(bids, storage, tracked, 1U, 10'250, 7U, 1U);
  ASSERT_NE(first.level, nullptr);

  auto duplicate = bids.prepare_level(domain::PriceTicks{10'250});
  ASSERT_TRUE(duplicate);
  EXPECT_EQ(duplicate.level(), first.level);
  EXPECT_FALSE(duplicate.created());
  EXPECT_EQ(duplicate.error(), BookSideError::none);
  EXPECT_EQ(duplicate.commit(), BookSideError::none);
  EXPECT_EQ(bids.level_count(), 1U);

  const auto& const_bids = std::as_const(bids);
  EXPECT_EQ(const_bids.find_level(domain::PriceTicks{10'250}), first.level);
  EXPECT_EQ(const_bids.best_level(), first.level);
  EXPECT_TRUE(bids.validate_invariants());
}

TEST(BookSide, PrepareRejectsInvalidPricesWithoutMutation) {
  HeapOrderStorage storage;
  AskBookSide asks;
  std::vector<RestingEntry> tracked;
  RestingCleanup cleanup{asks, tracked};
  const auto valid = rest_order(asks, storage, tracked, 1U, 10'250, 7U, 1U);
  ASSERT_NE(valid.level, nullptr);

  auto zero = asks.prepare_level(domain::PriceTicks{});
  EXPECT_FALSE(zero);
  EXPECT_FALSE(zero.has_value());
  EXPECT_EQ(zero.level(), nullptr);
  EXPECT_FALSE(zero.created());
  EXPECT_EQ(zero.error(), BookSideError::invalid_price);

  auto negative = asks.prepare_level(domain::PriceTicks{-1});
  EXPECT_FALSE(negative);
  EXPECT_EQ(negative.level(), nullptr);
  EXPECT_FALSE(negative.created());
  EXPECT_EQ(negative.error(), BookSideError::invalid_price);

  EXPECT_EQ(asks.level_count(), 1U);
  EXPECT_EQ(asks.best_level(), valid.level);
  EXPECT_EQ(asks.find_level(domain::PriceTicks{10'250}), valid.level);
  EXPECT_TRUE(asks.validate_invariants());
}

TEST(BookSide, LevelAddressesRemainStableAcrossMapGrowth) {
  HeapOrderStorage storage;
  BidBookSide bids;
  std::vector<RestingEntry> tracked;
  RestingCleanup cleanup{bids, tracked};
  std::vector<std::pair<domain::PriceTicks, PriceLevel*>> saved;
  tracked.reserve(2'048U);
  saved.reserve(2'048U);

  for (std::int64_t price = 1; price <= 2'048; ++price) {
    const auto entry = rest_order(bids, storage, tracked, static_cast<std::uint64_t>(price), price);
    ASSERT_NE(entry.level, nullptr) << "price=" << price;
    saved.emplace_back(domain::PriceTicks{price}, entry.level);
  }

  EXPECT_EQ(bids.level_count(), saved.size());
  EXPECT_EQ(bids.best_level(), saved.back().second);
  for (const auto& [price, address] : saved) {
    ASSERT_NE(address, nullptr);
    EXPECT_EQ(address->price(), price);
    EXPECT_EQ(bids.find_level(price), address);
  }
  EXPECT_TRUE(bids.validate_invariants());
}

TEST(BookSide, ErasesOnlyTheExactEmptyOwnedLevel) {
  HeapOrderStorage storage;
  BidBookSide bids;
  std::vector<RestingEntry> tracked;
  RestingCleanup cleanup{bids, tracked};
  const auto owned = rest_order(bids, storage, tracked, 1U, 10'250, 7U, 1U);
  ASSERT_NE(owned.level, nullptr);
  PriceLevel missing{domain::PriceTicks{10'251}};
  PriceLevel wrong_identity{domain::PriceTicks{10'250}};
  PriceLevel invalid_price{domain::PriceTicks{}};

  EXPECT_EQ(bids.erase_level(invalid_price), BookSideError::invalid_price);
  EXPECT_EQ(bids.erase_level(missing), BookSideError::level_not_found);
  EXPECT_EQ(bids.erase_level(wrong_identity), BookSideError::level_identity_mismatch);
  EXPECT_EQ(bids.level_count(), 1U);
  EXPECT_EQ(bids.find_level(domain::PriceTicks{10'250}), owned.level);

  ASSERT_EQ(owned.level->erase(*owned.node), PriceLevelError::none);
  ASSERT_EQ(bids.erase_level(*owned.level), BookSideError::none);
  release_tracked(tracked, owned);
  EXPECT_TRUE(bids.empty());
  EXPECT_EQ(bids.best_level(), nullptr);
  EXPECT_EQ(bids.find_level(domain::PriceTicks{10'250}), nullptr);
  EXPECT_TRUE(bids.validate_invariants());
}

TEST(BookSide, NonemptyEraseFailureIsAtomic) {
  HeapOrderStorage storage;
  BidBookSide bids;
  std::vector<RestingEntry> tracked;
  RestingCleanup cleanup{bids, tracked};
  const auto owned = rest_order(bids, storage, tracked, 1U, 10'000, 7U, 1U);
  ASSERT_NE(owned.level, nullptr);

  auto* const head = owned.level->head();
  auto* const tail = owned.level->tail();
  const auto aggregate = owned.level->aggregate_quantity();
  const auto count = owned.level->order_count();

  EXPECT_EQ(bids.erase_level(*owned.level), BookSideError::level_not_empty);
  EXPECT_EQ(bids.level_count(), 1U);
  EXPECT_EQ(bids.best_level(), owned.level);
  EXPECT_EQ(bids.find_level(domain::PriceTicks{10'000}), owned.level);
  EXPECT_EQ(owned.level->head(), head);
  EXPECT_EQ(owned.level->tail(), tail);
  EXPECT_EQ(owned.level->aggregate_quantity(), aggregate);
  EXPECT_EQ(owned.level->order_count(), count);
  EXPECT_EQ(owned.node->price_level(), owned.level);
  EXPECT_TRUE(owned.level->validate_invariants());
  EXPECT_TRUE(bids.validate_invariants());
}

TEST(BookSide, ErasingAnotherLevelPreservesLiveLevelAddress) {
  HeapOrderStorage storage;
  AskBookSide asks;
  std::vector<RestingEntry> tracked;
  RestingCleanup cleanup{asks, tracked};
  const auto retained = rest_order(asks, storage, tracked, 1U, 10'100, 7U, 1U);
  const auto removed = rest_order(asks, storage, tracked, 2U, 10'200, 5U, 2U);
  ASSERT_NE(retained.level, nullptr);
  ASSERT_NE(removed.level, nullptr);
  auto* const saved_address = retained.level;

  ASSERT_EQ(removed.level->erase(*removed.node), PriceLevelError::none);
  ASSERT_EQ(asks.erase_level(*removed.level), BookSideError::none);
  release_tracked(tracked, removed);

  EXPECT_EQ(asks.level_count(), 1U);
  EXPECT_EQ(asks.best_level(), saved_address);
  EXPECT_EQ(asks.find_level(domain::PriceTicks{10'100}), saved_address);
  EXPECT_EQ(saved_address->head(), retained.node);
  EXPECT_EQ(retained.node->price_level(), saved_address);
  EXPECT_TRUE(asks.validate_invariants());
}

TEST(BookSideInvariants, DetectsNullMappedLevelOwnership) {
  BidBookSide bids;
  ASSERT_TRUE(core::test::CoreAccess::insert_null_level(bids, domain::PriceTicks{10'250}));

  const auto invalid = bids.validate_invariants();
  EXPECT_EQ(invalid.error, BookSideInvariantError::null_level);
  EXPECT_EQ(invalid.price, domain::PriceTicks{10'250});
  EXPECT_EQ(invalid.level, nullptr);

  ASSERT_TRUE(core::test::CoreAccess::erase_level_entry(bids, domain::PriceTicks{10'250}));
  EXPECT_TRUE(bids.empty());
  EXPECT_TRUE(bids.validate_invariants());
}

TEST(BookSideInvariants, DetectsMapKeyAndLevelPriceMismatch) {
  HeapOrderStorage storage;
  BidBookSide bids;
  std::vector<RestingEntry> tracked;
  RestingCleanup cleanup{bids, tracked};
  const auto owned = rest_order(bids, storage, tracked, 1U, 10'250, 7U, 1U);
  ASSERT_NE(owned.level, nullptr);
  core::test::CoreAccess::set_level_price(*owned.level, domain::PriceTicks{10'251});

  const auto invalid = bids.validate_invariants();
  EXPECT_EQ(invalid.error, BookSideInvariantError::key_price_mismatch);
  EXPECT_EQ(invalid.price, domain::PriceTicks{10'250});
  EXPECT_EQ(invalid.level, owned.level);

  core::test::CoreAccess::set_level_price(*owned.level, domain::PriceTicks{10'250});
  EXPECT_TRUE(bids.validate_invariants());
}

TEST(BookSideInvariants, PropagatesDetailedPriceLevelFailure) {
  HeapOrderStorage storage;
  AskBookSide asks;
  std::vector<RestingEntry> tracked;
  RestingCleanup cleanup{asks, tracked};
  const auto owned = rest_order(asks, storage, tracked, 1U, 10'000, 7U, 1U);
  ASSERT_NE(owned.level, nullptr);
  core::test::CoreAccess::set_level_aggregate(*owned.level, domain::Quantity{8U});

  const auto invalid = asks.validate_invariants();
  EXPECT_EQ(invalid.error, BookSideInvariantError::level_invariant_violation);
  EXPECT_EQ(invalid.price, domain::PriceTicks{10'000});
  EXPECT_EQ(invalid.level, owned.level);
  EXPECT_EQ(invalid.level_result.error, PriceLevelInvariantError::aggregate_mismatch);
  EXPECT_EQ(invalid.level_result.observed_order_count, 1U);
  EXPECT_EQ(invalid.level_result.observed_aggregate_quantity, domain::Quantity{7U});

  core::test::CoreAccess::set_level_aggregate(*owned.level, domain::Quantity{7U});
  EXPECT_TRUE(asks.validate_invariants());
}

TEST(BookSideErrorVocabulary, HasStableStringRepresentations) {
  EXPECT_EQ(to_string(BookSideError::prepared_level_empty), "prepared_level_empty");
  EXPECT_EQ(to_string(BookSideInvariantError::key_price_mismatch), "key_price_mismatch");
  EXPECT_EQ(to_string(static_cast<BookSideError>(255U)), "unknown");
  EXPECT_EQ(to_string(static_cast<BookSideInvariantError>(255U)), "unknown");
}

}  // namespace
