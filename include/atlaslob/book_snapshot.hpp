#pragma once

#include <cstdint>
#include <vector>

#include "atlaslob/domain/types.hpp"

namespace atlaslob {

inline constexpr std::uint16_t atlaslob_semantics_version = 6U;

struct OrderSnapshot final {
  domain::OrderId order_id{};
  domain::ClientId client_id{};
  domain::InstrumentId instrument_id{};
  domain::Side side{};
  domain::PriceTicks price{};
  domain::Quantity remaining_quantity{};
  domain::Sequence priority_sequence{};

  bool operator==(const OrderSnapshot&) const = default;
};

struct PriceLevelSnapshot final {
  domain::PriceTicks price{};
  domain::Quantity aggregate_quantity{};
  std::vector<OrderSnapshot> orders;

  bool operator==(const PriceLevelSnapshot&) const = default;
};

struct BookSnapshot final {
  std::uint16_t semantics_version{atlaslob_semantics_version};
  domain::InstrumentId instrument_id{};
  domain::Sequence last_sequence{};
  bool sequence_exhausted{};
  std::uint64_t active_order_count{};
  std::vector<PriceLevelSnapshot> bids;
  std::vector<PriceLevelSnapshot> asks;

  bool operator==(const BookSnapshot&) const = default;
};

}  // namespace atlaslob
