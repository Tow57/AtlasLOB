#pragma once

#include <compare>
#include <cstdint>
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
struct InstrumentIdTag;
struct SequenceTag;
struct PriceTicksTag;
struct QuantityTag;

using OrderId = StrongValue<OrderIdTag, std::uint64_t>;
using InstrumentId = StrongValue<InstrumentIdTag, std::uint32_t>;
using Sequence = StrongValue<SequenceTag, std::uint64_t>;
using PriceTicks = StrongValue<PriceTicksTag, std::int64_t>;
using Quantity = StrongValue<QuantityTag, std::uint64_t>;

enum class Side : std::uint8_t { buy, sell };
enum class OrderType : std::uint8_t { limit, market };
enum class TimeInForce : std::uint8_t { gtc, ioc, fok };

enum class RejectReason : std::uint8_t {
  none,
  invalid_order_id,
  invalid_instrument_id,
  invalid_quantity,
  invalid_side,
  invalid_order_type,
  invalid_time_in_force,
  missing_limit_price,
  unexpected_limit_price,
  invalid_price,
  invalid_order_type_time_in_force,
  unsupported_time_in_force,
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

[[nodiscard]] constexpr std::string_view to_string(RejectReason value) noexcept {
  switch (value) {
    case RejectReason::none:
      return "none";
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
  }
  return "unknown";
}

}  // namespace atlaslob::domain
