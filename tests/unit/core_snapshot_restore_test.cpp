#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

#include "atlaslob/multi_instrument_engine.hpp"
#include "multi_instrument_engine_access.hpp"

namespace {

using namespace atlaslob;

constexpr domain::InstrumentId first_instrument{7U};
constexpr domain::InstrumentId second_instrument{9U};
constexpr domain::ClientId first_client{11U};
constexpr domain::ClientId second_client{22U};

std::size_t restore_allocation_calls{};
std::optional<std::size_t> restore_allocation_failure{};
using RestoreAllocationStage = core::MultiInstrumentEngineAccess::RestoreAllocationStage;
std::array<std::size_t, 11U> restore_allocation_stage_counts{};

void restore_allocation_hook(RestoreAllocationStage stage) {
  const auto call = restore_allocation_calls;
  ++restore_allocation_calls;
  ++restore_allocation_stage_counts[static_cast<std::size_t>(stage)];
  if (restore_allocation_failure == call) {
    throw std::bad_alloc{};
  }
}

class RestoreAllocationHookGuard final {
 public:
  RestoreAllocationHookGuard() {
    core::MultiInstrumentEngineAccess::set_restore_allocation_hook_for_testing(
        &restore_allocation_hook);
  }

  RestoreAllocationHookGuard(const RestoreAllocationHookGuard&) = delete;
  RestoreAllocationHookGuard& operator=(const RestoreAllocationHookGuard&) = delete;
  RestoreAllocationHookGuard(RestoreAllocationHookGuard&&) = delete;
  RestoreAllocationHookGuard& operator=(RestoreAllocationHookGuard&&) = delete;

  ~RestoreAllocationHookGuard() {
    core::MultiInstrumentEngineAccess::set_restore_allocation_hook_for_testing(nullptr);
    restore_allocation_failure.reset();
    restore_allocation_calls = 0U;
    restore_allocation_stage_counts.fill(0U);
  }
};

[[nodiscard]] InstrumentConfig instrument_config(domain::InstrumentId instrument_id) {
  return {
      .instrument_id = instrument_id,
      .matching =
          {
              .max_order_quantity = domain::Quantity{1'000U},
              .tick_increment = domain::PriceTicks{1},
              .max_active_orders = 16U,
          },
  };
}

[[nodiscard]] std::array<InstrumentConfig, 2U> catalog() {
  return {
      instrument_config(first_instrument),
      instrument_config(second_instrument),
  };
}

[[nodiscard]] domain::NewOrder limit_order(std::uint64_t order_id,
                                           domain::InstrumentId instrument_id, domain::Side side,
                                           std::int64_t price, std::uint64_t quantity,
                                           domain::ClientId client_id = first_client) {
  return {
      .client_id = client_id,
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
      .side = side,
      .order_type = domain::OrderType::limit,
      .time_in_force = domain::TimeInForce::gtc,
      .limit_price = domain::PriceTicks{price},
      .quantity = domain::Quantity{quantity},
  };
}

void execute_committed(MultiInstrumentEngine& engine, const domain::NewOrder& order) {
  const auto result = engine.execute(order);
  if (!result.committed()) {
    throw std::logic_error{"test setup command did not commit"};
  }
}

[[nodiscard]] EngineSnapshot populated_snapshot() {
  const auto instruments = catalog();
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{instruments}};
  execute_committed(engine, limit_order(11U, first_instrument, domain::Side::buy, 100, 5U));
  execute_committed(engine, limit_order(12U, first_instrument, domain::Side::buy, 100, 7U));
  execute_committed(
      engine, limit_order(13U, first_instrument, domain::Side::sell, 110, 11U, second_client));
  execute_committed(
      engine, limit_order(21U, second_instrument, domain::Side::sell, 200, 13U, second_client));
  return engine.snapshot();
}

[[nodiscard]] std::unique_ptr<MultiInstrumentEngine> restore(
    const EngineSnapshot& snapshot, std::optional<Digest256> expected_digest = std::nullopt) {
  return core::MultiInstrumentEngineAccess::restore_snapshot(snapshot, expected_digest);
}

TEST(MultiInstrumentSnapshotRestore, RestoresEveryCanonicalEmptyInstrument) {
  const auto instruments = catalog();
  const MultiInstrumentEngine engine{std::span<const InstrumentConfig>{instruments}};
  const auto snapshot = engine.snapshot();
  const auto digest = engine.state_digest();

  const auto restored = restore(snapshot, digest);

  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->snapshot(), snapshot);
  EXPECT_EQ(restored->state_digest(), digest);
  EXPECT_EQ(restored->next_sequence(), domain::Sequence{1U});
  EXPECT_FALSE(restored->sequence_exhausted());
  EXPECT_TRUE(core::MultiInstrumentEngineAccess::validate_invariants(*restored));
  ASSERT_TRUE(restored->snapshot(first_instrument).has_value());
  ASSERT_TRUE(restored->snapshot(second_instrument).has_value());
  EXPECT_EQ(restored->snapshot(first_instrument)->active_order_count, 0U);
  EXPECT_EQ(restored->snapshot(second_instrument)->active_order_count, 0U);
}

