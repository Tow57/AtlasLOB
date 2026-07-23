#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace atlaslob::domain {

template <typename Tag, typename Representation>
class StrongValue final {
 public:
  using representation_type = Representation;

  constexpr StrongValue() noexcept = default;
  explicit constexpr StrongValue(Representation value) noexcept : value_{value} {}

  [[nodiscard]] constexpr Representation value() const noexcept { return value_; }

  auto operator<=>(const StrongValue&) const = default;

 private:
  Representation value_{};
};

struct OrderIdTag;
struct ClientIdTag;
struct InstrumentIdTag;
struct SequenceTag;
struct PriceTicksTag;
struct QuantityTag;

using OrderId = StrongValue<OrderIdTag, std::uint64_t>;
using ClientId = StrongValue<ClientIdTag, std::uint32_t>;
using InstrumentId = StrongValue<InstrumentIdTag, std::uint32_t>;
using Sequence = StrongValue<SequenceTag, std::uint64_t>;
using PriceTicks = StrongValue<PriceTicksTag, std::int64_t>;
using Quantity = StrongValue<QuantityTag, std::uint64_t>;

template <typename Strong>
struct StrongValueHash final {
  [[nodiscard]] std::size_t operator()(Strong value) const noexcept {
    using Representation = typename Strong::representation_type;
    return std::hash<Representation>{}(value.value());
  }
};

enum class Side : std::uint8_t { buy = 1, sell = 2 };
enum class OrderType : std::uint8_t { limit = 1, market = 2 };
enum class TimeInForce : std::uint8_t { gtc = 1, ioc = 2, fok = 3 };
enum class CommandType : std::uint8_t { new_order = 1, cancel = 2, replace = 3 };
enum class EventType : std::uint8_t {
  accepted = 1,
  rejected = 2,
  trade = 3,
  rested = 4,
  canceled = 5,
  replaced = 6,
  done = 7,
  book_changed = 8,
};
enum class DoneReason : std::uint8_t {
  filled = 1,
  ioc_residual_canceled = 2,
  market_exhausted = 3,
  canceled = 4,
  replaced = 5,
  fok_unavailable = 6,
};

enum class RejectReason : std::uint16_t {
  none = 0,
  invalid_order_id = 1,
  invalid_instrument_id = 2,
  invalid_quantity = 3,
  invalid_side = 4,
  invalid_order_type = 5,
  invalid_time_in_force = 6,
  missing_limit_price = 7,
  unexpected_limit_price = 8,
  invalid_price = 9,
  invalid_order_type_time_in_force = 10,
  unsupported_time_in_force = 11,
  invalid_client_id = 12,
  unknown_instrument = 13,
  quantity_out_of_range = 14,
  invalid_tick = 15,
  duplicate_order_id = 16,
  unknown_order_id = 17,
  invalid_replacement_id = 18,
  ownership_mismatch = 19,
  instrument_mismatch = 20,
  capacity_exceeded = 21,
};

