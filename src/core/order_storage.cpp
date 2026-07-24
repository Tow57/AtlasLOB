#include "order_storage.hpp"

#include <exception>
#include <memory>
#include <utility>

namespace atlaslob::core {
namespace {

[[nodiscard]] constexpr StorageError validate(const OrderNodeSpec& spec) noexcept {
  if (spec.order_id.value() == 0) {
    return StorageError::invalid_order_id;
  }
  if (spec.client_id.value() == 0) {
    return StorageError::invalid_client_id;
  }
  if (spec.instrument_id.value() == 0) {
    return StorageError::invalid_instrument_id;
  }
  if (!domain::is_valid(spec.side)) {
    return StorageError::invalid_side;
  }
  if (spec.price.value() <= 0) {
    return StorageError::invalid_price;
  }
  if (spec.remaining_quantity.value() == 0) {
    return StorageError::invalid_remaining_quantity;
  }
  if (spec.priority_sequence.value() == 0) {
    return StorageError::invalid_priority_sequence;
  }
  return StorageError::none;
}

}  // namespace

HeapOrderStorage::~HeapOrderStorage() noexcept {
  for (const auto& [order_id, node] : orders_) {
    static_cast<void>(order_id);
    if (node->is_linked()) {
      std::terminate();
    }
  }
}

CreateOrderResult HeapOrderStorage::create(const OrderNodeSpec& spec) {
  const auto validation_error = validate(spec);
  if (validation_error != StorageError::none) {
    return {.node = nullptr, .error = validation_error};
  }

  if (orders_.contains(spec.order_id)) {
    return {.node = nullptr, .error = StorageError::duplicate_order_id};
  }

  auto node = HeapOrderStorage::OwnedOrderNode{new OrderNode{spec}, OrderNodeDeleter{}};
  auto* const address = node.get();
  const auto [position, inserted] = orders_.emplace(spec.order_id, std::move(node));
  static_cast<void>(position);
  if (!inserted) {
    return {.node = nullptr, .error = StorageError::duplicate_order_id};
  }
  return {.node = address, .error = StorageError::none};
}

StorageError HeapOrderStorage::destroy(OrderNode& node) noexcept {
  const auto position = orders_.find(node.order_id());
  if (position == orders_.end() || position->second.get() != &node) {
    return StorageError::not_owned;
  }
  if (node.is_linked()) {
    return StorageError::node_linked;
  }

  orders_.erase(position);
  return StorageError::none;
}

OrderNode* HeapOrderStorage::find(domain::OrderId order_id) noexcept {
  const auto position = orders_.find(order_id);
  return position == orders_.end() ? nullptr : position->second.get();
}

const OrderNode* HeapOrderStorage::find(domain::OrderId order_id) const noexcept {
  const auto position = orders_.find(order_id);
  return position == orders_.end() ? nullptr : position->second.get();
}

bool HeapOrderStorage::owns(const OrderNode& node) const noexcept {
  return find(node.order_id()) == &node;
}

}  // namespace atlaslob::core
