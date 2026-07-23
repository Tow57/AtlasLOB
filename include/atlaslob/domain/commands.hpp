#pragma once

#include <optional>

#include "atlaslob/domain/types.hpp"

namespace atlaslob::domain {

struct NewOrder final {
  OrderId order_id{};
  InstrumentId instrument_id{};
  Side side{Side::buy};
  OrderType order_type{OrderType::limit};
  TimeInForce time_in_force{TimeInForce::gtc};
  std::optional<PriceTicks> limit_price{};
  Quantity quantity{};
};

}  // namespace atlaslob::domain