TEST(MultiInstrumentSnapshotRestore, RebuildsFifoLevelsIndexesAndGlobalIdentities) {
  const auto snapshot = populated_snapshot();
  const auto digest = state_digest(snapshot);

  const auto restored = restore(snapshot, digest);

  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->snapshot(), snapshot);
  EXPECT_EQ(restored->state_digest(), digest);
  EXPECT_EQ(restored->active_order_count(), 4U);
  EXPECT_EQ(restored->next_sequence(), domain::Sequence{5U});
  EXPECT_TRUE(core::MultiInstrumentEngineAccess::validate_invariants(*restored));

  const auto first = restored->snapshot(first_instrument);
  ASSERT_TRUE(first.has_value());
  ASSERT_EQ(first->bids.size(), 1U);
  ASSERT_EQ(first->bids[0].orders.size(), 2U);
  EXPECT_EQ(first->bids[0].orders[0].order_id, domain::OrderId{11U});
  EXPECT_EQ(first->bids[0].orders[1].order_id, domain::OrderId{12U});

  const auto canceled = restored->execute(domain::CancelOrder{
      .client_id = first_client,
      .order_id = domain::OrderId{11U},
      .instrument_id = first_instrument,
  });
  ASSERT_TRUE(canceled.committed());
  EXPECT_EQ(canceled.batch()->command_sequence(), domain::Sequence{5U});
  EXPECT_EQ(restored->active_order_count(), 3U);
  EXPECT_TRUE(core::MultiInstrumentEngineAccess::validate_invariants(*restored));
}

TEST(MultiInstrumentSnapshotRestore, RebuildsAThousandOrdersThroughTheBulkPath) {
  auto instruments = catalog();
  for (auto& instrument : instruments) {
    instrument.matching.max_active_orders = 2'048U;
  }

  constexpr std::uint64_t order_count = 1'024U;
  PriceLevelSnapshot level{
      .price = domain::PriceTicks{100},
      .aggregate_quantity = domain::Quantity{order_count},
      .orders = {},
  };
  level.orders.reserve(order_count);
  for (std::uint64_t value = 1U; value <= order_count; ++value) {
    level.orders.push_back({
        .order_id = domain::OrderId{value},
        .client_id = first_client,
        .instrument_id = first_instrument,
        .side = domain::Side::buy,
        .price = level.price,
        .remaining_quantity = domain::Quantity{1U},
        .priority_sequence = domain::Sequence{value},
    });
  }

  const EngineSnapshot snapshot{
      .semantics_version = atlaslob_semantics_version,
      .engine_config = {.max_total_active_orders = 2'048U},
      .catalog = {instruments.begin(), instruments.end()},
      .last_sequence = domain::Sequence{order_count},
      .sequence_exhausted = false,
      .active_order_count = order_count,
      .instruments =
          {
              InstrumentSnapshot{
                  .instrument_id = first_instrument,
                  .active_order_count = order_count,
                  .bids = {std::move(level)},
                  .asks = {},
              },
              InstrumentSnapshot{
                  .instrument_id = second_instrument,
                  .active_order_count = 0U,
                  .bids = {},
                  .asks = {},
              },
          },
  };

  const auto restored = restore(snapshot, state_digest(snapshot));

  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->active_order_count(), order_count);
  EXPECT_EQ(restored->snapshot(), snapshot);
  EXPECT_TRUE(core::MultiInstrumentEngineAccess::validate_invariants(*restored));
}

TEST(MultiInstrumentSnapshotRestore, GlobalInvariantDetectsDuplicatePrioritiesInLinearPass) {
  const auto snapshot = populated_snapshot();
  auto restored = restore(snapshot, state_digest(snapshot));
  ASSERT_TRUE(core::MultiInstrumentEngineAccess::validate_invariants(*restored));

  core::MultiInstrumentEngineAccess::set_order_priority_for_testing(*restored, domain::OrderId{21U},
                                                                    domain::Sequence{1U});
  EXPECT_FALSE(core::MultiInstrumentEngineAccess::validate_invariants(*restored));

  core::MultiInstrumentEngineAccess::set_order_priority_for_testing(*restored, domain::OrderId{21U},
                                                                    domain::Sequence{4U});
  EXPECT_TRUE(core::MultiInstrumentEngineAccess::validate_invariants(*restored));
}

