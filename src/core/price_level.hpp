#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "order_node.hpp"

namespace atlaslob::core {

enum class PriceLevelError : std::uint8_t {
  none = 0,
  invalid_level_price = 1,
  invalid_remaining_quantity = 2,
  price_mismatch = 3,
  node_already_linked = 4,
  invalid_priority_sequence = 5,
  nonmonotonic_priority = 6,
  aggregate_overflow = 7,
  order_count_overflow = 8,
  node_not_in_level = 9,
  invalid_reduction = 10,
  aggregate_underflow = 11,
  level_invariant_violation = 12,
};

[[nodiscard]] constexpr std::string_view to_string(PriceLevelError error) noexcept {
  switch (error) {
    case PriceLevelError::none:
      return "none";
    case PriceLevelError::invalid_level_price:
      return "invalid_level_price";
    case PriceLevelError::invalid_remaining_quantity:
      return "invalid_remaining_quantity";
    case PriceLevelError::price_mismatch:
      return "price_mismatch";
    case PriceLevelError::node_already_linked:
      return "node_already_linked";
    case PriceLevelError::invalid_priority_sequence:
      return "invalid_priority_sequence";
    case PriceLevelError::nonmonotonic_priority:
      return "nonmonotonic_priority";
    case PriceLevelError::aggregate_overflow:
      return "aggregate_overflow";
    case PriceLevelError::order_count_overflow:
      return "order_count_overflow";
    case PriceLevelError::node_not_in_level:
      return "node_not_in_level";
    case PriceLevelError::invalid_reduction:
      return "invalid_reduction";
    case PriceLevelError::aggregate_underflow:
      return "aggregate_underflow";
    case PriceLevelError::level_invariant_violation:
      return "level_invariant_violation";
  }
  return "unknown";
}

enum class PriceLevelInvariantError : std::uint8_t {
  none = 0,
  invalid_level_price = 1,
  empty_has_head = 2,
  empty_has_tail = 3,
  empty_has_aggregate = 4,
  nonempty_missing_head = 5,
  nonempty_missing_tail = 6,
  head_has_previous = 7,
  tail_has_next = 8,
  forward_cycle = 9,
  backward_cycle = 10,
  wrong_backlink = 11,
  wrong_price = 12,
  zero_remaining_quantity = 13,
  zero_priority_sequence = 14,
  nonmonotonic_priority = 15,
  broken_previous_link = 16,
  broken_next_link = 17,
  forward_tail_mismatch = 18,
  backward_head_mismatch = 19,
  count_overflow = 20,
  count_mismatch = 21,
  aggregate_overflow = 22,
  aggregate_mismatch = 23,
};

[[nodiscard]] constexpr std::string_view to_string(PriceLevelInvariantError error) noexcept {
  switch (error) {
    case PriceLevelInvariantError::none:
      return "none";
    case PriceLevelInvariantError::invalid_level_price:
      return "invalid_level_price";
    case PriceLevelInvariantError::empty_has_head:
      return "empty_has_head";
    case PriceLevelInvariantError::empty_has_tail:
      return "empty_has_tail";
    case PriceLevelInvariantError::empty_has_aggregate:
      return "empty_has_aggregate";
    case PriceLevelInvariantError::nonempty_missing_head:
      return "nonempty_missing_head";
    case PriceLevelInvariantError::nonempty_missing_tail:
      return "nonempty_missing_tail";
    case PriceLevelInvariantError::head_has_previous:
      return "head_has_previous";
    case PriceLevelInvariantError::tail_has_next:
      return "tail_has_next";
    case PriceLevelInvariantError::forward_cycle:
      return "forward_cycle";
    case PriceLevelInvariantError::backward_cycle:
      return "backward_cycle";
    case PriceLevelInvariantError::wrong_backlink:
      return "wrong_backlink";
    case PriceLevelInvariantError::wrong_price:
      return "wrong_price";
    case PriceLevelInvariantError::zero_remaining_quantity:
      return "zero_remaining_quantity";
    case PriceLevelInvariantError::zero_priority_sequence:
      return "zero_priority_sequence";
    case PriceLevelInvariantError::nonmonotonic_priority:
      return "nonmonotonic_priority";
    case PriceLevelInvariantError::broken_previous_link:
      return "broken_previous_link";
    case PriceLevelInvariantError::broken_next_link:
      return "broken_next_link";
    case PriceLevelInvariantError::forward_tail_mismatch:
      return "forward_tail_mismatch";
    case PriceLevelInvariantError::backward_head_mismatch:
      return "backward_head_mismatch";
    case PriceLevelInvariantError::count_overflow:
      return "count_overflow";
    case PriceLevelInvariantError::count_mismatch:
      return "count_mismatch";
    case PriceLevelInvariantError::aggregate_overflow:
      return "aggregate_overflow";
    case PriceLevelInvariantError::aggregate_mismatch:
      return "aggregate_mismatch";
  }
  return "unknown";
}

struct PriceLevelInvariantResult final {
  PriceLevelInvariantError error{PriceLevelInvariantError::none};
  const OrderNode* node{};
  std::size_t observed_order_count{};
  domain::Quantity observed_aggregate_quantity{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return error == PriceLevelInvariantError::none;
  }

  explicit constexpr operator bool() const noexcept { return valid(); }

  bool operator==(const PriceLevelInvariantResult&) const = default;
};

class PriceLevel final {
 public:
  explicit PriceLevel(domain::PriceTicks price) noexcept : price_{price} {}

  PriceLevel(const PriceLevel&) = delete;
  PriceLevel& operator=(const PriceLevel&) = delete;
  PriceLevel(PriceLevel&&) = delete;
  PriceLevel& operator=(PriceLevel&&) = delete;
  ~PriceLevel() noexcept;

  [[nodiscard]] domain::PriceTicks price() const noexcept { return price_; }
  [[nodiscard]] domain::Quantity aggregate_quantity() const noexcept { return aggregate_quantity_; }
  [[nodiscard]] std::size_t order_count() const noexcept { return order_count_; }
  [[nodiscard]] const OrderNode* head() const noexcept { return head_; }
  [[nodiscard]] const OrderNode* tail() const noexcept { return tail_; }
  [[nodiscard]] bool empty() const noexcept { return order_count_ == 0; }

  [[nodiscard]] PriceLevelError append(OrderNode& node) noexcept;
  [[nodiscard]] PriceLevelError erase(OrderNode& node) noexcept;
  [[nodiscard]] PriceLevelError reduce_remaining(OrderNode& node,
                                                 domain::Quantity reduction) noexcept;

  [[nodiscard]] PriceLevelInvariantResult validate_invariants() const noexcept;

 private:
#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
  friend class test::CoreAccess;
#endif

  [[nodiscard]] PriceLevelError validate_boundary_state() const noexcept;
  [[nodiscard]] PriceLevelError validate_member(const OrderNode& node) const noexcept;
  void enforce_postconditions() const noexcept;

  domain::PriceTicks price_{};
  domain::Quantity aggregate_quantity_{};
  std::size_t order_count_{};
  OrderNode* head_{};
  OrderNode* tail_{};
};

}  // namespace atlaslob::core
