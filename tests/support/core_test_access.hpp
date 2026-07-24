#pragma once

#if !defined(ATLAS_ENABLE_TEST_ACCESS) || !ATLAS_ENABLE_TEST_ACCESS
#error "CoreAccess is available only through the test-only atlas_core_testable target"
#endif

#include <cstddef>
#include <initializer_list>
#include <utility>

#include "active_order_index.hpp"
#include "book_side.hpp"
#include "instrument_book.hpp"
#include "order_storage.hpp"
#include "price_level.hpp"

namespace atlaslob::core::test {

class CoreAccess final {
 public:
  [[nodiscard]] static bool insert_index_entry(ActiveOrderIndex& index, domain::OrderId order_id,
                                               OrderNode* node) {
    return index.orders_.try_emplace(order_id, node).second;
  }

  [[nodiscard]] static bool erase_index_entry(ActiveOrderIndex& index,
                                              domain::OrderId order_id) noexcept {
    return index.orders_.erase(order_id) == 1U;
  }

  [[nodiscard]] static std::size_t index_bucket_count(const ActiveOrderIndex& index) noexcept {
    return index.orders_.bucket_count();
  }

  static void force_index_rehash(ActiveOrderIndex& index, std::size_t minimum_bucket_count) {
    index.orders_.rehash(minimum_bucket_count);
  }

  [[nodiscard]] static ActiveOrderIndex& active_index(InstrumentBook& book) noexcept {
    return book.index_;
  }

  [[nodiscard]] static HeapOrderStorage& storage(InstrumentBook& book) noexcept {
    return book.storage_;
  }

  [[nodiscard]] static BidBookSide& bids(InstrumentBook& book) noexcept { return book.bids_; }

  [[nodiscard]] static AskBookSide& asks(InstrumentBook& book) noexcept { return book.asks_; }

  static void set_book_instrument_id(InstrumentBook& book,
                                     domain::InstrumentId instrument_id) noexcept {
    book.instrument_id_ = instrument_id;
  }

  [[nodiscard]] static OrderNode* replace_index_entry(ActiveOrderIndex& index,
                                                      domain::OrderId order_id,
                                                      OrderNode* replacement) noexcept {
    const auto position = index.orders_.find(order_id);
    if (position == index.orders_.end()) {
      return nullptr;
    }
    return std::exchange(position->second, replacement);
  }

  template <domain::Side RestingSide>
  [[nodiscard]] static bool insert_null_level(BookSide<RestingSide>& side,
                                              domain::PriceTicks price) {
    return side.levels_.try_emplace(price, nullptr).second;
  }

  template <domain::Side RestingSide>
  [[nodiscard]] static bool erase_level_entry(BookSide<RestingSide>& side,
                                              domain::PriceTicks price) noexcept {
    return side.levels_.erase(price) == 1U;
  }

  [[nodiscard]] static std::size_t storage_bucket_count(const HeapOrderStorage& storage) noexcept {
    return storage.orders_.bucket_count();
  }

  static void force_storage_rehash(HeapOrderStorage& storage, std::size_t minimum_bucket_count) {
    storage.orders_.rehash(minimum_bucket_count);
  }

  static void set_previous(OrderNode& node, OrderNode* previous) noexcept {
    node.previous_ = previous;
  }

  static void set_order_id(OrderNode& node, domain::OrderId order_id) noexcept {
    node.order_id_ = order_id;
  }

  static void set_instrument_id(OrderNode& node, domain::InstrumentId instrument_id) noexcept {
    node.instrument_id_ = instrument_id;
  }

  static void set_client_id(OrderNode& node, domain::ClientId client_id) noexcept {
    node.client_id_ = client_id;
  }

  static void set_side(OrderNode& node, domain::Side side) noexcept { node.side_ = side; }

  static void set_next(OrderNode& node, OrderNode* next) noexcept { node.next_ = next; }

  static void set_price_level(OrderNode& node, PriceLevel* level) noexcept {
    node.price_level_ = level;
  }

  [[nodiscard]] static PriceLevel* mutable_price_level(OrderNode& node) noexcept {
    return node.price_level_;
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

  static void set_level_price(PriceLevel& level, domain::PriceTicks price) noexcept {
    level.price_ = price;
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

  template <domain::Side RestingSide>
  [[nodiscard]] static bool reprice_level(BookSide<RestingSide>& side, domain::PriceTicks old_price,
                                          domain::PriceTicks new_price) {
    auto entry = side.levels_.extract(old_price);
    if (entry.empty() || side.levels_.contains(new_price)) {
      if (!entry.empty()) {
        side.levels_.insert(std::move(entry));
      }
      return false;
    }

    entry.key() = new_price;
    auto* const level = entry.mapped().get();
    level->price_ = new_price;
    for (auto* node = level->head_; node != nullptr; node = node->next_) {
      node->price_ = new_price;
    }
    side.levels_.insert(std::move(entry));
    return true;
  }
};

}  // namespace atlaslob::core::test
