#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include "atlaslob/multi_instrument_engine.hpp"

namespace {

using namespace atlaslob;

class SplitMix64 final {
 public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_{seed} {}

  [[nodiscard]] std::uint64_t next() noexcept {
    state_ += 0x9e3779b97f4a7c15ULL;
    auto value = state_;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }

  [[nodiscard]] std::size_t below(std::size_t bound) noexcept {
    return static_cast<std::size_t>(next() % static_cast<std::uint64_t>(bound));
  }

 private:
  std::uint64_t state_;
};

struct ActiveOrder final {
  domain::OrderId order_id{};
  domain::ClientId client_id{};
  domain::InstrumentId instrument_id{};
};

[[nodiscard]] std::vector<ActiveOrder> active_orders(const EngineSnapshot& snapshot) {
  std::vector<ActiveOrder> result;
  result.reserve(static_cast<std::size_t>(snapshot.active_order_count));
  for (const auto& instrument : snapshot.instruments) {
    const auto append_side = [&result](const auto& levels) {
      for (const auto& level : levels) {
        for (const auto& order : level.orders) {
          result.push_back({
              .order_id = order.order_id,
              .client_id = order.client_id,
              .instrument_id = order.instrument_id,
          });
        }
      }
    };
    append_side(instrument.bids);
    append_side(instrument.asks);
  }
  return result;
}

void verify_snapshot(const EngineSnapshot& snapshot, std::size_t max_total_active_orders) {
  ASSERT_EQ(snapshot.catalog.size(), snapshot.instruments.size());
  ASSERT_LE(snapshot.active_order_count, static_cast<std::uint64_t>(max_total_active_orders));

  std::unordered_set<domain::OrderId, domain::StrongValueHash<domain::OrderId>> order_ids;
  std::unordered_set<domain::Sequence, domain::StrongValueHash<domain::Sequence>> priorities;
  std::uint64_t observed_total = 0U;
  for (std::size_t instrument_index = 0U; instrument_index < snapshot.instruments.size();
       ++instrument_index) {
    const auto& instrument = snapshot.instruments[instrument_index];
    const auto& config = snapshot.catalog[instrument_index];
    ASSERT_EQ(instrument.instrument_id, config.instrument_id);
    ASSERT_LE(instrument.active_order_count,
              static_cast<std::uint64_t>(config.matching.max_active_orders));

    std::uint64_t observed_instrument = 0U;
    const auto verify_side = [&instrument, &order_ids, &priorities, &observed_instrument,
                              &snapshot](const auto& levels, domain::Side expected_side) {
      for (const auto& level : levels) {
        std::uint64_t aggregate = 0U;
        for (const auto& order : level.orders) {
          ASSERT_EQ(order.instrument_id, instrument.instrument_id);
          ASSERT_EQ(order.side, expected_side);
          ASSERT_EQ(order.price, level.price);
          ASSERT_NE(order.remaining_quantity.value(), 0U);
          ASSERT_NE(order.priority_sequence.value(), 0U);
          ASSERT_LE(order.priority_sequence, snapshot.last_sequence);
          ASSERT_TRUE(order_ids.insert(order.order_id).second);
          ASSERT_TRUE(priorities.insert(order.priority_sequence).second);
          ASSERT_LE(order.remaining_quantity.value(),
                    std::numeric_limits<std::uint64_t>::max() - aggregate);
          aggregate += order.remaining_quantity.value();
          ++observed_instrument;
        }
        ASSERT_EQ(aggregate, level.aggregate_quantity.value());
      }
    };
    verify_side(instrument.bids, domain::Side::buy);
    verify_side(instrument.asks, domain::Side::sell);
    ASSERT_EQ(observed_instrument, instrument.active_order_count);
    observed_total += observed_instrument;
  }
  ASSERT_EQ(observed_total, snapshot.active_order_count);
  ASSERT_EQ(order_ids.size(), static_cast<std::size_t>(snapshot.active_order_count));
  ASSERT_EQ(priorities.size(), static_cast<std::size_t>(snapshot.active_order_count));
}

TEST(MultiInstrumentEngineStress, FixedSeedMixedRoutingMaintainsGlobalAndPerBookInvariants) {
  constexpr std::array instrument_ids{
      domain::InstrumentId{7U},
      domain::InstrumentId{9U},
      domain::InstrumentId{11U},
  };
  constexpr std::size_t per_book_capacity = 48U;
  constexpr std::size_t global_capacity = 96U;
  const std::array catalog{
      InstrumentConfig{
          .instrument_id = instrument_ids[0],
          .matching =
              {
                  .max_order_quantity = domain::Quantity{100U},
                  .tick_increment = domain::PriceTicks{1},
                  .max_active_orders = per_book_capacity,
              },
      },
      InstrumentConfig{
          .instrument_id = instrument_ids[1],
          .matching =
              {
                  .max_order_quantity = domain::Quantity{100U},
                  .tick_increment = domain::PriceTicks{1},
                  .max_active_orders = per_book_capacity,
              },
      },
      InstrumentConfig{
          .instrument_id = instrument_ids[2],
          .matching =
              {
                  .max_order_quantity = domain::Quantity{100U},
                  .tick_increment = domain::PriceTicks{1},
                  .max_active_orders = per_book_capacity,
              },
      },
  };
  MultiInstrumentEngine engine{
      std::span<const InstrumentConfig>{catalog},
      MultiInstrumentEngineConfig{
          .max_total_active_orders = global_capacity,
      },
  };
  SplitMix64 random{0x41544c534d453031ULL};
  std::uint64_t next_order_id = 1U;

  for (std::size_t operation = 0U; operation < 10'000U; ++operation) {
    const auto before = engine.snapshot();
    const auto active = active_orders(before);
    const auto action = random.below(100U);
    domain::Command command;

    if (active.empty() || action < 58U) {
      const bool duplicate = !active.empty() && random.below(10U) == 0U;
      const auto order_id = duplicate ? active[random.below(active.size())].order_id
                                      : domain::OrderId{next_order_id++};
      const auto instrument = instrument_ids[random.below(instrument_ids.size())];
      const auto side = random.below(2U) == 0U ? domain::Side::buy : domain::Side::sell;
      const auto price = side == domain::Side::buy
                             ? 90 + static_cast<std::int64_t>(random.below(21U))
                             : 100 + static_cast<std::int64_t>(random.below(21U));
      command = domain::NewOrder{
          .client_id = domain::ClientId{1U + static_cast<std::uint32_t>(random.below(8U))},
          .order_id = order_id,
          .instrument_id = instrument,
          .side = side,
          .order_type = domain::OrderType::limit,
          .time_in_force =
              random.below(5U) == 0U ? domain::TimeInForce::ioc : domain::TimeInForce::gtc,
          .limit_price = domain::PriceTicks{price},
          .quantity = domain::Quantity{1U + static_cast<std::uint64_t>(random.below(20U))},
      };
    } else if (action < 80U) {
      const auto& selected = active[random.below(active.size())];
      auto instrument = selected.instrument_id;
      auto client = selected.client_id;
      if (random.below(8U) == 0U) {
        instrument = instrument_ids[(random.below(instrument_ids.size() - 1U) +
                                     (selected.instrument_id == instrument_ids[0] ? 1U : 0U)) %
                                    instrument_ids.size()];
      }
      if (random.below(10U) == 0U) {
        client = domain::ClientId{client.value() == 8U ? 1U : client.value() + 1U};
      }
      command = domain::CancelOrder{
          .client_id = client,
          .order_id = selected.order_id,
          .instrument_id = instrument,
      };
    } else {
      const auto& selected = active[random.below(active.size())];
      auto instrument = selected.instrument_id;
      auto client = selected.client_id;
      if (random.below(8U) == 0U) {
        instrument = instrument_ids[(random.below(instrument_ids.size() - 1U) +
                                     (selected.instrument_id == instrument_ids[0] ? 1U : 0U)) %
                                    instrument_ids.size()];
      }
      if (random.below(10U) == 0U) {
        client = domain::ClientId{client.value() == 8U ? 1U : client.value() + 1U};
      }
      const bool collide = active.size() > 1U && random.below(10U) == 0U;
      const auto replacement_id =
          collide ? active[random.below(active.size())].order_id : domain::OrderId{next_order_id++};
      command = domain::ReplaceOrder{
          .client_id = client,
          .old_order_id = selected.order_id,
          .new_order_id = replacement_id,
          .instrument_id = instrument,
          .new_limit_price = domain::PriceTicks{90 + static_cast<std::int64_t>(random.below(31U))},
          .new_quantity = domain::Quantity{1U + static_cast<std::uint64_t>(random.below(20U))},
      };
    }

    const auto submitted_instrument =
        std::visit([](const auto& value) { return value.instrument_id; }, command);
    auto result = engine.execute(command);
    ASSERT_TRUE(result.has_value()) << "operation=" << operation;
    ASSERT_EQ(result.error(), EngineError::none);
    ASSERT_NE(result.batch(), nullptr);
    ASSERT_EQ(result.batch()->command_sequence(),
              domain::Sequence{before.last_sequence.value() + 1U});
    for (std::size_t event_index = 0U; event_index < result.batch()->size(); ++event_index) {
      const auto& header = domain::event_header((*result.batch())[event_index]);
      ASSERT_EQ(header.command_sequence, result.batch()->command_sequence());
      ASSERT_EQ(header.event_index, static_cast<std::uint32_t>(event_index));
      ASSERT_EQ(header.instrument_id, submitted_instrument);
    }

    verify_snapshot(engine.snapshot(), global_capacity);
  }
}

}  // namespace
