#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>

#include "order_node.hpp"

namespace atlaslob::core {

enum class ActiveOrderIndexError : std::uint8_t {
  none = 0,
  invalid_order_id = 1,
  duplicate_order_id = 2,
  unknown_order_id = 3,
  pointer_mismatch = 4,
  index_invariant_violation = 5,
};

[[nodiscard]] constexpr std::string_view to_string(ActiveOrderIndexError error) noexcept {
  switch (error) {
    case ActiveOrderIndexError::none:
      return "none";
    case ActiveOrderIndexError::invalid_order_id:
      return "invalid_order_id";
    case ActiveOrderIndexError::duplicate_order_id:
      return "duplicate_order_id";
    case ActiveOrderIndexError::unknown_order_id:
      return "unknown_order_id";
    case ActiveOrderIndexError::pointer_mismatch:
      return "pointer_mismatch";
    case ActiveOrderIndexError::index_invariant_violation:
      return "index_invariant_violation";
  }
  return "unknown";
}

enum class ActiveOrderIndexInvariantError : std::uint8_t {
  none = 0,
  invalid_order_id = 1,
  null_node = 2,
  key_order_id_mismatch = 3,
};

[[nodiscard]] constexpr std::string_view to_string(ActiveOrderIndexInvariantError error) noexcept {
  switch (error) {
    case ActiveOrderIndexInvariantError::none:
      return "none";
    case ActiveOrderIndexInvariantError::invalid_order_id:
      return "invalid_order_id";
    case ActiveOrderIndexInvariantError::null_node:
      return "null_node";
    case ActiveOrderIndexInvariantError::key_order_id_mismatch:
      return "key_order_id_mismatch";
  }
  return "unknown";
}

struct ActiveOrderIndexInvariantResult final {
  ActiveOrderIndexInvariantError error{ActiveOrderIndexInvariantError::none};
  domain::OrderId order_id{};
  const OrderNode* node{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return error == ActiveOrderIndexInvariantError::none;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

  bool operator==(const ActiveOrderIndexInvariantResult&) const = default;
};

class ActiveOrderIndex final {
 public:
  ActiveOrderIndex() = default;
  ActiveOrderIndex(const ActiveOrderIndex&) = delete;
  ActiveOrderIndex& operator=(const ActiveOrderIndex&) = delete;
  ActiveOrderIndex(ActiveOrderIndex&&) = delete;
  ActiveOrderIndex& operator=(ActiveOrderIndex&&) = delete;
  ~ActiveOrderIndex() = default;

  [[nodiscard]] ActiveOrderIndexError insert(OrderNode& node);
  [[nodiscard]] OrderNode* find(domain::OrderId order_id) noexcept;
  [[nodiscard]] const OrderNode* find(domain::OrderId order_id) const noexcept;
  [[nodiscard]] ActiveOrderIndexError erase(OrderNode& node) noexcept;

  [[nodiscard]] bool empty() const noexcept { return orders_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return orders_.size(); }
  [[nodiscard]] OrderNode* any_order() noexcept;
  [[nodiscard]] const OrderNode* any_order() const noexcept;

  template <typename Visitor>
  void for_each(Visitor&& visitor) const {
    for (const auto& [order_id, node] : orders_) {
      visitor(order_id, static_cast<const OrderNode*>(node));
    }
  }

  [[nodiscard]] ActiveOrderIndexInvariantResult validate_invariants() const noexcept;

 private:
#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
  friend class test::CoreAccess;
#endif

  using Orders =
      std::unordered_map<domain::OrderId, OrderNode*, domain::StrongValueHash<domain::OrderId>>;

  Orders orders_;
};

}  // namespace atlaslob::core
