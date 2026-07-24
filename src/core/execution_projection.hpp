#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "match_plan.hpp"
#include "top_of_book.hpp"

namespace atlaslob::core {

enum class ExecutionProjectionError : std::uint8_t {
  none = 0,
  invalid_side = 1,
  invalid_plan = 2,
  invalid_resting_residual = 3,
  book_invariant_violation = 4,
  plan_book_mismatch = 5,
  cancel_target_mismatch = 6,
  aggregate_overflow = 7,
  aggregate_underflow = 8,
  crossed_book = 9,
  replacement_order_mismatch = 10,
};

[[nodiscard]] constexpr std::string_view to_string(ExecutionProjectionError error) noexcept {
  switch (error) {
    case ExecutionProjectionError::none:
      return "none";
    case ExecutionProjectionError::invalid_side:
      return "invalid_side";
    case ExecutionProjectionError::invalid_plan:
      return "invalid_plan";
    case ExecutionProjectionError::invalid_resting_residual:
      return "invalid_resting_residual";
    case ExecutionProjectionError::book_invariant_violation:
      return "book_invariant_violation";
    case ExecutionProjectionError::plan_book_mismatch:
      return "plan_book_mismatch";
    case ExecutionProjectionError::cancel_target_mismatch:
      return "cancel_target_mismatch";
    case ExecutionProjectionError::aggregate_overflow:
      return "aggregate_overflow";
    case ExecutionProjectionError::aggregate_underflow:
      return "aggregate_underflow";
    case ExecutionProjectionError::crossed_book:
      return "crossed_book";
    case ExecutionProjectionError::replacement_order_mismatch:
      return "replacement_order_mismatch";
  }
  return "unknown";
}

struct ProjectedRestingResidual final {
  domain::PriceTicks price{};
  domain::Quantity quantity{};

  bool operator==(const ProjectedRestingResidual&) const = default;
};

struct CancelProjectionTarget final {
  domain::OrderId order_id{};
  domain::Side side{};
  domain::PriceTicks price{};
  domain::Quantity remaining_quantity{};
  domain::Sequence priority_sequence{};

  bool operator==(const CancelProjectionTarget&) const = default;
};

[[nodiscard]] inline CancelProjectionTarget make_cancel_projection_target(
    const OrderNode& node) noexcept {
  return {
      .order_id = node.order_id(),
      .side = node.side(),
      .price = node.price(),
      .remaining_quantity = node.remaining_quantity(),
      .priority_sequence = node.priority_sequence(),
  };
}

struct ExecutionProjectionResult final {
  TopOfBookSnapshot snapshot{};
  ExecutionProjectionError error{ExecutionProjectionError::none};

  [[nodiscard]] constexpr bool has_value() const noexcept {
    return error == ExecutionProjectionError::none;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }

  bool operator==(const ExecutionProjectionResult&) const = default;
};

// Projects the visible top of book after applying an immutable match plan. When
// the plan leaves a GTC limit residual, `resting_residual` supplies its price
// and quantity. The helper performs no allocation and never mutates `book`.
[[nodiscard]] ExecutionProjectionResult project_new_top_of_book(
    const InstrumentBook& book, domain::Side aggressor_side, const MatchPlan& plan,
    std::optional<ProjectedRestingResidual> resting_residual = std::nullopt) noexcept;

// Projects the visible top of book after fully removing one currently active
// order. The value-only target is revalidated against the book before use.
[[nodiscard]] ExecutionProjectionResult project_cancel_top_of_book(
    const InstrumentBook& book, const CancelProjectionTarget& target) noexcept;

// Projects the visible top of book for an atomic replace. The old value-only
// target is removed from its side, passive fills from `plan` are applied to the
// opposite side, and an optional GTC residual is then added with new priority.
// The helper performs no allocation and never mutates `book`.
[[nodiscard]] ExecutionProjectionResult project_replace_top_of_book(
    const InstrumentBook& book, const CancelProjectionTarget& old_target,
    const domain::NewOrder& replacement_order, const MatchPlan& plan,
    std::optional<ProjectedRestingResidual> resting_residual = std::nullopt) noexcept;

}  // namespace atlaslob::core