TEST(MultiInstrumentSnapshotRestore, PreservesARejectedCommandBoundary) {
  const auto instruments = catalog();
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{instruments}};
  const auto rejected =
      engine.execute(limit_order(0U, first_instrument, domain::Side::buy, 100, 5U));
  ASSERT_TRUE(rejected.rejected());
  const auto snapshot = engine.snapshot();
  ASSERT_EQ(snapshot.last_sequence, domain::Sequence{1U});
  ASSERT_EQ(snapshot.active_order_count, 0U);

  const auto restored = restore(snapshot, engine.state_digest());

  EXPECT_EQ(restored->next_sequence(), domain::Sequence{2U});
  const auto accepted =
      restored->execute(limit_order(1U, second_instrument, domain::Side::buy, 90, 3U));
  ASSERT_TRUE(accepted.committed());
  EXPECT_EQ(accepted.batch()->command_sequence(), domain::Sequence{2U});
  EXPECT_TRUE(core::MultiInstrumentEngineAccess::validate_invariants(*restored));
}

TEST(MultiInstrumentSnapshotRestore, PreservesGlobalSequenceExhaustion) {
  auto snapshot = populated_snapshot();
  snapshot.last_sequence = domain::Sequence{std::numeric_limits<std::uint64_t>::max()};
  snapshot.sequence_exhausted = true;

  const auto restored = restore(snapshot, state_digest(snapshot));

  EXPECT_TRUE(restored->sequence_exhausted());
  EXPECT_EQ(restored->next_sequence(), domain::Sequence{});
  EXPECT_EQ(restored->active_order_count(), 4U);
  const auto result =
      restored->execute(limit_order(99U, first_instrument, domain::Side::buy, 100, 1U));
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), EngineError::sequence_exhausted);
  EXPECT_EQ(restored->snapshot(), snapshot);
}

TEST(MultiInstrumentSnapshotRestore, RejectsDuplicateIdentityAndPriority) {
  const auto snapshot = populated_snapshot();

  auto duplicate_id = snapshot;
  duplicate_id.instruments[1].asks[0].orders[0].order_id =
      duplicate_id.instruments[0].bids[0].orders[0].order_id;
  EXPECT_THROW(static_cast<void>(restore(duplicate_id)), std::invalid_argument);

  auto duplicate_priority = snapshot;
  duplicate_priority.instruments[1].asks[0].orders[0].priority_sequence =
      duplicate_priority.instruments[0].bids[0].orders[0].priority_sequence;
  EXPECT_THROW(static_cast<void>(restore(duplicate_priority)), std::invalid_argument);
}

TEST(MultiInstrumentSnapshotRestore, RejectsMalformedFifoLevelsAndAggregates) {
  const auto snapshot = populated_snapshot();

  auto nonmonotonic_fifo = snapshot;
  std::swap(nonmonotonic_fifo.instruments[0].bids[0].orders[0].priority_sequence,
            nonmonotonic_fifo.instruments[0].bids[0].orders[1].priority_sequence);
  EXPECT_THROW(static_cast<void>(restore(nonmonotonic_fifo)), std::invalid_argument);

  auto wrong_aggregate = snapshot;
  wrong_aggregate.instruments[0].bids[0].aggregate_quantity = domain::Quantity{13U};
  EXPECT_THROW(static_cast<void>(restore(wrong_aggregate)), std::invalid_argument);

  auto wrong_count = snapshot;
  wrong_count.instruments[0].active_order_count = 4U;
  EXPECT_THROW(static_cast<void>(restore(wrong_count)), std::invalid_argument);

  auto empty_level = snapshot;
  empty_level.instruments[0].bids[0].orders.clear();
  empty_level.instruments[0].bids[0].aggregate_quantity = {};
  EXPECT_THROW(static_cast<void>(restore(empty_level)), std::invalid_argument);

  auto noncanonical_levels = snapshot;
  noncanonical_levels.instruments[0].bids.push_back({
      .price = domain::PriceTicks{90},
      .aggregate_quantity = domain::Quantity{1U},
      .orders =
          {
              OrderSnapshot{
                  .order_id = domain::OrderId{99U},
                  .client_id = first_client,
                  .instrument_id = first_instrument,
                  .side = domain::Side::buy,
                  .price = domain::PriceTicks{90},
                  .remaining_quantity = domain::Quantity{1U},
                  .priority_sequence = domain::Sequence{5U},
              },
          },
  });
  std::swap(noncanonical_levels.instruments[0].bids[0], noncanonical_levels.instruments[0].bids[1]);
  noncanonical_levels.instruments[0].active_order_count = 4U;
  noncanonical_levels.active_order_count = 5U;
  noncanonical_levels.last_sequence = domain::Sequence{5U};
  EXPECT_THROW(static_cast<void>(restore(noncanonical_levels)), std::invalid_argument);
}

