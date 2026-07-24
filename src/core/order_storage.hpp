#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>

#include "order_node.hpp"

namespace atlaslob::core {

enum class StorageError : std::uint8_t {
  none = 0,
  invalid_order_id = 1,
  invalid_client_id = 2,
  invalid_instrument_id = 3,
  invalid_side = 4,
  invalid_price = 5,
  invalid_remaining_quantity = 6,
  invalid_priority_sequence = 7,
  duplicate_order_id = 8,
  not_owned = 9,
  node_linked = 10,
};

[[nodiscard]] constexpr std::string_view to_string(StorageError error) noexcept {
  switch (error) {
    case StorageError::none:
      return "none";
    case StorageError::invalid_order_id:
      return "invalid_order_id";
    case StorageError::invalid_client_id:
      return "invalid_client_id";
    case StorageError::invalid_instrument_id:
      return "invalid_instrument_id";
    case StorageError::invalid_side:
      return "invalid_side";
    case StorageError::invalid_price:
      return "invalid_price";
    case StorageError::invalid_remaining_quantity:
      return "invalid_remaining_quantity";
    case StorageError::invalid_priority_sequence:
      return "invalid_priority_sequence";
    case StorageError::duplicate_order_id:
      return "duplicate_order_id";
    case StorageError::not_owned:
      return "not_owned";
    case StorageError::node_linked:
      return "node_linked";
  }
  return "unknown";
}

struct CreateOrderResult final {
  OrderNode* node{};
  StorageError error{StorageError::none};

  [[nodiscard]] constexpr bool has_value() const noexcept {
    return node != nullptr && error == StorageError::none;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }
};

class OrderNodeDeleter final {
 public:
  OrderNodeDeleter(const OrderNodeDeleter&) noexcept = default;
  OrderNodeDeleter& operator=(const OrderNodeDeleter&) noexcept = default;
  OrderNodeDeleter(OrderNodeDeleter&&) noexcept = default;
  OrderNodeDeleter& operator=(OrderNodeDeleter&&) noexcept = default;
  ~OrderNodeDeleter() noexcept {}

  void operator()(OrderNode* node) const noexcept { delete node; }

 private:
  friend class HeapOrderStorage;

  OrderNodeDeleter() noexcept = default;
};

class OrderStorage {
 public:
  OrderStorage(const OrderStorage&) = delete;
  OrderStorage& operator=(const OrderStorage&) = delete;
  OrderStorage(OrderStorage&&) = delete;
  OrderStorage& operator=(OrderStorage&&) = delete;
  virtual ~OrderStorage() = default;

  [[nodiscard]] virtual CreateOrderResult create(const OrderNodeSpec& spec) = 0;
  [[nodiscard]] virtual StorageError destroy(OrderNode& node) noexcept = 0;
  [[nodiscard]] StorageError destroy(OrderNode* node) noexcept {
    return node == nullptr ? StorageError::not_owned : destroy(*node);
  }
  [[nodiscard]] virtual std::size_t size() const noexcept = 0;

 protected:
  OrderStorage() = default;
};

class HeapOrderStorage final : public OrderStorage {
 public:
  HeapOrderStorage() = default;
  HeapOrderStorage(const HeapOrderStorage&) = delete;
  HeapOrderStorage& operator=(const HeapOrderStorage&) = delete;
  HeapOrderStorage(HeapOrderStorage&&) = delete;
  HeapOrderStorage& operator=(HeapOrderStorage&&) = delete;
  ~HeapOrderStorage() noexcept override;

  using OrderStorage::destroy;

  [[nodiscard]] CreateOrderResult create(const OrderNodeSpec& spec) override;
  [[nodiscard]] StorageError destroy(OrderNode& node) noexcept override;
  [[nodiscard]] std::size_t size() const noexcept override { return orders_.size(); }

 private:
#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
  friend class test::CoreAccess;
#endif

  using OwnedOrderNode = std::unique_ptr<OrderNode, OrderNodeDeleter>;
  using Orders =
      std::unordered_map<domain::OrderId, OwnedOrderNode, domain::StrongValueHash<domain::OrderId>>;

  Orders orders_;
};

}  // namespace atlaslob::core
