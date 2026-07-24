#include "active_order_index.hpp"

namespace atlaslob::core {
namespace {

[[nodiscard]] bool entry_is_valid(domain::OrderId order_id, const OrderNode* node) noexcept {
  return order_id.value() != 0 && node != nullptr && node->order_id() == order_id;
}

}  // namespace

ActiveOrderIndexError ActiveOrderIndex::insert(OrderNode& node) {
  const auto order_id = node.order_id();
  if (order_id.value() == 0) {
    return ActiveOrderIndexError::invalid_order_id;
  }

  if (const auto position = orders_.find(order_id); position != orders_.end()) {
    if (!entry_is_valid(position->first, position->second)) {
      return ActiveOrderIndexError::index_invariant_violation;
    }
    return ActiveOrderIndexError::duplicate_order_id;
  }

  const auto [position, inserted] = orders_.emplace(order_id, &node);
  static_cast<void>(position);
  return inserted ? ActiveOrderIndexError::none : ActiveOrderIndexError::duplicate_order_id;
}

OrderNode* ActiveOrderIndex::find(domain::OrderId order_id) noexcept {
  if (order_id.value() == 0) {
    return nullptr;
  }

  const auto position = orders_.find(order_id);
  if (position == orders_.end() || !entry_is_valid(position->first, position->second)) {
    return nullptr;
  }
  return position->second;
}

const OrderNode* ActiveOrderIndex::find(domain::OrderId order_id) const noexcept {
  if (order_id.value() == 0) {
    return nullptr;
  }

  const auto position = orders_.find(order_id);
  if (position == orders_.end() || !entry_is_valid(position->first, position->second)) {
    return nullptr;
  }
  return position->second;
}

ActiveOrderIndexError ActiveOrderIndex::erase(OrderNode& node) noexcept {
  const auto order_id = node.order_id();
  if (order_id.value() == 0) {
    return ActiveOrderIndexError::invalid_order_id;
  }

  const auto position = orders_.find(order_id);
  if (position == orders_.end()) {
    return ActiveOrderIndexError::unknown_order_id;
  }
  if (!entry_is_valid(position->first, position->second)) {
    return ActiveOrderIndexError::index_invariant_violation;
  }
  if (position->second != &node) {
    return ActiveOrderIndexError::pointer_mismatch;
  }

  orders_.erase(position);
  return ActiveOrderIndexError::none;
}

OrderNode* ActiveOrderIndex::any_order() noexcept {
  for (const auto& [order_id, node] : orders_) {
    if (entry_is_valid(order_id, node)) {
      return node;
    }
  }
  return nullptr;
}

const OrderNode* ActiveOrderIndex::any_order() const noexcept {
  for (const auto& [order_id, node] : orders_) {
    if (entry_is_valid(order_id, node)) {
      return node;
    }
  }
  return nullptr;
}

ActiveOrderIndexInvariantResult ActiveOrderIndex::validate_invariants() const noexcept {
  ActiveOrderIndexInvariantResult result{};
  const auto fail = [&result](ActiveOrderIndexInvariantError error, domain::OrderId order_id,
                              const OrderNode* node) noexcept {
    result.error = error;
    result.order_id = order_id;
    result.node = node;
    return result;
  };

  for (const auto& [order_id, node] : orders_) {
    if (order_id.value() == 0) {
      return fail(ActiveOrderIndexInvariantError::invalid_order_id, order_id, node);
    }
    if (node == nullptr) {
      return fail(ActiveOrderIndexInvariantError::null_node, order_id, nullptr);
    }
    if (node->order_id() != order_id) {
      return fail(ActiveOrderIndexInvariantError::key_order_id_mismatch, order_id, node);
    }
  }

  return result;
}

}  // namespace atlaslob::core
