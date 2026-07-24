#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "active_order_index.hpp"
#include "core_test_access.hpp"
#include "order_storage.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::core;

static_assert(!std::is_copy_constructible_v<ActiveOrderIndex>);
static_assert(!std::is_copy_assignable_v<ActiveOrderIndex>);
static_assert(!std::is_move_constructible_v<ActiveOrderIndex>);
static_assert(!std::is_move_assignable_v<ActiveOrderIndex>);

OrderNodeSpec valid_spec(std::uint64_t order_id = 1U, std::uint64_t priority = 1U) {
  return {
      .order_id = domain::OrderId{order_id},
      .client_id = domain::ClientId{7U},
      .instrument_id = domain::InstrumentId{11U},
      .side = domain::Side::buy,
      .price = domain::PriceTicks{10'250},
      .remaining_quantity = domain::Quantity{25U},
      .priority_sequence = domain::Sequence{priority},
  };
}

TEST(ActiveOrderIndex, InsertsAndFindsPreparedUnlinkedNodes) {
  HeapOrderStorage storage;
  const auto created = storage.create(valid_spec());
  ASSERT_TRUE(created);
  ASSERT_FALSE(created.node->is_linked());
  ActiveOrderIndex index;

  EXPECT_EQ(index.insert(*created.node), ActiveOrderIndexError::none);
  EXPECT_FALSE(index.empty());
  EXPECT_EQ(index.size(), 1U);
  EXPECT_EQ(index.find(domain::OrderId{1U}), created.node);
  EXPECT_EQ(index.find(domain::OrderId{}), nullptr);
  EXPECT_EQ(index.find(domain::OrderId{99U}), nullptr);

  const auto& const_index = index;
  static_assert(std::is_same_v<decltype(const_index.find(domain::OrderId{1U})), const OrderNode*>);
  EXPECT_EQ(const_index.find(domain::OrderId{1U}), created.node);
  EXPECT_EQ(const_index.any_order(), created.node);
  EXPECT_TRUE(index.validate_invariants());
}

TEST(ActiveOrderIndex, RejectsInvalidAndDuplicateIdsWithoutMutation) {
  HeapOrderStorage storage;
  const auto created = storage.create(valid_spec());
  ASSERT_TRUE(created);
  ActiveOrderIndex index;

  core::test::CoreAccess::set_order_id(*created.node, {});
  EXPECT_EQ(index.insert(*created.node), ActiveOrderIndexError::invalid_order_id);
  EXPECT_EQ(index.size(), 0U);
  core::test::CoreAccess::set_order_id(*created.node, domain::OrderId{1U});

  ASSERT_EQ(index.insert(*created.node), ActiveOrderIndexError::none);
  EXPECT_EQ(index.insert(*created.node), ActiveOrderIndexError::duplicate_order_id);
  EXPECT_EQ(index.size(), 1U);
  EXPECT_EQ(index.find(domain::OrderId{1U}), created.node);
}

TEST(ActiveOrderIndex, ErasesOnlyTheExactIndexedPointer) {
  HeapOrderStorage indexed_storage;
  HeapOrderStorage foreign_storage;
  const auto indexed = indexed_storage.create(valid_spec());
  const auto foreign_same_id = foreign_storage.create(valid_spec());
  const auto unknown = foreign_storage.create(valid_spec(2U, 2U));
  ASSERT_TRUE(indexed);
  ASSERT_TRUE(foreign_same_id);
  ASSERT_TRUE(unknown);
  ActiveOrderIndex index;
  ASSERT_EQ(index.insert(*indexed.node), ActiveOrderIndexError::none);

  EXPECT_EQ(index.erase(*foreign_same_id.node), ActiveOrderIndexError::pointer_mismatch);
  EXPECT_EQ(index.erase(*unknown.node), ActiveOrderIndexError::unknown_order_id);
  EXPECT_EQ(index.size(), 1U);
  EXPECT_EQ(index.find(domain::OrderId{1U}), indexed.node);

  EXPECT_EQ(index.erase(*indexed.node), ActiveOrderIndexError::none);
  EXPECT_TRUE(index.empty());
  EXPECT_EQ(index.any_order(), nullptr);
}

TEST(ActiveOrderIndex, RejectsEraseWithInvalidIdWithoutMutation) {
  HeapOrderStorage storage;
  const auto created = storage.create(valid_spec());
  ASSERT_TRUE(created);
  ActiveOrderIndex index;

  core::test::CoreAccess::set_order_id(*created.node, {});
  EXPECT_EQ(index.erase(*created.node), ActiveOrderIndexError::invalid_order_id);
  EXPECT_TRUE(index.empty());
  core::test::CoreAccess::set_order_id(*created.node, domain::OrderId{1U});
}

TEST(ActiveOrderIndex, PreservesIndexedNodeAddressesAcrossRehash) {
  HeapOrderStorage storage;
  ActiveOrderIndex index;
  std::vector<const OrderNode*> addresses;
  addresses.reserve(4'096U);

  for (std::uint64_t id = 1U; id <= 4'096U; ++id) {
    const auto created = storage.create(valid_spec(id, id));
    ASSERT_TRUE(created) << "id=" << id;
    ASSERT_EQ(index.insert(*created.node), ActiveOrderIndexError::none) << "id=" << id;
    addresses.push_back(created.node);
  }

  const auto buckets_before = core::test::CoreAccess::index_bucket_count(index);
  core::test::CoreAccess::force_index_rehash(index, buckets_before * 2U + 1U);
  EXPECT_GT(core::test::CoreAccess::index_bucket_count(index), buckets_before);
  ASSERT_EQ(index.size(), addresses.size());
  for (std::size_t offset = 0; offset < addresses.size(); ++offset) {
    const auto order_id = domain::OrderId{static_cast<std::uint64_t>(offset + 1U)};
    EXPECT_EQ(index.find(order_id), addresses[offset]);
  }
  EXPECT_TRUE(index.validate_invariants());
}

TEST(ActiveOrderIndex, IteratesEveryEntryAndOffersOneForSafeDraining) {
  HeapOrderStorage storage;
  ActiveOrderIndex index;
  std::unordered_map<std::uint64_t, const OrderNode*> expected;
  for (std::uint64_t id = 1U; id <= 8U; ++id) {
    const auto created = storage.create(valid_spec(id, id));
    ASSERT_TRUE(created);
    ASSERT_EQ(index.insert(*created.node), ActiveOrderIndexError::none);
    expected.emplace(id, created.node);
  }

  std::unordered_set<std::uint64_t> visited;
  index.for_each([&](domain::OrderId order_id, const OrderNode* node) {
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(expected.at(order_id.value()), node);
    visited.insert(order_id.value());
  });

  EXPECT_EQ(visited.size(), expected.size());
  auto* const selected = index.any_order();
  ASSERT_NE(selected, nullptr);
  EXPECT_EQ(index.find(selected->order_id()), selected);
}

TEST(ActiveOrderIndexInvariants, DetectsZeroKeyNullNodeAndKeyMismatch) {
  HeapOrderStorage storage;
  const auto created = storage.create(valid_spec());
  ASSERT_TRUE(created);

  ActiveOrderIndex zero_key;
  ASSERT_TRUE(core::test::CoreAccess::insert_index_entry(zero_key, {}, created.node));
  auto result = zero_key.validate_invariants();
  EXPECT_EQ(result.error, ActiveOrderIndexInvariantError::invalid_order_id);
  EXPECT_EQ(result.order_id, domain::OrderId{});
  EXPECT_EQ(result.node, created.node);

  ActiveOrderIndex null_node;
  ASSERT_TRUE(core::test::CoreAccess::insert_index_entry(null_node, domain::OrderId{1U}, nullptr));
  result = null_node.validate_invariants();
  EXPECT_EQ(result.error, ActiveOrderIndexInvariantError::null_node);
  EXPECT_EQ(result.order_id, domain::OrderId{1U});
  EXPECT_EQ(result.node, nullptr);
  EXPECT_EQ(null_node.find(domain::OrderId{1U}), nullptr);

  ActiveOrderIndex mismatched_key;
  ASSERT_TRUE(core::test::CoreAccess::insert_index_entry(mismatched_key, domain::OrderId{2U},
                                                         created.node));
  result = mismatched_key.validate_invariants();
  EXPECT_EQ(result.error, ActiveOrderIndexInvariantError::key_order_id_mismatch);
  EXPECT_EQ(result.order_id, domain::OrderId{2U});
  EXPECT_EQ(result.node, created.node);
  EXPECT_EQ(mismatched_key.find(domain::OrderId{2U}), nullptr);
}

TEST(ActiveOrderIndexInvariants, CorruptDuplicateSlotFailsAtomically) {
  HeapOrderStorage storage;
  const auto created = storage.create(valid_spec());
  ASSERT_TRUE(created);
  ActiveOrderIndex index;
  ASSERT_TRUE(core::test::CoreAccess::insert_index_entry(index, domain::OrderId{1U}, nullptr));

  EXPECT_EQ(index.insert(*created.node), ActiveOrderIndexError::index_invariant_violation);
  EXPECT_EQ(index.erase(*created.node), ActiveOrderIndexError::index_invariant_violation);
  EXPECT_EQ(index.size(), 1U);
  EXPECT_EQ(index.find(domain::OrderId{1U}), nullptr);
}

TEST(ActiveOrderIndexErrorVocabulary, HasStableStringRepresentations) {
  EXPECT_EQ(to_string(ActiveOrderIndexError::duplicate_order_id), "duplicate_order_id");
  EXPECT_EQ(to_string(ActiveOrderIndexError::pointer_mismatch), "pointer_mismatch");
  EXPECT_EQ(to_string(ActiveOrderIndexInvariantError::null_node), "null_node");
  EXPECT_EQ(to_string(ActiveOrderIndexInvariantError::key_order_id_mismatch),
            "key_order_id_mismatch");
  EXPECT_EQ(to_string(static_cast<ActiveOrderIndexError>(255U)), "unknown");
  EXPECT_EQ(to_string(static_cast<ActiveOrderIndexInvariantError>(255U)), "unknown");
}

}  // namespace
