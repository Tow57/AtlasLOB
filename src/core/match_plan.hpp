#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "atlaslob/domain/commands.hpp"
#include "instrument_book.hpp"

namespace atlaslob::core {

enum class ResidualDisposition : std::uint8_t {
  rest = 1,
  filled = 2,
  ioc_canceled = 3,
  market_exhausted = 4,
};

[[nodiscard]] constexpr std::string_view to_string(ResidualDisposition disposition) noexcept {
  switch (disposition) {
    case ResidualDisposition::rest:
      return "rest";
    case ResidualDisposition::filled:
      return "filled";
    case ResidualDisposition::ioc_canceled:
      return "ioc_canceled";
    case ResidualDisposition::market_exhausted:
      return "market_exhausted";
  }
  return "unknown";
}

struct PlannedTrade final {
  domain::OrderId resting_order_id{};
  domain::ClientId resting_client_id{};
  domain::PriceTicks execution_price{};
  domain::Quantity execution_quantity{};
  domain::Quantity aggressor_remaining{};
  domain::Quantity resting_remaining_before{};
  domain::Quantity resting_remaining_after{};
  domain::Sequence expected_resting_priority{};

  bool operator==(const PlannedTrade&) const = default;
};

struct MatchPlan final {
  std::vector<PlannedTrade> trades;
  domain::Quantity residual_quantity{};
  ResidualDisposition residual_disposition{ResidualDisposition::filled};

  bool operator==(const MatchPlan&) const = default;
};

enum class MatchPlanError : std::uint8_t {
  none = 0,
  invalid_request = 1,
  instrument_mismatch = 2,
  duplicate_order_id = 3,
  book_invariant_violation = 4,
};

[[nodiscard]] constexpr std::string_view to_string(MatchPlanError error) noexcept {
  switch (error) {
    case MatchPlanError::none:
      return "none";
    case MatchPlanError::invalid_request:
      return "invalid_request";
    case MatchPlanError::instrument_mismatch:
      return "instrument_mismatch";
    case MatchPlanError::duplicate_order_id:
      return "duplicate_order_id";
    case MatchPlanError::book_invariant_violation:
      return "book_invariant_violation";
  }
  return "unknown";
}

struct MatchPlanResult final {
  MatchPlan plan{};
  MatchPlanError error{MatchPlanError::none};
  domain::RejectReason validation_reason{domain::RejectReason::none};

  [[nodiscard]] bool has_value() const noexcept { return error == MatchPlanError::none; }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
};

enum class ActiveOrderProjectionError : std::uint8_t {
  none = 0,
  invalid_plan = 1,
  count_underflow = 2,
  count_overflow = 3,
};

[[nodiscard]] constexpr std::string_view to_string(ActiveOrderProjectionError error) noexcept {
  switch (error) {
    case ActiveOrderProjectionError::none:
      return "none";
    case ActiveOrderProjectionError::invalid_plan:
      return "invalid_plan";
    case ActiveOrderProjectionError::count_underflow:
      return "count_underflow";
    case ActiveOrderProjectionError::count_overflow:
      return "count_overflow";
  }
  return "unknown";
}

struct ActiveOrderProjection final {
  std::size_t final_active_order_count{};
  ActiveOrderProjectionError error{ActiveOrderProjectionError::none};

  [[nodiscard]] constexpr bool has_value() const noexcept {
    return error == ActiveOrderProjectionError::none;
  }

  [[nodiscard]] constexpr bool within_limit(std::size_t maximum) const noexcept {
    return has_value() && final_active_order_count <= maximum;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }
};

// Produces a value-only execution plan without mutating the resting book. Allocation failure
// propagates to the caller so the command sequence may be consumed while semantic state remains
// unchanged.
[[nodiscard]] MatchPlanResult plan_matches(const domain::NewOrder& order,
                                           const InstrumentBook& book);

// Projects capacity from the complete plan before mutation. `removes_existing_order` is true for
// replace, whose old same-side order becomes terminal before any residual is published.
[[nodiscard]] ActiveOrderProjection project_active_order_count(
    const MatchPlan& plan, std::size_t current_active_order_count,
    bool removes_existing_order) noexcept;

}  // namespace atlaslob::core
