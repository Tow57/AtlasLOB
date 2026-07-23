#pragma once

#include <cstdint>
#include <optional>
#include <type_traits>
#include <variant>

#include "atlaslob/domain/types.hpp"

namespace atlaslob::domain {

struct EventHeader final {
  Sequence command_sequence{};
  std::uint32_t event_index{};
  InstrumentId instrument_id{};

  bool operator==(const EventHeader&) const = default;
};

struct AcceptedEvent final {
  EventHeader header{};
  CommandType command_type{};

  bool operator==(const AcceptedEvent&) const = default;
};

struct RejectedEvent final {
  EventHeader header{};
  CommandType command_type{};
  RejectReason reason{RejectReason::none};
  std::optional<OrderId> order_id{};

  bool operator==(const RejectedEvent&) const = default;
};

struct TradeEvent final {
  EventHeader header{};
  OrderId aggressor_order_id{};
  OrderId resting_order_id{};
  ClientId aggressor_client_id{};
  ClientId resting_client_id{};
  Side aggressor_side{};
  PriceTicks execution_price{};
  Quantity execution_quantity{};
  Quantity aggressor_remaining{};
  Quantity resting_remaining{};

  bool operator==(const TradeEvent&) const = default;
};

struct RestedEvent final {
  EventHeader header{};
  OrderId order_id{};
  ClientId client_id{};
  Side side{};
  PriceTicks price{};
  Quantity remaining_quantity{};

  bool operator==(const RestedEvent&) const = default;
};

struct CanceledEvent final {
  EventHeader header{};
  OrderId order_id{};
  Quantity canceled_quantity{};

  bool operator==(const CanceledEvent&) const = default;
};

struct ReplacedEvent final {
  EventHeader header{};
  OrderId old_order_id{};
  OrderId new_order_id{};

  bool operator==(const ReplacedEvent&) const = default;
};

struct DoneEvent final {
  EventHeader header{};
  OrderId order_id{};
  DoneReason reason{};
  Quantity remaining_quantity{};

  bool operator==(const DoneEvent&) const = default;
};

struct TopOfBookLevel final {
  PriceTicks price{};
  Quantity aggregate_quantity{};

  bool operator==(const TopOfBookLevel&) const = default;
};

struct BookChangedEvent final {
  EventHeader header{};
  std::optional<TopOfBookLevel> best_bid{};
  std::optional<TopOfBookLevel> best_ask{};

  bool operator==(const BookChangedEvent&) const = default;
};

using Event = std::variant<AcceptedEvent, RejectedEvent, TradeEvent, RestedEvent, CanceledEvent,
                           ReplacedEvent, DoneEvent, BookChangedEvent>;

template <typename EventValue>
[[nodiscard]] constexpr EventType expected_event_type() noexcept {
  using Value = std::remove_cvref_t<EventValue>;
  if constexpr (std::is_same_v<Value, AcceptedEvent>) {
    return EventType::accepted;
  } else if constexpr (std::is_same_v<Value, RejectedEvent>) {
    return EventType::rejected;
  } else if constexpr (std::is_same_v<Value, TradeEvent>) {
    return EventType::trade;
  } else if constexpr (std::is_same_v<Value, RestedEvent>) {
    return EventType::rested;
  } else if constexpr (std::is_same_v<Value, CanceledEvent>) {
    return EventType::canceled;
  } else if constexpr (std::is_same_v<Value, ReplacedEvent>) {
    return EventType::replaced;
  } else if constexpr (std::is_same_v<Value, DoneEvent>) {
    return EventType::done;
  } else {
    static_assert(std::is_same_v<Value, BookChangedEvent>);
    return EventType::book_changed;
  }
}

[[nodiscard]] inline EventType event_type(const Event& event) noexcept {
  return std::visit(
      [](const auto& value) noexcept { return expected_event_type<decltype(value)>(); }, event);
}

}  // namespace atlaslob::domain
