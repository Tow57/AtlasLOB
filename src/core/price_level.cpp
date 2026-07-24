#include "price_level.hpp"

#include <exception>
#include <limits>

#include "checked_arithmetic.hpp"

namespace atlaslob::core {

namespace {

[[nodiscard]] constexpr bool is_positive(domain::PriceTicks value) noexcept {
  return value.value() > 0;
}

[[nodiscard]] constexpr bool is_positive(domain::Quantity value) noexcept {
  return value.value() > 0;
}

[[nodiscard]] constexpr bool is_assigned(domain::Sequence value) noexcept {
  return value.value() > 0;
}

}  // namespace

PriceLevel::~PriceLevel() noexcept {
  if (order_count_ != 0 || aggregate_quantity_.value() != 0 || head_ != nullptr ||
      tail_ != nullptr) {
    std::terminate();
  }
}

PriceLevelError PriceLevel::validate_boundary_state() const noexcept {
  if (!is_positive(price_)) {
    return PriceLevelError::invalid_level_price;
  }

  if (order_count_ == 0) {
    if (head_ != nullptr || tail_ != nullptr || aggregate_quantity_.value() != 0) {
      return PriceLevelError::level_invariant_violation;
    }
    return PriceLevelError::none;
  }

  if (head_ == nullptr || tail_ == nullptr || aggregate_quantity_.value() == 0) {
    return PriceLevelError::level_invariant_violation;
  }
  if (head_->previous_ != nullptr || tail_->next_ != nullptr) {
    return PriceLevelError::level_invariant_violation;
  }
  if (head_->price_level_ != this || tail_->price_level_ != this) {
    return PriceLevelError::level_invariant_violation;
  }
  if (head_->price_ != price_ || tail_->price_ != price_) {
    return PriceLevelError::level_invariant_violation;
  }
  if (!is_positive(head_->remaining_quantity_) || !is_positive(tail_->remaining_quantity_)) {
    return PriceLevelError::level_invariant_violation;
  }
  if (!is_assigned(head_->priority_sequence_) || !is_assigned(tail_->priority_sequence_)) {
    return PriceLevelError::level_invariant_violation;
  }
  if (order_count_ == 1 &&
      (head_ != tail_ || head_->next_ != nullptr || tail_->previous_ != nullptr)) {
    return PriceLevelError::level_invariant_violation;
  }
  if (order_count_ > 1 &&
      (head_ == tail_ || head_->next_ == nullptr || tail_->previous_ == nullptr)) {
    return PriceLevelError::level_invariant_violation;
  }

  return PriceLevelError::none;
}

PriceLevelError PriceLevel::validate_member(const OrderNode& node) const noexcept {
  const auto boundary_error = validate_boundary_state();
  if (boundary_error != PriceLevelError::none) {
    return boundary_error;
  }
  if (node.price_level_ != this) {
    return PriceLevelError::node_not_in_level;
  }
  if (node.price_ != price_ || !is_positive(node.remaining_quantity_) ||
      !is_assigned(node.priority_sequence_)) {
    return PriceLevelError::level_invariant_violation;
  }

  if (node.previous_ == nullptr) {
    if (head_ != &node) {
      return PriceLevelError::level_invariant_violation;
    }
  } else if (node.previous_->next_ != &node || node.previous_->price_level_ != this) {
    return PriceLevelError::level_invariant_violation;
  }

  if (node.next_ == nullptr) {
    if (tail_ != &node) {
      return PriceLevelError::level_invariant_violation;
    }
  } else if (node.next_->previous_ != &node || node.next_->price_level_ != this) {
    return PriceLevelError::level_invariant_violation;
  }

  return PriceLevelError::none;
}

PriceLevelError PriceLevel::append(OrderNode& node) noexcept {
  const auto boundary_error = validate_boundary_state();
  if (boundary_error != PriceLevelError::none) {
    return boundary_error;
  }
  if (!is_positive(node.remaining_quantity_)) {
    return PriceLevelError::invalid_remaining_quantity;
  }
  if (node.price_ != price_) {
    return PriceLevelError::price_mismatch;
  }
  if (node.previous_ != nullptr || node.next_ != nullptr || node.price_level_ != nullptr) {
    return PriceLevelError::node_already_linked;
  }
  if (!is_assigned(node.priority_sequence_)) {
    return PriceLevelError::invalid_priority_sequence;
  }
  if (tail_ != nullptr && node.priority_sequence_ <= tail_->priority_sequence_) {
    return PriceLevelError::nonmonotonic_priority;
  }

  std::uint64_t new_aggregate{};
  if (!detail::checked_add(aggregate_quantity_.value(), node.remaining_quantity_.value(),
                           new_aggregate)) {
    return PriceLevelError::aggregate_overflow;
  }
  if (order_count_ == std::numeric_limits<std::size_t>::max()) {
    return PriceLevelError::order_count_overflow;
  }

  OrderNode* const previous_tail = tail_;
  node.previous_ = previous_tail;
  node.next_ = nullptr;
  node.price_level_ = this;
  if (previous_tail == nullptr) {
    head_ = &node;
  } else {
    previous_tail->next_ = &node;
  }
  tail_ = &node;
  aggregate_quantity_ = domain::Quantity{new_aggregate};
  ++order_count_;

  enforce_postconditions();
  return PriceLevelError::none;
}

PriceLevelError PriceLevel::erase(OrderNode& node) noexcept {
  const auto membership_error = validate_member(node);
  if (membership_error != PriceLevelError::none) {
    return membership_error;
  }
  if (order_count_ == 0) {
    return PriceLevelError::level_invariant_violation;
  }

  std::uint64_t new_aggregate{};
  if (!detail::checked_subtract(aggregate_quantity_.value(), node.remaining_quantity_.value(),
                                new_aggregate)) {
    return PriceLevelError::aggregate_underflow;
  }
  const std::size_t new_count = order_count_ - 1;
  if ((new_count == 0 && new_aggregate != 0) || (new_count != 0 && new_aggregate == 0)) {
    return PriceLevelError::level_invariant_violation;
  }

  OrderNode* const previous = node.previous_;
  OrderNode* const next = node.next_;

  if (previous == nullptr) {
    head_ = next;
  } else {
    previous->next_ = next;
  }
  if (next == nullptr) {
    tail_ = previous;
  } else {
    next->previous_ = previous;
  }

  aggregate_quantity_ = domain::Quantity{new_aggregate};
  order_count_ = new_count;
  node.previous_ = nullptr;
  node.next_ = nullptr;
  node.price_level_ = nullptr;

  enforce_postconditions();
  return PriceLevelError::none;
}

PriceLevelError PriceLevel::reduce_remaining(OrderNode& node, domain::Quantity reduction) noexcept {
  const auto membership_error = validate_member(node);
  if (membership_error != PriceLevelError::none) {
    return membership_error;
  }
  if (!is_positive(reduction) || reduction.value() >= node.remaining_quantity_.value()) {
    return PriceLevelError::invalid_reduction;
  }

  std::uint64_t new_remaining{};
  std::uint64_t new_aggregate{};
  if (!detail::checked_subtract(node.remaining_quantity_.value(), reduction.value(),
                                new_remaining) ||
      new_remaining == 0) {
    return PriceLevelError::invalid_reduction;
  }
  if (!detail::checked_subtract(aggregate_quantity_.value(), reduction.value(), new_aggregate)) {
    return PriceLevelError::aggregate_underflow;
  }
  if (new_aggregate == 0) {
    return PriceLevelError::level_invariant_violation;
  }

  node.remaining_quantity_ = domain::Quantity{new_remaining};
  aggregate_quantity_ = domain::Quantity{new_aggregate};

  enforce_postconditions();
  return PriceLevelError::none;
}

PriceLevelInvariantResult PriceLevel::validate_invariants() const noexcept {
  PriceLevelInvariantResult result{};
  const auto fail = [&result](PriceLevelInvariantError error,
                              const OrderNode* node = nullptr) noexcept {
    result.error = error;
    result.node = node;
    return result;
  };

  if (!is_positive(price_)) {
    return fail(PriceLevelInvariantError::invalid_level_price);
  }

  if (order_count_ == 0) {
    if (head_ != nullptr) {
      return fail(PriceLevelInvariantError::empty_has_head, head_);
    }
    if (tail_ != nullptr) {
      return fail(PriceLevelInvariantError::empty_has_tail, tail_);
    }
    if (aggregate_quantity_.value() != 0) {
      return fail(PriceLevelInvariantError::empty_has_aggregate);
    }
    return result;
  }

  if (head_ == nullptr) {
    return fail(PriceLevelInvariantError::nonempty_missing_head);
  }
  if (tail_ == nullptr) {
    return fail(PriceLevelInvariantError::nonempty_missing_tail);
  }
  if (head_->previous_ != nullptr) {
    return fail(PriceLevelInvariantError::head_has_previous, head_);
  }
  if (tail_->next_ != nullptr) {
    return fail(PriceLevelInvariantError::tail_has_next, tail_);
  }

  const OrderNode* slow = head_;
  const OrderNode* fast = head_;
  while (fast != nullptr && fast->next_ != nullptr) {
    slow = slow->next_;
    fast = fast->next_->next_;
    if (slow == fast) {
      return fail(PriceLevelInvariantError::forward_cycle, slow);
    }
  }

  slow = tail_;
  fast = tail_;
  while (fast != nullptr && fast->previous_ != nullptr) {
    slow = slow->previous_;
    fast = fast->previous_->previous_;
    if (slow == fast) {
      return fail(PriceLevelInvariantError::backward_cycle, slow);
    }
  }

  const OrderNode* previous = nullptr;
  const OrderNode* current = head_;
  domain::Sequence previous_sequence{};
  std::uint64_t observed_aggregate{};
  while (current != nullptr) {
    result.node = current;

    if (current->price_level_ != this) {
      return fail(PriceLevelInvariantError::wrong_backlink, current);
    }
    if (current->price_ != price_) {
      return fail(PriceLevelInvariantError::wrong_price, current);
    }
    if (!is_positive(current->remaining_quantity_)) {
      return fail(PriceLevelInvariantError::zero_remaining_quantity, current);
    }
    if (!is_assigned(current->priority_sequence_)) {
      return fail(PriceLevelInvariantError::zero_priority_sequence, current);
    }
    if (previous != nullptr && current->priority_sequence_ <= previous_sequence) {
      return fail(PriceLevelInvariantError::nonmonotonic_priority, current);
    }
    if (current->previous_ != previous) {
      return fail(PriceLevelInvariantError::broken_previous_link, current);
    }
    if (current->next_ != nullptr && current->next_->previous_ != current) {
      return fail(PriceLevelInvariantError::broken_next_link, current);
    }
    if (result.observed_order_count == std::numeric_limits<std::size_t>::max()) {
      return fail(PriceLevelInvariantError::count_overflow, current);
    }
    ++result.observed_order_count;

    std::uint64_t new_aggregate{};
    if (!detail::checked_add(observed_aggregate, current->remaining_quantity_.value(),
                             new_aggregate)) {
      result.observed_aggregate_quantity = domain::Quantity{observed_aggregate};
      return fail(PriceLevelInvariantError::aggregate_overflow, current);
    }
    observed_aggregate = new_aggregate;
    result.observed_aggregate_quantity = domain::Quantity{observed_aggregate};

    previous = current;
    previous_sequence = current->priority_sequence_;
    current = current->next_;
  }

  if (previous != tail_) {
    return fail(PriceLevelInvariantError::forward_tail_mismatch, previous);
  }

  const OrderNode* next = nullptr;
  current = tail_;
  while (current != nullptr) {
    if (current->next_ != next) {
      return fail(PriceLevelInvariantError::broken_next_link, current);
    }
    if (current->previous_ != nullptr && current->previous_->next_ != current) {
      return fail(PriceLevelInvariantError::broken_previous_link, current);
    }
    next = current;
    current = current->previous_;
  }
  if (next != head_) {
    return fail(PriceLevelInvariantError::backward_head_mismatch, next);
  }
  if (result.observed_order_count != order_count_) {
    return fail(PriceLevelInvariantError::count_mismatch);
  }
  if (observed_aggregate != aggregate_quantity_.value()) {
    return fail(PriceLevelInvariantError::aggregate_mismatch);
  }

  result.node = nullptr;
  return result;
}

void PriceLevel::enforce_postconditions() const noexcept {
#if defined(ATLAS_ENABLE_INVARIANTS) && ATLAS_ENABLE_INVARIANTS
  if (!validate_invariants().valid()) {
    std::terminate();
  }
#endif
}

}  // namespace atlaslob::core
