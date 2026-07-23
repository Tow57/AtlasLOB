#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "core_test_access.hpp"
#include "order_storage.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::core;

static_assert(std::is_abstract_v<OrderStorage>);
static_assert(!std::is_copy_constructible_v<OrderNode>);
static_assert(!std::is_copy_assignable_v<OrderNode>);
static_assert(!std::is_move_constructible_v<OrderNode>);
static_assert(!std::is_move_assignable_v<OrderNode>);
static_assert(!std::is_destructible_v<OrderNode>);
static_assert(!std::is_default_constructible_v<OrderNodeDeleter>);
static_assert(!std::is_trivially_copyable_v<OrderNodeDeleter>);
static_assert(!std::is_constructible_v<std::unique_ptr<OrderNode, OrderNodeDeleter>, OrderNode*>);
static_assert(!std::is_copy_constructible_v<HeapOrderStorage>);
static_assert(!std::is_copy_assignable_v<HeapOrderStorage>);
static_assert(!std::is_move_constructible_v<HeapOrderStorage>);
static_assert(!std::is_move_assignable_v<HeapOrderStorage>);

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

TEST(HeapOrderStorage, CreatesInitializedStableNodes) {
  HeapOrderStorage storage;
  const auto result = storage.create(valid_spec());

  ASSERT_TRUE(result);
  ASSERT_NE(result.node, nullptr);
  EXPECT_EQ(result.error, StorageError::none);
  EXPECT_EQ(storage.size(), 1U);
  EXPECT_EQ(result.node->order_id(), domain::OrderId{1U});
  EXPECT_EQ(result.node->client_id(), domain::ClientId{7U});
  EXPECT_EQ(result.node->instrument_id(), domain::InstrumentId{11U});
  EXPECT_EQ(result.node->side(), domain::Side::buy);
  EXPECT_EQ(result.node->price(), domain::PriceTicks{10'250});
  EXPECT_EQ(result.node->remaining_quantity(), domain::Quantity{25U});
  EXPECT_EQ(result.node->priority_sequence(), domain::Sequence{1U});
  EXPECT_EQ(result.node->previous(), nullptr);
  EXPECT_EQ(result.node->next(), nullptr);
  EXPECT_EQ(result.node->price_level(), nullptr);
  EXPECT_FALSE(result.node->is_linked());
}

TEST(HeapOrderStorage, KeepsDistinctAddressesStableAcrossRehash) {
  HeapOrderStorage storage;
  auto first_result = storage.create(valid_spec());
  ASSERT_TRUE(first_result);
  auto* const first = first_result.node;
  std::unordered_set<const OrderNode*> addresses;
  std::vector<const OrderNode*> saved_addresses;
  addresses.insert(first);
  saved_addresses.push_back(first);

  for (std::uint64_t id = 2U; id <= 4'096U; ++id) {
    const auto result = storage.create(valid_spec(id, id));
    ASSERT_TRUE(result) << "id=" << id;
    ASSERT_TRUE(addresses.insert(result.node).second) << "id=" << id;
    saved_addresses.push_back(result.node);
  }

  const auto bucket_count_before_rehash = core::test::CoreAccess::storage_bucket_count(storage);
  core::test::CoreAccess::force_storage_rehash(storage, bucket_count_before_rehash * 2U + 1U);
  EXPECT_GT(core::test::CoreAccess::storage_bucket_count(storage), bucket_count_before_rehash);
  EXPECT_EQ(storage.size(), 4'096U);
  for (std::size_t index = 0; index < saved_addresses.size(); ++index) {
    const auto expected = static_cast<std::uint64_t>(index + 1U);
    EXPECT_EQ(saved_addresses[index]->order_id(), domain::OrderId{expected});
    EXPECT_EQ(saved_addresses[index]->priority_sequence(), domain::Sequence{expected});
  }
}

TEST(HeapOrderStorage, RejectsDuplicateCreationWithoutMutation) {
  HeapOrderStorage storage;
  const auto original = storage.create(valid_spec());
  ASSERT_TRUE(original);
  auto duplicate_spec = valid_spec();
  duplicate_spec.remaining_quantity = domain::Quantity{999U};

  const auto duplicate = storage.create(duplicate_spec);

  EXPECT_FALSE(duplicate);
  EXPECT_EQ(duplicate.node, nullptr);
  EXPECT_EQ(duplicate.error, StorageError::duplicate_order_id);
  EXPECT_EQ(storage.size(), 1U);
  EXPECT_EQ(original.node->remaining_quantity(), domain::Quantity{25U});
}

TEST(HeapOrderStorage, RejectsInvalidSpecificationsWithoutMutation) {
  HeapOrderStorage storage;
  auto spec = valid_spec();

  spec.order_id = {};
  EXPECT_EQ(storage.create(spec).error, StorageError::invalid_order_id);
  spec = valid_spec();
  spec.client_id = {};
  EXPECT_EQ(storage.create(spec).error, StorageError::invalid_client_id);
  spec = valid_spec();
  spec.instrument_id = {};
  EXPECT_EQ(storage.create(spec).error, StorageError::invalid_instrument_id);
  spec = valid_spec();
  spec.side = static_cast<domain::Side>(0U);
  EXPECT_EQ(storage.create(spec).error, StorageError::invalid_side);
  spec = valid_spec();
  spec.side = static_cast<domain::Side>(255U);
  EXPECT_EQ(storage.create(spec).error, StorageError::invalid_side);
  spec = valid_spec();
  spec.price = {};
  EXPECT_EQ(storage.create(spec).error, StorageError::invalid_price);
  spec = valid_spec();
  spec.price = domain::PriceTicks{-1};
  EXPECT_EQ(storage.create(spec).error, StorageError::invalid_price);
  spec = valid_spec();
  spec.remaining_quantity = {};
  EXPECT_EQ(storage.create(spec).error, StorageError::invalid_remaining_quantity);
  spec = valid_spec();
  spec.priority_sequence = {};
  EXPECT_EQ(storage.create(spec).error, StorageError::invalid_priority_sequence);

  EXPECT_EQ(storage.size(), 0U);
}

TEST(HeapOrderStorage, IncorrectDestroyDoesNotMutateEitherStorage) {
  HeapOrderStorage first;
  HeapOrderStorage second;
  const auto first_node = first.create(valid_spec());
  const auto second_node = second.create(valid_spec());
  ASSERT_TRUE(first_node);
  ASSERT_TRUE(second_node);
  ASSERT_NE(first_node.node, second_node.node);

  EXPECT_EQ(first.destroy(*second_node.node), StorageError::not_owned);
  EXPECT_EQ(first.destroy(nullptr), StorageError::not_owned);
  EXPECT_EQ(first.size(), 1U);
  EXPECT_EQ(second.size(), 1U);
  EXPECT_EQ(first_node.node->order_id(), domain::OrderId{1U});
  EXPECT_EQ(second_node.node->order_id(), domain::OrderId{1U});
}

TEST(HeapOrderStorage, DestroysUnlinkedNodesAndAllowsTerminalIdReuse) {
  HeapOrderStorage storage;
  const auto first = storage.create(valid_spec());
  ASSERT_TRUE(first);

  EXPECT_EQ(storage.destroy(*first.node), StorageError::none);
  EXPECT_EQ(storage.size(), 0U);

  const auto reused = storage.create(valid_spec());
  EXPECT_TRUE(reused);
  EXPECT_EQ(storage.size(), 1U);
}

TEST(StorageErrorVocabulary, HasStableStringRepresentations) {
  EXPECT_EQ(to_string(StorageError::duplicate_order_id), "duplicate_order_id");
  EXPECT_EQ(to_string(StorageError::node_linked), "node_linked");
  EXPECT_EQ(to_string(static_cast<StorageError>(255U)), "unknown");
}

}  // namespace
