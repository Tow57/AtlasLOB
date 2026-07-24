#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "atlaslob/domain/commands.hpp"
#include "execution_policy.hpp"
#include "instrument_book.hpp"

namespace atlaslob::core {

enum class StateValidationError : std::uint8_t {
  none = 0,
  invalid_policy = 1,
  book_invariant_violation = 2,
};

[[nodiscard]] constexpr std::string_view to_string(StateValidationError error) noexcept {
  switch (error) {
    case StateValidationError::none:
      return "none";
    case StateValidationError::invalid_policy:
      return "invalid_policy";
    case StateValidationError::book_invariant_violation:
      return "book_invariant_violation";
  }
  return "unknown";
}

struct StateValidationResult final {
  domain::RejectReason reason{domain::RejectReason::none};
  std::optional<domain::OrderId> relevant_order_id{};
  StateValidationError internal_error{StateValidationError::none};

  [[nodiscard]] constexpr bool accepted() const noexcept {
    return internal_error == StateValidationError::none && reason == domain::RejectReason::none;
  }

  [[nodiscard]] constexpr bool rejected() const noexcept {
    return internal_error == StateValidationError::none && reason != domain::RejectReason::none;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return accepted(); }

  bool operator==(const StateValidationResult&) const = default;
};

[[nodiscard]] StateValidationResult validate_state(const domain::NewOrder& order,
                                                   const InstrumentBook& book,
                                                   const ExecutionPolicy& policy) noexcept;
[[nodiscard]] StateValidationResult validate_state(const domain::CancelOrder& order,
                                                   const InstrumentBook& book,
                                                   const ExecutionPolicy& policy) noexcept;
[[nodiscard]] StateValidationResult validate_state(const domain::ReplaceOrder& order,
                                                   const InstrumentBook& book,
                                                   const ExecutionPolicy& policy) noexcept;
[[nodiscard]] StateValidationResult validate_state(const domain::Command& command,
                                                   const InstrumentBook& book,
                                                   const ExecutionPolicy& policy) noexcept;

}  // namespace atlaslob::core
