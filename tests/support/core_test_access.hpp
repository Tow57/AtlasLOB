#pragma once

#if !defined(ATLAS_ENABLE_TEST_ACCESS) || !ATLAS_ENABLE_TEST_ACCESS
#error "CoreAccess is available only through the test-only atlas_core_testable target"
#endif

#include <cstddef>
#include <initializer_list>

#include "order_storage.hpp"
#include "price_level.hpp"

namespace atlaslob::core::test {

class CoreAccess final {
 public:
  [[nodiscard]] static std::size_t storage_bucket_count(const HeapOrderStorage& storage) noexcept {
    return storage.orders_.bucket_count();
  }

  static void force_storage_rehash(HeapOrderStorage& storage, std::size_t minimum_bucket_count) {
    storage.orders_.rehash(minimum_bucket_count);
  }

  static void set_previous(OrderNode& node, OrderNode* previous) noexcept {
    node.previous_ = previous;
  }

  static void set_next(OrderNode& node, OrderNode* next) noexcept { node.next_ = next; }

  static void set_price_level(OrderNode& node, PriceLevel* level) noexcept {
    node.price_level_ = level;
  }

  static void set_price(OrderNode& node, domain::PriceTicks price) noexcept { node.price_ = price; }

  static void set_remaining(OrderNode& node, domain::Quantity remaining) noexcept {
    node.remaining_quantity_ = remaining;
  }

  static void set_priority(OrderNode& node, domain::Sequence priority) noexcept {
    node.priority_sequence_ = priority;
  }

  static void set_level_aggregate(PriceLevel& level, domain::Quantity aggregate) noexcept {
    level.aggregate_quantity_ = aggregate;
  }

  static void set_level_count(PriceLevel& level, std::size_t count) noexcept {
    level.order_count_ = count;
  }

  static void set_level_head(PriceLevel& level, OrderNode* head) noexcept { level.head_ = head; }

  static void set_level_tail(PriceLevel& level, OrderNode* tail) noexcept { level.tail_ = tail; }

  static void force_clear(PriceLevel& level, std::initializer_list<OrderNode*> nodes) noexcept {
    for (auto* node : nodes) {
      if (node != nullptr) {
        node->previous_ = nullptr;
        node->next_ = nullptr;
        node->price_level_ = nullptr;
      }
    }
    level.aggregate_quantity_ = {};
    level.order_count_ = 0U;
    level.head_ = nullptr;
    level.tail_ = nullptr;
  }
};

}  // namespace atlaslob::core::test
