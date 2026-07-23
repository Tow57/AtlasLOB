#pragma once

#include <concepts>
#include <limits>

namespace atlaslob::core::detail {

template <std::unsigned_integral Integer>
[[nodiscard]] constexpr bool checked_add(Integer left, Integer right, Integer& result) noexcept {
  if (right > std::numeric_limits<Integer>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

template <std::unsigned_integral Integer>
[[nodiscard]] constexpr bool checked_subtract(Integer left, Integer right,
                                              Integer& result) noexcept {
  if (right > left) {
    return false;
  }
  result = left - right;
  return true;
}

}  // namespace atlaslob::core::detail
