#pragma once

#include <optional>

#include "atlaslob/domain/events.hpp"
#include "instrument_book.hpp"

namespace atlaslob::core {

struct TopOfBookSnapshot final {
  std::optional<domain::TopOfBookLevel> best_bid{};
  std::optional<domain::TopOfBookLevel> best_ask{};

  bool operator==(const TopOfBookSnapshot&) const = default;
};

namespace detail {

[[nodiscard]] inline std::optional<domain::TopOfBookLevel> snapshot_level(
    const PriceLevel* level) noexcept {
  if (level == nullptr) {
    return std::nullopt;
  }
  return domain::TopOfBookLevel{
      .price = level->price(),
      .aggregate_quantity = level->aggregate_quantity(),
  };
}

}  // namespace detail

[[nodiscard]] inline TopOfBookSnapshot snapshot_top_of_book(const InstrumentBook& book) noexcept {
  return {
      .best_bid = detail::snapshot_level(book.best_bid()),
      .best_ask = detail::snapshot_level(book.best_ask()),
  };
}

}  // namespace atlaslob::core
