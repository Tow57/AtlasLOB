#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "core_test_access.hpp"
#include "order_storage.hpp"
#include "price_level.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::core;

static_assert(!std::is_copy_constructible_v<PriceLevel>);
static_assert(!std::is_copy_assignable_v<PriceLevel>);
static_assert(!std::is_move_constructible_v<PriceLevel>);
static_assert(!std::is_move_assignable_v<PriceLevel>);

OrderNodeSpec node_spec(std::uint64_t id, std::uint64_t quantity, std::uint64_t priority,
                        std::int64_t price = 10'250) {
  return {
      .order_id = domain::OrderId{id},
      .client_id = domain::ClientId{7U},
      .instrument_id = domain::InstrumentId{11U},
      .side = domain::Side::buy,
      .price = domain::PriceTicks{price},
      .remaining_quantity = domain::Quantity{quantity},
      .priority_sequence = domain::Sequence{priority},
  };
}

struct LevelSnapshot final {
  domain::Quantity aggregate{};
  std::size_t count{};
  const OrderNode* head{};
  const OrderNode* tail{};

  bool operator==(const LevelSnapshot&) const = default;
};

struct NodeSnapshot final {
  domain::Quantity remaining{};
  domain::Sequence priority{};
  const OrderNode* previous{};
  const OrderNode* next{};
  const PriceLevel* level{};

  bool operator==(const NodeSnapshot&) const = default;
};

LevelSnapshot snapshot(const PriceLevel& level) {
  return {
      .aggregate = level.aggregate_quantity(),
      .count = level.order_count(),
      .head = level.head(),
      .tail = level.tail(),
  };
}

NodeSnapshot snapshot(const OrderNode& node) {
  return {
      .remaining = node.remaining_quantity(),
      .priority = node.priority_sequence(),
      .previous = node.previous(),
      .next = node.next(),
      .level = node.price_level(),
  };
}

