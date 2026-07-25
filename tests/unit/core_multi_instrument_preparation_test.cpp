#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "atlaslob/multi_instrument_engine.hpp"
#include "multi_instrument_engine_access.hpp"

namespace {

using namespace atlaslob;

constexpr domain::InstrumentId first_instrument{7U};
constexpr domain::InstrumentId second_instrument{9U};
constexpr domain::ClientId first_client{11U};
constexpr domain::ClientId second_client{22U};

static_assert(!std::is_copy_constructible_v<core::PreparedMultiInstrumentCommand>);
static_assert(std::is_nothrow_move_constructible_v<core::PreparedMultiInstrumentCommand>);
static_assert(std::is_nothrow_move_assignable_v<core::PreparedMultiInstrumentCommand>);
static_assert(noexcept(std::declval<core::PreparedMultiInstrumentCommand&>().commit()));

[[nodiscard]] std::array<InstrumentConfig, 2U> catalog() {
  return {
      InstrumentConfig{
          .instrument_id = first_instrument,
          .matching =
              {
                  .max_order_quantity = domain::Quantity{1'000U},
                  .tick_increment = domain::PriceTicks{1},
                  .max_active_orders = 8U,
              },
      },
      InstrumentConfig{
          .instrument_id = second_instrument,
          .matching =
              {
                  .max_order_quantity = domain::Quantity{1'000U},
                  .tick_increment = domain::PriceTicks{1},
                  .max_active_orders = 8U,
              },
      },
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

TEST(MultiInstrumentPreparation,
     AbandoningARestingCommandLeavesDirectoryBookAndSequenceUnpublished) {
  const auto instruments = catalog();
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{instruments}};
  const auto before = engine.snapshot();
  const auto before_digest = engine.state_digest();

  {
    auto prepared = core::MultiInstrumentEngineAccess::prepare(
        engine, domain::Command{limit_order(1U, first_instrument, domain::Side::buy, 100, 5U)});

    ASSERT_TRUE(prepared.committed());
    ASSERT_NE(prepared.batch(), nullptr);
    EXPECT_EQ(prepared.batch()->command_sequence(), domain::Sequence{1U});
    EXPECT_EQ(engine.next_sequence(), domain::Sequence{1U});
    EXPECT_EQ(engine.active_order_count(), 0U);
    EXPECT_EQ(engine.top(first_instrument), std::optional<BookTop>{BookTop{}});
    EXPECT_EQ(engine.snapshot(), before);
    EXPECT_EQ(engine.state_digest(), before_digest);
    EXPECT_TRUE(engine.validate_invariants_for_testing());
  }

  EXPECT_EQ(engine.snapshot(), before);
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{1U});
  EXPECT_TRUE(engine.validate_invariants_for_testing());
}

TEST(MultiInstrumentPreparation, CommitPublishesThePreparedBookDirectoryAndSequenceTogether) {
  const auto instruments = catalog();
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{instruments}};
  auto prepared = core::MultiInstrumentEngineAccess::prepare(
      engine, domain::Command{limit_order(1U, first_instrument, domain::Side::buy, 100, 5U)});
  ASSERT_TRUE(prepared.committed());
  const std::vector<domain::Event> expected_events{
      prepared.batch()->events().begin(),
      prepared.batch()->events().end(),
  };

  auto result = prepared.commit();

  ASSERT_TRUE(result.committed());
  ASSERT_NE(result.batch(), nullptr);
  ASSERT_EQ(result.batch()->size(), expected_events.size());
  for (std::size_t index = 0U; index < expected_events.size(); ++index) {
    EXPECT_EQ((*result.batch())[index], expected_events[index]);
  }
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{2U});
  EXPECT_EQ(engine.active_order_count(), 1U);
  EXPECT_EQ(engine.snapshot(first_instrument)->active_order_count, 1U);
  EXPECT_TRUE(engine.validate_invariants_for_testing());
}

TEST(MultiInstrumentPreparation, RejectionIsInspectableAndConsumesItsSequenceOnlyAtCommit) {
  const auto instruments = catalog();
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{instruments}};
  auto invalid = limit_order(1U, first_instrument, domain::Side::buy, 100, 0U);
  auto prepared = core::MultiInstrumentEngineAccess::prepare(engine, domain::Command{invalid});

  ASSERT_TRUE(prepared.rejected());
  ASSERT_NE(prepared.batch(), nullptr);
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{1U});
  EXPECT_EQ(engine.snapshot().last_sequence, domain::Sequence{});

  auto result = prepared.commit();

  ASSERT_TRUE(result.rejected());
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{2U});
  EXPECT_EQ(engine.snapshot().last_sequence, domain::Sequence{1U});
  EXPECT_EQ(engine.active_order_count(), 0U);
  EXPECT_TRUE(engine.validate_invariants_for_testing());
}

TEST(MultiInstrumentPreparation, LeaseRejectsOverlapForCancelFullFillAndRejectionPreparations) {
  const auto instruments = catalog();
  const std::array first_commands{
      domain::Command{domain::CancelOrder{
          .client_id = first_client,
          .order_id = domain::OrderId{1U},
          .instrument_id = first_instrument,
      }},
      domain::Command{limit_order(2U, first_instrument, domain::Side::buy, 100, 5U, second_client)},
      domain::Command{limit_order(3U, second_instrument, domain::Side::buy, 90, 0U, second_client)},
  };

  for (const auto& first_command : first_commands) {
    MultiInstrumentEngine isolated{std::span<const InstrumentConfig>{instruments}};
    ASSERT_TRUE(isolated.execute(limit_order(1U, first_instrument, domain::Side::sell, 100, 5U))
                    .committed());
    const auto before = isolated.snapshot();

    {
      auto first = core::MultiInstrumentEngineAccess::prepare(isolated, first_command);
      ASSERT_TRUE(first.has_value());
      auto overlap = core::MultiInstrumentEngineAccess::prepare(
          isolated,
          domain::Command{limit_order(99U, second_instrument, domain::Side::buy, 90, 2U)});
      EXPECT_FALSE(overlap.has_value());
      EXPECT_EQ(overlap.error(), EngineError::internal_failure);
      EXPECT_EQ(overlap.commit().error(), EngineError::internal_failure);
      EXPECT_EQ(isolated.next_sequence(), domain::Sequence{2U});
      EXPECT_EQ(isolated.snapshot(), before);
      EXPECT_TRUE(isolated.validate_invariants_for_testing());
    }

    EXPECT_EQ(isolated.snapshot(), before);
    EXPECT_EQ(isolated.next_sequence(), domain::Sequence{2U});
    EXPECT_TRUE(isolated.validate_invariants_for_testing());
  }
}

TEST(MultiInstrumentPreparation, MoveTransfersTheExclusiveCoordinatorLeaseAndCommitRight) {
  const auto instruments = catalog();
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{instruments}};
  auto first = core::MultiInstrumentEngineAccess::prepare(
      engine, domain::Command{limit_order(1U, first_instrument, domain::Side::buy, 100, 5U)});
  auto moved = std::move(first);

  EXPECT_FALSE(first.has_value());
  EXPECT_EQ(first.error(), EngineError::internal_failure);
  auto overlap = core::MultiInstrumentEngineAccess::prepare(
      engine, domain::Command{limit_order(2U, second_instrument, domain::Side::buy, 90, 5U)});
  EXPECT_EQ(overlap.error(), EngineError::internal_failure);

  EXPECT_TRUE(moved.commit().committed());
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{2U});
  EXPECT_EQ(engine.active_order_count(), 1U);
  EXPECT_TRUE(engine.validate_invariants_for_testing());
}

}  // namespace
