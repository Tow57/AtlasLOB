#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "order_storage.hpp"
#include "price_level.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::core;

constexpr std::size_t level_count = 8U;
constexpr std::size_t operation_count = 10'000U;
constexpr std::uint64_t stress_seed = 0xA71A'510B'2027ULL;

struct ActiveOrder final {
  OrderNode* node{};
  PriceLevel* level{};
};

OrderNodeSpec make_spec(std::uint64_t id, std::size_t level_index, std::uint64_t quantity,
                        std::uint64_t priority) {
  return {
      .order_id = domain::OrderId{id},
      .client_id = domain::ClientId{1U + static_cast<std::uint32_t>(id % 17U)},
      .instrument_id = domain::InstrumentId{1U},
      .side = id % 2U == 0U ? domain::Side::buy : domain::Side::sell,
      .price = domain::PriceTicks{10'000 + static_cast<std::int64_t>(level_index)},
      .remaining_quantity = domain::Quantity{quantity},
      .priority_sequence = domain::Sequence{priority},
  };
}

TEST(PriceLevelStress, MaintainsEveryInvariantForTenThousandSeededMutations) {
  HeapOrderStorage storage;
  std::array<std::unique_ptr<PriceLevel>, level_count> levels;
  for (std::size_t index = 0; index < levels.size(); ++index) {
    levels[index] =
        std::make_unique<PriceLevel>(domain::PriceTicks{10'000 + static_cast<std::int64_t>(index)});
  }

  std::mt19937_64 random{stress_seed};
  std::vector<ActiveOrder> active;
  std::uint64_t next_order_id = 1U;
  std::uint64_t next_priority = 1U;

  const auto erase_active = [&storage, &active](std::size_t index) {
    auto selected = active[index];
    if (selected.level->erase(*selected.node) != PriceLevelError::none) {
      return false;
    }
    if (storage.destroy(*selected.node) != StorageError::none) {
      return false;
    }
    active[index] = active.back();
    active.pop_back();
    return true;
  };

  for (std::size_t operation = 0; operation < operation_count; ++operation) {
    const auto action = static_cast<std::uint32_t>(random() % 100U);
    if (active.empty() || action < 45U) {
      const auto level_index = static_cast<std::size_t>(random() % level_count);
      const auto quantity = 1U + (random() % 100U);
      const auto created =
          storage.create(make_spec(next_order_id, level_index, quantity, next_priority));
      ASSERT_TRUE(created) << "operation=" << operation;
      ASSERT_EQ(levels[level_index]->append(*created.node), PriceLevelError::none)
          << "operation=" << operation;
      active.push_back({.node = created.node, .level = levels[level_index].get()});
      ++next_order_id;
      ++next_priority;
    } else {
      const auto active_index = static_cast<std::size_t>(random() % active.size());
      auto& selected = active[active_index];
      const auto remaining = selected.node->remaining_quantity().value();
      if (action < 72U && remaining > 1U) {
        const auto reduction = 1U + (random() % (remaining - 1U));
        ASSERT_EQ(selected.level->reduce_remaining(*selected.node, domain::Quantity{reduction}),
                  PriceLevelError::none)
            << "operation=" << operation;
      } else {
        ASSERT_TRUE(erase_active(active_index)) << "operation=" << operation;
      }
    }

    std::size_t observed_order_count = 0U;
    for (const auto& level : levels) {
      const auto invariant = level->validate_invariants();
      ASSERT_TRUE(invariant) << "operation=" << operation
                             << " error=" << to_string(invariant.error);
      observed_order_count += level->order_count();
    }
    ASSERT_EQ(observed_order_count, active.size()) << "operation=" << operation;
    ASSERT_EQ(storage.size(), active.size()) << "operation=" << operation;
  }

  while (!active.empty()) {
    ASSERT_TRUE(erase_active(active.size() - 1U));
  }
  EXPECT_EQ(storage.size(), 0U);
  for (const auto& level : levels) {
    EXPECT_TRUE(level->validate_invariants());
    EXPECT_TRUE(level->empty());
  }
}

}  // namespace
