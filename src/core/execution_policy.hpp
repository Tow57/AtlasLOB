#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "atlaslob/domain/types.hpp"

namespace atlaslob::core {

struct ExecutionPolicy final {
  domain::Quantity max_order_quantity{std::numeric_limits<std::uint64_t>::max()};
  domain::PriceTicks tick_increment{1};
  std::size_t max_active_orders{std::numeric_limits<std::size_t>::max()};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return max_order_quantity.value() != 0U && tick_increment.value() > 0;
  }

  bool operator==(const ExecutionPolicy&) const = default;
};

}  // namespace atlaslob::core