[[nodiscard]] constexpr bool is_valid(Side value) noexcept {
  switch (value) {
    case Side::buy:
    case Side::sell:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool is_valid(OrderType value) noexcept {
  switch (value) {
    case OrderType::limit:
    case OrderType::market:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool is_valid(TimeInForce value) noexcept {
  switch (value) {
    case TimeInForce::gtc:
    case TimeInForce::ioc:
    case TimeInForce::fok:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool is_valid(CommandType value) noexcept {
  switch (value) {
    case CommandType::new_order:
    case CommandType::cancel:
    case CommandType::replace:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool is_valid(EventType value) noexcept {
  switch (value) {
    case EventType::accepted:
    case EventType::rejected:
    case EventType::trade:
    case EventType::rested:
    case EventType::canceled:
    case EventType::replaced:
    case EventType::done:
    case EventType::book_changed:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool is_valid(DoneReason value) noexcept {
  switch (value) {
    case DoneReason::filled:
    case DoneReason::ioc_residual_canceled:
    case DoneReason::market_exhausted:
    case DoneReason::canceled:
    case DoneReason::replaced:
    case DoneReason::fok_unavailable:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool is_valid(RejectReason value) noexcept {
  switch (value) {
    case RejectReason::invalid_order_id:
    case RejectReason::invalid_instrument_id:
    case RejectReason::invalid_quantity:
    case RejectReason::invalid_side:
    case RejectReason::invalid_order_type:
    case RejectReason::invalid_time_in_force:
    case RejectReason::missing_limit_price:
    case RejectReason::unexpected_limit_price:
    case RejectReason::invalid_price:
    case RejectReason::invalid_order_type_time_in_force:
    case RejectReason::unsupported_time_in_force:
    case RejectReason::invalid_client_id:
    case RejectReason::unknown_instrument:
    case RejectReason::quantity_out_of_range:
    case RejectReason::invalid_tick:
    case RejectReason::duplicate_order_id:
    case RejectReason::unknown_order_id:
    case RejectReason::invalid_replacement_id:
    case RejectReason::ownership_mismatch:
    case RejectReason::instrument_mismatch:
    case RejectReason::capacity_exceeded:
      return true;
    case RejectReason::none:
      return false;
  }
  return false;
}

[[nodiscard]] constexpr std::string_view to_string(Side value) noexcept {
  switch (value) {
    case Side::buy:
      return "buy";
    case Side::sell:
      return "sell";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(OrderType value) noexcept {
  switch (value) {
    case OrderType::limit:
      return "limit";
    case OrderType::market:
      return "market";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(TimeInForce value) noexcept {
  switch (value) {
    case TimeInForce::gtc:
      return "gtc";
    case TimeInForce::ioc:
      return "ioc";
    case TimeInForce::fok:
      return "fok";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(CommandType value) noexcept {
  switch (value) {
    case CommandType::new_order:
      return "new";
    case CommandType::cancel:
      return "cancel";
    case CommandType::replace:
      return "replace";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(EventType value) noexcept {
  switch (value) {
    case EventType::accepted:
      return "accepted";
    case EventType::rejected:
      return "rejected";
    case EventType::trade:
      return "trade";
    case EventType::rested:
      return "rested";
    case EventType::canceled:
      return "canceled";
    case EventType::replaced:
      return "replaced";
    case EventType::done:
      return "done";
    case EventType::book_changed:
      return "book_changed";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(DoneReason value) noexcept {
  switch (value) {
    case DoneReason::filled:
      return "filled";
    case DoneReason::ioc_residual_canceled:
      return "ioc_residual_canceled";
    case DoneReason::market_exhausted:
      return "market_exhausted";
    case DoneReason::canceled:
      return "canceled";
    case DoneReason::replaced:
      return "replaced";
    case DoneReason::fok_unavailable:
      return "fok_unavailable";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(RejectReason value) noexcept {
  switch (value) {
    case RejectReason::none:
      return "none";
    case RejectReason::invalid_client_id:
      return "invalid_client_id";
    case RejectReason::invalid_order_id:
      return "invalid_order_id";
    case RejectReason::invalid_instrument_id:
      return "invalid_instrument_id";
    case RejectReason::invalid_quantity:
      return "invalid_quantity";
    case RejectReason::invalid_side:
      return "invalid_side";
    case RejectReason::invalid_order_type:
      return "invalid_order_type";
    case RejectReason::invalid_time_in_force:
      return "invalid_time_in_force";
    case RejectReason::missing_limit_price:
      return "missing_limit_price";
    case RejectReason::unexpected_limit_price:
      return "unexpected_limit_price";
    case RejectReason::invalid_price:
      return "invalid_price";
    case RejectReason::invalid_order_type_time_in_force:
      return "invalid_order_type_time_in_force";
    case RejectReason::unsupported_time_in_force:
      return "unsupported_time_in_force";
    case RejectReason::unknown_instrument:
      return "unknown_instrument";
    case RejectReason::quantity_out_of_range:
      return "quantity_out_of_range";
    case RejectReason::invalid_tick:
      return "invalid_tick";
    case RejectReason::duplicate_order_id:
      return "duplicate_order_id";
    case RejectReason::unknown_order_id:
      return "unknown_order_id";
    case RejectReason::invalid_replacement_id:
      return "invalid_replacement_id";
    case RejectReason::ownership_mismatch:
      return "ownership_mismatch";
    case RejectReason::instrument_mismatch:
      return "instrument_mismatch";
    case RejectReason::capacity_exceeded:
      return "capacity_exceeded";
  }
  return "unknown";
}

}  // namespace atlaslob::domain
