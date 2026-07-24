#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <vector>

#include "instrument_book.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::core;

constexpr std::size_t operation_count = 10'000U;
constexpr std::size_t maximum_active_orders = 256U;
constexpr std::uint64_t stress_seed = 0xA71A'B00C'2027ULL;
constexpr domain::InstrumentId instrument_id{1U};

struct ModelOrder final {
  domain::OrderId order_id{};
  domain::ClientId client_id{};
  domain::Side side{};
  domain::PriceTicks price{};
  domain::Quantity remaining_quantity{};
  domain::Sequence priority_sequence{};
};

template <typename Comparator>
using SideModel = std::map<std::int64_t, std::vector<ModelOrder>, Comparator>;

OrderNodeSpec make_spec(const ModelOrder& order) {
  return {
      .order_id = order.order_id,
      .client_id = order.client_id,
      .instrument_id = instrument_id,
      .side = order.side,
      .price = order.price,
      .remaining_quantity = order.remaining_quantity,
      .priority_sequence = order.priority_sequence,
  };
}

template <typename Side, typename Model>
::testing::AssertionResult side_matches_model(const Side& side, const Model& model) {
  if (side.level_count() != model.size()) {
    return ::testing::AssertionFailure()
           << "level count actual=" << side.level_count() << " expected=" << model.size();
  }

  auto actual_level = side.begin();
  auto expected_level = model.begin();
  while (actual_level != side.end() && expected_level != model.end()) {
    const auto& level = *actual_level;
    const auto& [expected_price, expected_orders] = *expected_level;
    if (level.price().value() != expected_price) {
      return ::testing::AssertionFailure()
             << "level price actual=" << level.price().value() << " expected=" << expected_price;
    }
    if (level.order_count() != expected_orders.size()) {
      return ::testing::AssertionFailure()
             << "price=" << expected_price << " order count actual=" << level.order_count()
             << " expected=" << expected_orders.size();
    }

    std::uint64_t expected_aggregate = 0U;
    const OrderNode* node = level.head();
    for (const auto& expected : expected_orders) {
      if (node == nullptr) {
        return ::testing::AssertionFailure() << "price=" << expected_price << " FIFO ended early";
      }
      if (node->order_id() != expected.order_id || node->client_id() != expected.client_id ||
          node->instrument_id() != instrument_id || node->side() != expected.side ||
          node->price() != expected.price ||
          node->remaining_quantity() != expected.remaining_quantity ||
          node->priority_sequence() != expected.priority_sequence ||
          node->price_level() != &level) {
        return ::testing::AssertionFailure()
               << "price=" << expected_price << " unexpected node id=" << node->order_id().value()
               << " expected id=" << expected.order_id.value();
      }
      expected_aggregate += expected.remaining_quantity.value();
      node = node->next();
    }
    if (node != nullptr) {
      return ::testing::AssertionFailure() << "price=" << expected_price << " FIFO has extra nodes";
    }
    if (level.aggregate_quantity().value() != expected_aggregate) {
      return ::testing::AssertionFailure()
             << "price=" << expected_price
             << " aggregate actual=" << level.aggregate_quantity().value()
             << " expected=" << expected_aggregate;
    }

    ++actual_level;
    ++expected_level;
  }
  if (actual_level != side.end() || expected_level != model.end()) {
    return ::testing::AssertionFailure() << "side iteration lengths differ";
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult book_matches_model(const InstrumentBook& book,
                                              const std::vector<ModelOrder>& active) {
  const auto invariant = book.validate_invariants();
  if (!invariant) {
    return ::testing::AssertionFailure()
           << "invariant=" << to_string(invariant.error)
           << " order_id=" << invariant.order_id.value() << " price=" << invariant.price.value()
           << " observed=" << invariant.observed_order_count
           << " storage=" << invariant.storage_size << " index=" << invariant.index_size;
  }
  if (invariant.observed_order_count != active.size() || invariant.storage_size != active.size() ||
      invariant.index_size != active.size() || book.active_order_count() != active.size() ||
      book.empty() != active.empty()) {
    return ::testing::AssertionFailure()
           << "cardinality mismatch active=" << active.size()
           << " observed=" << invariant.observed_order_count
           << " storage=" << invariant.storage_size << " index=" << invariant.index_size
           << " public=" << book.active_order_count();
  }

  SideModel<std::greater<std::int64_t>> expected_bids;
  SideModel<std::less<std::int64_t>> expected_asks;
  for (const auto& expected : active) {
    const auto* const node = book.find(expected.order_id);
    if (node == nullptr || node->client_id() != expected.client_id ||
        node->instrument_id() != instrument_id || node->side() != expected.side ||
        node->price() != expected.price ||
        node->remaining_quantity() != expected.remaining_quantity ||
        node->priority_sequence() != expected.priority_sequence) {
      return ::testing::AssertionFailure()
             << "lookup mismatch for id=" << expected.order_id.value();
    }

    if (expected.side == domain::Side::buy) {
      expected_bids[expected.price.value()].push_back(expected);
    } else {
      expected_asks[expected.price.value()].push_back(expected);
    }
  }
  const auto priority_first = [](const ModelOrder& left, const ModelOrder& right) {
    return left.priority_sequence < right.priority_sequence;
  };
  for (auto& [price, orders] : expected_bids) {
    static_cast<void>(price);
    std::sort(orders.begin(), orders.end(), priority_first);
  }
  for (auto& [price, orders] : expected_asks) {
    static_cast<void>(price);
    std::sort(orders.begin(), orders.end(), priority_first);
  }

  const auto bids_match = side_matches_model(book.bids(), expected_bids);
  if (!bids_match) {
    return bids_match;
  }
  const auto asks_match = side_matches_model(book.asks(), expected_asks);
  if (!asks_match) {
    return asks_match;
  }

  if (expected_bids.empty()) {
    if (book.best_bid() != nullptr) {
      return ::testing::AssertionFailure() << "unexpected best bid";
    }
  } else if (book.best_bid() == nullptr ||
             book.best_bid()->price().value() != expected_bids.begin()->first) {
    return ::testing::AssertionFailure() << "best bid mismatch";
  }

  if (expected_asks.empty()) {
    if (book.best_ask() != nullptr) {
      return ::testing::AssertionFailure() << "unexpected best ask";
    }
  } else if (book.best_ask() == nullptr ||
             book.best_ask()->price().value() != expected_asks.begin()->first) {
    return ::testing::AssertionFailure() << "best ask mismatch";
  }

  return ::testing::AssertionSuccess();
}

TEST(InstrumentBookStress, MaintainsWholeBookInvariantsForTenThousandSeededMutations) {
  InstrumentBook book{instrument_id};
  std::mt19937_64 random{stress_seed};
  std::vector<ModelOrder> active;
  active.reserve(maximum_active_orders);
  std::uint64_t next_order_id = 1U;
  std::uint64_t next_priority = 1U;

  for (std::size_t operation = 0; operation < operation_count; ++operation) {
    SCOPED_TRACE(::testing::Message() << "operation=" << operation << " seed=" << stress_seed);
    const auto action = static_cast<std::uint32_t>(random() % 100U);
    const bool should_rest =
        active.empty() || (active.size() < maximum_active_orders && action < 45U);

    if (should_rest) {
      const bool is_buy = random() % 2U == 0U;
      const auto side = is_buy ? domain::Side::buy : domain::Side::sell;
      const auto price = is_buy ? 9'900 + static_cast<std::int64_t>(random() % 100U)
                                : 10'001 + static_cast<std::int64_t>(random() % 100U);
      const ModelOrder order{
          .order_id = domain::OrderId{next_order_id},
          .client_id = domain::ClientId{1U + static_cast<std::uint32_t>(random() % 17U)},
          .side = side,
          .price = domain::PriceTicks{price},
          .remaining_quantity = domain::Quantity{1U + random() % 100U},
          .priority_sequence = domain::Sequence{next_priority},
      };
      const auto result = book.rest(make_spec(order));
      ASSERT_TRUE(result) << "rest error=" << to_string(result.status.error)
                          << " component storage=" << to_string(result.status.storage_error)
                          << " index=" << to_string(result.status.index_error)
                          << " side=" << to_string(result.status.side_error)
                          << " level=" << to_string(result.status.level_error);
      ASSERT_EQ(result.node, book.find(order.order_id));
      active.push_back(order);
      ++next_order_id;
      ++next_priority;
    } else {
      const auto selected_index = static_cast<std::size_t>(random() % active.size());
      auto& selected = active[selected_index];
      const auto remaining = selected.remaining_quantity.value();
      const bool should_reduce = action < 72U && remaining > 1U;
      auto* const node = book.find(selected.order_id);
      ASSERT_NE(node, nullptr);

      if (should_reduce) {
        const auto reduction = 1U + random() % (remaining - 1U);
        const auto status = book.reduce_remaining(*node, domain::Quantity{reduction});
        ASSERT_TRUE(status) << "reduce error=" << to_string(status.error)
                            << " level=" << to_string(status.level_error);
        selected.remaining_quantity = domain::Quantity{remaining - reduction};
      } else {
        const auto result = book.cancel(selected.order_id);
        ASSERT_TRUE(result) << "cancel error=" << to_string(result.status.error);
        EXPECT_EQ(result.order.order_id, selected.order_id);
        EXPECT_EQ(result.order.client_id, selected.client_id);
        EXPECT_EQ(result.order.instrument_id, instrument_id);
        EXPECT_EQ(result.order.side, selected.side);
        EXPECT_EQ(result.order.price, selected.price);
        EXPECT_EQ(result.order.remaining_quantity, selected.remaining_quantity);
        EXPECT_EQ(result.order.priority_sequence, selected.priority_sequence);
        EXPECT_EQ(book.find(selected.order_id), nullptr);
        active[selected_index] = active.back();
        active.pop_back();
      }
    }

    ASSERT_TRUE(book_matches_model(book, active));
  }

  while (!active.empty()) {
    const auto selected = active.back();
    ASSERT_TRUE(book.cancel(selected.order_id));
    active.pop_back();
    ASSERT_TRUE(book_matches_model(book, active));
  }
  EXPECT_TRUE(book.empty());
  EXPECT_TRUE(book.validate_invariants());
}

}  // namespace
