#pragma once

#include <optional>
#include <type_traits>
#include <variant>

#include "atlaslob/domain/types.hpp"

namespace atlaslob::domain {

struct NewOrder final {
  ClientId client_id{};
  OrderId order_id{};
  InstrumentId instrument_id{};
  Side side{};
  OrderType order_type{};
  TimeInForce time_in_force{};
  std::optional<PriceTicks> limit_price{};
  Quantity quantity{};
};

struct CancelOrder final {
  ClientId client_id{};
  OrderId order_id{};
  InstrumentId instrument_id{};
};

struct ReplaceOrder final {
  ClientId client_id{};
  OrderId old_order_id{};
  OrderId new_order_id{};
  InstrumentId instrument_id{};
  PriceTicks new_limit_price{};
  Quantity new_quantity{};
};

using Command = std::variant<NewOrder, CancelOrder, ReplaceOrder>;

[[nodiscard]] inline CommandType command_type(const Command& command) noexcept {
  return std::visit(
      [](const auto& value) noexcept {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, NewOrder>) {
          return CommandType::new_order;
        } else if constexpr (std::is_same_v<Value, CancelOrder>) {
          return CommandType::cancel;
        } else {
          static_assert(std::is_same_v<Value, ReplaceOrder>);
          return CommandType::replace;
        }
      },
      command);
}

}  // namespace atlaslob::domain