TEST(MultiInstrumentSnapshotRestore, RejectsCrossingAndOutOfRangePriorities) {
  const auto snapshot = populated_snapshot();

  auto crossed = snapshot;
  crossed.instruments[0].asks[0].price = domain::PriceTicks{100};
  crossed.instruments[0].asks[0].orders[0].price = domain::PriceTicks{100};
  EXPECT_THROW(static_cast<void>(restore(crossed)), std::invalid_argument);

  auto future_priority = snapshot;
  future_priority.instruments[1].asks[0].orders[0].priority_sequence = domain::Sequence{5U};
  EXPECT_THROW(static_cast<void>(restore(future_priority)), std::invalid_argument);

  auto inconsistent_exhaustion = snapshot;
  inconsistent_exhaustion.sequence_exhausted = true;
  EXPECT_THROW(static_cast<void>(restore(inconsistent_exhaustion)), std::invalid_argument);
}

TEST(MultiInstrumentSnapshotRestore, RejectsCatalogTopologyAndDigestMismatch) {
  const auto snapshot = populated_snapshot();

  auto missing_instrument = snapshot;
  missing_instrument.instruments.pop_back();
  EXPECT_THROW(static_cast<void>(restore(missing_instrument)), std::invalid_argument);

  auto reversed_catalog = snapshot;
  std::swap(reversed_catalog.catalog[0], reversed_catalog.catalog[1]);
  std::swap(reversed_catalog.instruments[0], reversed_catalog.instruments[1]);
  EXPECT_THROW(static_cast<void>(restore(reversed_catalog)), std::invalid_argument);

  auto wrong_digest = state_digest(snapshot);
  wrong_digest.bytes[0] ^= 0xffU;
  EXPECT_THROW(static_cast<void>(restore(snapshot, wrong_digest)), std::invalid_argument);
}

TEST(MultiInstrumentSnapshotRestore, PropagatesEveryInjectedAllocationFailureWithoutPublishing) {
  const auto snapshot = populated_snapshot();
  const auto digest = state_digest(snapshot);

  std::size_t allocation_boundary_count{};
  {
    RestoreAllocationHookGuard hook_guard;
    restore_allocation_calls = 0U;
    const auto restored = restore(snapshot, digest);
    ASSERT_NE(restored, nullptr);
    allocation_boundary_count = restore_allocation_calls;
    EXPECT_EQ(
        restore_allocation_stage_counts[static_cast<std::size_t>(RestoreAllocationStage::engine)],
        1U);
    EXPECT_EQ(restore_allocation_stage_counts[static_cast<std::size_t>(
                  RestoreAllocationStage::global_directory_reserve)],
              1U);
    EXPECT_EQ(restore_allocation_stage_counts[static_cast<std::size_t>(
                  RestoreAllocationStage::global_priority_directory_reserve)],
              1U);
    EXPECT_EQ(restore_allocation_stage_counts[static_cast<std::size_t>(
                  RestoreAllocationStage::book_storage_reserve)],
              snapshot.instruments.size());
    EXPECT_EQ(restore_allocation_stage_counts[static_cast<std::size_t>(
                  RestoreAllocationStage::book_index_reserve)],
              snapshot.instruments.size());
    EXPECT_EQ(restore_allocation_stage_counts[static_cast<std::size_t>(
                  RestoreAllocationStage::price_level)],
              3U);
    EXPECT_EQ(restore_allocation_stage_counts[static_cast<std::size_t>(
                  RestoreAllocationStage::order_storage)],
              snapshot.active_order_count);
    EXPECT_EQ(restore_allocation_stage_counts[static_cast<std::size_t>(
                  RestoreAllocationStage::order_index)],
              snapshot.active_order_count);
    EXPECT_EQ(restore_allocation_stage_counts[static_cast<std::size_t>(
                  RestoreAllocationStage::global_identity)],
              snapshot.active_order_count);
    EXPECT_EQ(restore_allocation_stage_counts[static_cast<std::size_t>(
                  RestoreAllocationStage::global_priority_identity)],
              snapshot.active_order_count);
  }
  ASSERT_EQ(allocation_boundary_count, 26U);

  for (std::size_t failure = 0U; failure < allocation_boundary_count; ++failure) {
    {
      RestoreAllocationHookGuard hook_guard;
      restore_allocation_calls = 0U;
      restore_allocation_failure = failure;
      EXPECT_THROW(static_cast<void>(restore(snapshot, digest)), std::bad_alloc)
          << "allocation boundary " << failure;
      EXPECT_EQ(restore_allocation_calls, failure + 1U);
    }

    const auto restored = restore(snapshot, digest);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->snapshot(), snapshot);
    EXPECT_EQ(restored->state_digest(), digest);
    EXPECT_TRUE(core::MultiInstrumentEngineAccess::validate_invariants(*restored));
  }
}

}  // namespace