TEST(PriceLevel, RunsTheCanonicalFifoScenario) {
  HeapOrderStorage storage;
  PriceLevel level{domain::PriceTicks{10'250}};
  const auto first = storage.create(node_spec(1U, 5U, 1U));
  const auto second = storage.create(node_spec(2U, 7U, 2U));
  const auto third = storage.create(node_spec(3U, 11U, 3U));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(third);

  EXPECT_EQ(level.append(*first.node), PriceLevelError::none);
  EXPECT_TRUE(level.validate_invariants());
  EXPECT_EQ(level.append(*second.node), PriceLevelError::none);
  EXPECT_TRUE(level.validate_invariants());
  EXPECT_EQ(level.append(*third.node), PriceLevelError::none);
  EXPECT_TRUE(level.validate_invariants());
  EXPECT_EQ(level.head(), first.node);
  EXPECT_EQ(first.node->next(), second.node);
  EXPECT_EQ(second.node->next(), third.node);
  EXPECT_EQ(level.tail(), third.node);
  EXPECT_EQ(level.order_count(), 3U);
  EXPECT_EQ(level.aggregate_quantity(), domain::Quantity{23U});

  EXPECT_EQ(level.erase(*second.node), PriceLevelError::none);
  EXPECT_TRUE(level.validate_invariants());
  EXPECT_EQ(level.head(), first.node);
  EXPECT_EQ(first.node->next(), third.node);
  EXPECT_EQ(third.node->previous(), first.node);
  EXPECT_EQ(level.tail(), third.node);
  EXPECT_EQ(level.order_count(), 2U);
  EXPECT_EQ(level.aggregate_quantity(), domain::Quantity{16U});
  EXPECT_FALSE(second.node->is_linked());

  EXPECT_EQ(level.reduce_remaining(*first.node, domain::Quantity{2U}), PriceLevelError::none);
  EXPECT_TRUE(level.validate_invariants());
  EXPECT_EQ(first.node->remaining_quantity(), domain::Quantity{3U});
  EXPECT_EQ(first.node->priority_sequence(), domain::Sequence{1U});
  EXPECT_EQ(level.aggregate_quantity(), domain::Quantity{14U});

  EXPECT_EQ(level.erase(*first.node), PriceLevelError::none);
  EXPECT_TRUE(level.validate_invariants());
  EXPECT_EQ(level.head(), third.node);
  EXPECT_EQ(level.tail(), third.node);
  EXPECT_EQ(third.node->previous(), nullptr);
  EXPECT_EQ(level.aggregate_quantity(), domain::Quantity{11U});

  EXPECT_EQ(level.erase(*third.node), PriceLevelError::none);
  EXPECT_TRUE(level.validate_invariants());
  EXPECT_TRUE(level.empty());
  EXPECT_EQ(level.order_count(), 0U);
  EXPECT_EQ(level.aggregate_quantity(), domain::Quantity{});
  EXPECT_EQ(level.head(), nullptr);
  EXPECT_EQ(level.tail(), nullptr);

  EXPECT_EQ(storage.destroy(*first.node), StorageError::none);
  EXPECT_EQ(storage.destroy(*second.node), StorageError::none);
  EXPECT_EQ(storage.destroy(*third.node), StorageError::none);
}

TEST(PriceLevel, ErasesTheTailOfAMultiNodeLevelWithoutScanning) {
  HeapOrderStorage storage;
  PriceLevel level{domain::PriceTicks{10'250}};
  const auto first = storage.create(node_spec(1U, 5U, 1U));
  const auto second = storage.create(node_spec(2U, 7U, 2U));
  const auto third = storage.create(node_spec(3U, 11U, 3U));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(third);
  ASSERT_EQ(level.append(*first.node), PriceLevelError::none);
  ASSERT_EQ(level.append(*second.node), PriceLevelError::none);
  ASSERT_EQ(level.append(*third.node), PriceLevelError::none);

  EXPECT_EQ(level.erase(*third.node), PriceLevelError::none);
  EXPECT_TRUE(level.validate_invariants());
  EXPECT_EQ(level.tail(), second.node);
  EXPECT_EQ(second.node->next(), nullptr);
  EXPECT_EQ(level.order_count(), 2U);
  EXPECT_EQ(level.aggregate_quantity(), domain::Quantity{12U});
  EXPECT_FALSE(third.node->is_linked());

  EXPECT_EQ(level.erase(*first.node), PriceLevelError::none);
  EXPECT_EQ(level.erase(*second.node), PriceLevelError::none);
}

TEST(PriceLevel, RefusesToDestroyALinkedNode) {
  HeapOrderStorage storage;
  PriceLevel level{domain::PriceTicks{10'250}};
  const auto node = storage.create(node_spec(1U, 5U, 1U));
  ASSERT_TRUE(node);
  ASSERT_EQ(level.append(*node.node), PriceLevelError::none);

  EXPECT_EQ(storage.destroy(*node.node), StorageError::node_linked);
  EXPECT_EQ(storage.size(), 1U);
  EXPECT_EQ(level.head(), node.node);

  EXPECT_EQ(level.erase(*node.node), PriceLevelError::none);
  EXPECT_EQ(storage.destroy(*node.node), StorageError::none);
}

TEST(PriceLevel, AppendFailuresAreAtomic) {
  HeapOrderStorage storage;
  PriceLevel level{domain::PriceTicks{10'250}};
  const auto tail = storage.create(node_spec(1U, 5U, 2U));
  const auto wrong_price = storage.create(node_spec(2U, 7U, 3U, 10'251));
  const auto older = storage.create(node_spec(3U, 7U, 1U));
  const auto zero_remaining = storage.create(node_spec(4U, 7U, 4U));
  const auto zero_priority = storage.create(node_spec(5U, 7U, 5U));
  const auto equal_priority = storage.create(node_spec(6U, 7U, 2U));
  ASSERT_TRUE(tail);
  ASSERT_TRUE(wrong_price);
  ASSERT_TRUE(older);
  ASSERT_TRUE(zero_remaining);
  ASSERT_TRUE(zero_priority);
  ASSERT_TRUE(equal_priority);
  ASSERT_EQ(level.append(*tail.node), PriceLevelError::none);
  const auto tail_before = snapshot(*tail.node);

  auto level_before = snapshot(level);
  auto node_before = snapshot(*wrong_price.node);
  EXPECT_EQ(level.append(*wrong_price.node), PriceLevelError::price_mismatch);
  EXPECT_EQ(snapshot(level), level_before);
  EXPECT_EQ(snapshot(*tail.node), tail_before);
  EXPECT_EQ(snapshot(*wrong_price.node), node_before);

  level_before = snapshot(level);
  node_before = snapshot(*older.node);
  EXPECT_EQ(level.append(*older.node), PriceLevelError::nonmonotonic_priority);
  EXPECT_EQ(snapshot(level), level_before);
  EXPECT_EQ(snapshot(*tail.node), tail_before);
  EXPECT_EQ(snapshot(*older.node), node_before);

  level_before = snapshot(level);
  node_before = snapshot(*equal_priority.node);
  EXPECT_EQ(level.append(*equal_priority.node), PriceLevelError::nonmonotonic_priority);
  EXPECT_EQ(snapshot(level), level_before);
  EXPECT_EQ(snapshot(*tail.node), tail_before);
  EXPECT_EQ(snapshot(*equal_priority.node), node_before);

  core::test::CoreAccess::set_remaining(*zero_remaining.node, domain::Quantity{});
  level_before = snapshot(level);
  node_before = snapshot(*zero_remaining.node);
  EXPECT_EQ(level.append(*zero_remaining.node), PriceLevelError::invalid_remaining_quantity);
  EXPECT_EQ(snapshot(level), level_before);
  EXPECT_EQ(snapshot(*tail.node), tail_before);
  EXPECT_EQ(snapshot(*zero_remaining.node), node_before);

  core::test::CoreAccess::set_priority(*zero_priority.node, domain::Sequence{});
  level_before = snapshot(level);
  node_before = snapshot(*zero_priority.node);
  EXPECT_EQ(level.append(*zero_priority.node), PriceLevelError::invalid_priority_sequence);
  EXPECT_EQ(snapshot(level), level_before);
  EXPECT_EQ(snapshot(*tail.node), tail_before);
  EXPECT_EQ(snapshot(*zero_priority.node), node_before);

  EXPECT_EQ(level.erase(*tail.node), PriceLevelError::none);
}

TEST(PriceLevel, AlreadyLinkedAppendFailureIsAtomicForBothLevels) {
  HeapOrderStorage storage;
  PriceLevel first_level{domain::PriceTicks{10'250}};
  PriceLevel second_level{domain::PriceTicks{10'250}};
  const auto node = storage.create(node_spec(1U, 5U, 1U));
  ASSERT_TRUE(node);
  ASSERT_EQ(first_level.append(*node.node), PriceLevelError::none);
  const auto first_before = snapshot(first_level);
  const auto second_before = snapshot(second_level);
  const auto node_before = snapshot(*node.node);

  EXPECT_EQ(second_level.append(*node.node), PriceLevelError::node_already_linked);
  EXPECT_EQ(snapshot(first_level), first_before);
  EXPECT_EQ(snapshot(second_level), second_before);
  EXPECT_EQ(snapshot(*node.node), node_before);

  EXPECT_EQ(first_level.erase(*node.node), PriceLevelError::none);
}

TEST(PriceLevel, DetectsAggregateAndCountOverflowBeforeAppendMutation) {
  HeapOrderStorage storage;
  PriceLevel aggregate_level{domain::PriceTicks{10'250}};
  const auto maximum = storage.create(node_spec(1U, std::numeric_limits<std::uint64_t>::max(), 1U));
  const auto extra = storage.create(node_spec(2U, 1U, 2U));
  ASSERT_TRUE(maximum);
  ASSERT_TRUE(extra);
  ASSERT_EQ(aggregate_level.append(*maximum.node), PriceLevelError::none);
  const auto aggregate_before = snapshot(aggregate_level);
  const auto maximum_before = snapshot(*maximum.node);
  const auto extra_before = snapshot(*extra.node);

  EXPECT_EQ(aggregate_level.append(*extra.node), PriceLevelError::aggregate_overflow);
  EXPECT_EQ(snapshot(aggregate_level), aggregate_before);
  EXPECT_EQ(snapshot(*maximum.node), maximum_before);
  EXPECT_EQ(snapshot(*extra.node), extra_before);
  EXPECT_EQ(aggregate_level.erase(*maximum.node), PriceLevelError::none);

  PriceLevel count_level{domain::PriceTicks{10'250}};
  const auto first = storage.create(node_spec(3U, 1U, 3U));
  const auto second = storage.create(node_spec(4U, 1U, 4U));
  const auto third = storage.create(node_spec(5U, 1U, 5U));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(third);
  ASSERT_EQ(count_level.append(*first.node), PriceLevelError::none);
  ASSERT_EQ(count_level.append(*second.node), PriceLevelError::none);
  core::test::CoreAccess::set_level_count(count_level, std::numeric_limits<std::size_t>::max());
  const auto count_before = snapshot(count_level);
  const auto second_before = snapshot(*second.node);
  const auto third_before = snapshot(*third.node);

  EXPECT_EQ(count_level.append(*third.node), PriceLevelError::order_count_overflow);
  EXPECT_EQ(snapshot(count_level), count_before);
  EXPECT_EQ(snapshot(*second.node), second_before);
  EXPECT_EQ(snapshot(*third.node), third_before);

  core::test::CoreAccess::set_level_count(count_level, 2U);
  EXPECT_EQ(count_level.erase(*first.node), PriceLevelError::none);
  EXPECT_EQ(count_level.erase(*second.node), PriceLevelError::none);
}

TEST(PriceLevel, InvalidEraseAndReductionFailuresAreAtomic) {
  HeapOrderStorage storage;
  PriceLevel level{domain::PriceTicks{10'250}};
  const auto member = storage.create(node_spec(1U, 5U, 1U));
  const auto foreign = storage.create(node_spec(2U, 7U, 2U));
  ASSERT_TRUE(member);
  ASSERT_TRUE(foreign);
  ASSERT_EQ(level.append(*member.node), PriceLevelError::none);

  auto level_before = snapshot(level);
  auto member_before = snapshot(*member.node);
  auto foreign_before = snapshot(*foreign.node);
  EXPECT_EQ(level.erase(*foreign.node), PriceLevelError::node_not_in_level);
  EXPECT_EQ(snapshot(level), level_before);
  EXPECT_EQ(snapshot(*member.node), member_before);
  EXPECT_EQ(snapshot(*foreign.node), foreign_before);

  for (const auto reduction : {domain::Quantity{}, domain::Quantity{5U}, domain::Quantity{6U}}) {
    level_before = snapshot(level);
    member_before = snapshot(*member.node);
    EXPECT_EQ(level.reduce_remaining(*member.node, reduction), PriceLevelError::invalid_reduction);
    EXPECT_EQ(snapshot(level), level_before);
    EXPECT_EQ(snapshot(*member.node), member_before);
  }

  level_before = snapshot(level);
  foreign_before = snapshot(*foreign.node);
  EXPECT_EQ(level.reduce_remaining(*foreign.node, domain::Quantity{1U}),
            PriceLevelError::node_not_in_level);
  EXPECT_EQ(snapshot(level), level_before);
  EXPECT_EQ(snapshot(*foreign.node), foreign_before);

  core::test::CoreAccess::set_level_aggregate(level, domain::Quantity{1U});
  level_before = snapshot(level);
  member_before = snapshot(*member.node);
  EXPECT_EQ(level.erase(*member.node), PriceLevelError::aggregate_underflow);
  EXPECT_EQ(snapshot(level), level_before);
  EXPECT_EQ(snapshot(*member.node), member_before);

  EXPECT_EQ(level.reduce_remaining(*member.node, domain::Quantity{2U}),
            PriceLevelError::aggregate_underflow);
  EXPECT_EQ(snapshot(level), level_before);
  EXPECT_EQ(snapshot(*member.node), member_before);

  core::test::CoreAccess::set_level_aggregate(level, domain::Quantity{5U});
  EXPECT_EQ(level.erase(*member.node), PriceLevelError::none);
}

TEST(PriceLevel, InvalidAndCorruptBoundaryFailuresAreAtomic) {
  HeapOrderStorage storage;
  PriceLevel invalid_price{domain::PriceTicks{}};
  PriceLevel valid_level{domain::PriceTicks{10'250}};
  const auto node = storage.create(node_spec(1U, 5U, 1U));
  ASSERT_TRUE(node);
  const auto invalid_before = snapshot(invalid_price);
  const auto node_before = snapshot(*node.node);

  EXPECT_EQ(invalid_price.append(*node.node), PriceLevelError::invalid_level_price);
  EXPECT_EQ(snapshot(invalid_price), invalid_before);
  EXPECT_EQ(snapshot(*node.node), node_before);

  core::test::CoreAccess::set_level_aggregate(valid_level, domain::Quantity{1U});
  const auto corrupt_before = snapshot(valid_level);
  EXPECT_EQ(valid_level.append(*node.node), PriceLevelError::level_invariant_violation);
  EXPECT_EQ(snapshot(valid_level), corrupt_before);
  EXPECT_EQ(snapshot(*node.node), node_before);
  core::test::CoreAccess::set_level_aggregate(valid_level, {});
}

class PriceLevelInvariantTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto first_result = storage_.create(node_spec(1U, 5U, 1U));
    const auto second_result = storage_.create(node_spec(2U, 7U, 2U));
    const auto third_result = storage_.create(node_spec(3U, 11U, 3U));
    ASSERT_TRUE(first_result);
    ASSERT_TRUE(second_result);
    ASSERT_TRUE(third_result);
    first_ = first_result.node;
    second_ = second_result.node;
    third_ = third_result.node;
    ASSERT_EQ(level_.append(*first_), PriceLevelError::none);
    ASSERT_EQ(level_.append(*second_), PriceLevelError::none);
    ASSERT_EQ(level_.append(*third_), PriceLevelError::none);
    ASSERT_TRUE(level_.validate_invariants());
  }

  void TearDown() override {
    core::test::CoreAccess::force_clear(level_, {first_, second_, third_, extra_});
  }

  HeapOrderStorage storage_;
  PriceLevel level_{domain::PriceTicks{10'250}};
  OrderNode* first_{};
  OrderNode* second_{};
  OrderNode* third_{};
  OrderNode* extra_{};
};

TEST_F(PriceLevelInvariantTest, DetectsBrokenReciprocalLinks) {
  core::test::CoreAccess::set_previous(*second_, nullptr);

  EXPECT_EQ(level_.validate_invariants().error, PriceLevelInvariantError::broken_next_link);
}

TEST_F(PriceLevelInvariantTest, DetectsForwardAndBackwardCyclesSafely) {
  core::test::CoreAccess::set_next(*second_, first_);
  EXPECT_EQ(level_.validate_invariants().error, PriceLevelInvariantError::forward_cycle);

  core::test::CoreAccess::set_next(*second_, third_);
  core::test::CoreAccess::set_previous(*second_, third_);
  EXPECT_EQ(level_.validate_invariants().error, PriceLevelInvariantError::backward_cycle);
}

TEST_F(PriceLevelInvariantTest, DetectsWrongBacklinkPriceAndZeroQuantity) {
  core::test::CoreAccess::set_price_level(*second_, nullptr);
  EXPECT_EQ(level_.validate_invariants().error, PriceLevelInvariantError::wrong_backlink);
  core::test::CoreAccess::set_price_level(*second_, &level_);

  core::test::CoreAccess::set_price(*second_, domain::PriceTicks{10'251});
  EXPECT_EQ(level_.validate_invariants().error, PriceLevelInvariantError::wrong_price);
  core::test::CoreAccess::set_price(*second_, domain::PriceTicks{10'250});

  core::test::CoreAccess::set_remaining(*second_, {});
  EXPECT_EQ(level_.validate_invariants().error, PriceLevelInvariantError::zero_remaining_quantity);
}

TEST_F(PriceLevelInvariantTest, DetectsInvalidAndNonmonotonicPriority) {
  core::test::CoreAccess::set_priority(*second_, {});
  EXPECT_EQ(level_.validate_invariants().error, PriceLevelInvariantError::zero_priority_sequence);

  core::test::CoreAccess::set_priority(*second_, domain::Sequence{1U});
  EXPECT_EQ(level_.validate_invariants().error, PriceLevelInvariantError::nonmonotonic_priority);
}

TEST_F(PriceLevelInvariantTest, DetectsWrongAggregateAndCount) {
  core::test::CoreAccess::set_level_aggregate(level_, domain::Quantity{24U});
  EXPECT_EQ(level_.validate_invariants().error, PriceLevelInvariantError::aggregate_mismatch);
  core::test::CoreAccess::set_level_aggregate(level_, domain::Quantity{23U});

  core::test::CoreAccess::set_level_count(level_, 4U);
  EXPECT_EQ(level_.validate_invariants().error, PriceLevelInvariantError::count_mismatch);
}

TEST_F(PriceLevelInvariantTest, DetectsBoundaryAndTailMismatches) {
  core::test::CoreAccess::set_previous(*first_, third_);
  EXPECT_EQ(level_.validate_invariants().error, PriceLevelInvariantError::head_has_previous);
  core::test::CoreAccess::set_previous(*first_, nullptr);

  core::test::CoreAccess::set_next(*third_, first_);
  EXPECT_EQ(level_.validate_invariants().error, PriceLevelInvariantError::tail_has_next);
  core::test::CoreAccess::set_next(*third_, nullptr);

  const auto extra_result = storage_.create(node_spec(4U, 13U, 4U));
  ASSERT_TRUE(extra_result);
  extra_ = extra_result.node;
  core::test::CoreAccess::set_level_tail(level_, extra_);
  EXPECT_EQ(level_.validate_invariants().error, PriceLevelInvariantError::forward_tail_mismatch);
}

TEST_F(PriceLevelInvariantTest, DetectsAggregateOverflowDuringTraversal) {
  core::test::CoreAccess::set_remaining(
      *first_, domain::Quantity{std::numeric_limits<std::uint64_t>::max()});

  EXPECT_EQ(level_.validate_invariants().error, PriceLevelInvariantError::aggregate_overflow);
}

TEST(PriceLevelInvariants, DetectsInvalidAndCorruptEmptyLevels) {
  HeapOrderStorage storage;
  PriceLevel invalid_price{domain::PriceTicks{}};
  PriceLevel level{domain::PriceTicks{10'250}};
  const auto node = storage.create(node_spec(1U, 5U, 1U));
  ASSERT_TRUE(node);

  EXPECT_EQ(invalid_price.validate_invariants().error,
            PriceLevelInvariantError::invalid_level_price);

  core::test::CoreAccess::set_level_head(level, node.node);
  EXPECT_EQ(level.validate_invariants().error, PriceLevelInvariantError::empty_has_head);
  core::test::CoreAccess::set_level_head(level, nullptr);

  core::test::CoreAccess::set_level_tail(level, node.node);
  EXPECT_EQ(level.validate_invariants().error, PriceLevelInvariantError::empty_has_tail);
  core::test::CoreAccess::set_level_tail(level, nullptr);

  core::test::CoreAccess::set_level_aggregate(level, domain::Quantity{1U});
  EXPECT_EQ(level.validate_invariants().error, PriceLevelInvariantError::empty_has_aggregate);
  core::test::CoreAccess::set_level_aggregate(level, {});
}

TEST(PriceLevelErrorVocabulary, HasStableStringRepresentations) {
  EXPECT_EQ(to_string(PriceLevelError::aggregate_overflow), "aggregate_overflow");
  EXPECT_EQ(to_string(PriceLevelInvariantError::forward_cycle), "forward_cycle");
  EXPECT_EQ(to_string(static_cast<PriceLevelError>(255U)), "unknown");
  EXPECT_EQ(to_string(static_cast<PriceLevelInvariantError>(255U)), "unknown");
}

}  // namespace
