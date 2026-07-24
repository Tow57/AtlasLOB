#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "atlaslob/book_snapshot.hpp"
#include "atlaslob/digest.hpp"
#include "atlaslob/matching_engine.hpp"
#include "canonical_digest.hpp"
#include "snapshot_sequence.hpp"

namespace {

using namespace atlaslob;

constexpr domain::InstrumentId instrument_id{7U};

domain::EventHeader event_header(std::uint32_t index,
                                 domain::Sequence sequence = domain::Sequence{41U},
                                 domain::InstrumentId instrument = instrument_id) {
  return {
      .command_sequence = sequence,
      .event_index = index,
      .instrument_id = instrument,
  };
}

std::vector<domain::Event> every_event() {
  return {
      domain::AcceptedEvent{
          .header = event_header(0U),
          .command_type = domain::CommandType::replace,
      },
      domain::RejectedEvent{
          .header = event_header(1U),
          .command_type = domain::CommandType::cancel,
          .reason = domain::RejectReason::unknown_order_id,
          .order_id = domain::OrderId{99U},
      },
      domain::TradeEvent{
          .header = event_header(2U),
          .aggressor_order_id = domain::OrderId{101U},
          .resting_order_id = domain::OrderId{102U},
          .aggressor_client_id = domain::ClientId{11U},
          .resting_client_id = domain::ClientId{12U},
          .aggressor_side = domain::Side::sell,
          .execution_price = domain::PriceTicks{1'001},
          .execution_quantity = domain::Quantity{13U},
          .aggressor_remaining = domain::Quantity{14U},
          .resting_remaining = domain::Quantity{15U},
      },
      domain::RestedEvent{
          .header = event_header(3U),
          .order_id = domain::OrderId{103U},
          .client_id = domain::ClientId{16U},
          .side = domain::Side::buy,
          .price = domain::PriceTicks{1'002},
          .remaining_quantity = domain::Quantity{17U},
      },
      domain::CanceledEvent{
          .header = event_header(4U),
          .order_id = domain::OrderId{104U},
          .canceled_quantity = domain::Quantity{18U},
      },
      domain::ReplacedEvent{
          .header = event_header(5U),
          .old_order_id = domain::OrderId{105U},
          .new_order_id = domain::OrderId{106U},
      },
      domain::DoneEvent{
          .header = event_header(6U),
          .order_id = domain::OrderId{107U},
          .reason = domain::DoneReason::replaced,
          .remaining_quantity = domain::Quantity{19U},
      },
      domain::BookChangedEvent{
          .header = event_header(7U),
          .best_bid =
              domain::TopOfBookLevel{
                  .price = domain::PriceTicks{1'000},
                  .aggregate_quantity = domain::Quantity{20U},
              },
          .best_ask = std::nullopt,
      },
  };
}

BookSnapshot representative_snapshot() {
  return {
      .semantics_version = atlaslob_semantics_version,
      .instrument_id = instrument_id,
      .last_sequence = domain::Sequence{5U},
      .sequence_exhausted = false,
      .active_order_count = 3U,
      .bids =
          {
              PriceLevelSnapshot{
                  .price = domain::PriceTicks{101},
                  .aggregate_quantity = domain::Quantity{7U},
                  .orders =
                      {
                          OrderSnapshot{
                              .order_id = domain::OrderId{2U},
                              .client_id = domain::ClientId{12U},
                              .instrument_id = instrument_id,
                              .side = domain::Side::buy,
                              .price = domain::PriceTicks{101},
                              .remaining_quantity = domain::Quantity{7U},
                              .priority_sequence = domain::Sequence{2U},
                          },
                      },
              },
              PriceLevelSnapshot{
                  .price = domain::PriceTicks{100},
                  .aggregate_quantity = domain::Quantity{16U},
                  .orders =
                      {
                          OrderSnapshot{
                              .order_id = domain::OrderId{1U},
                              .client_id = domain::ClientId{11U},
                              .instrument_id = instrument_id,
                              .side = domain::Side::buy,
                              .price = domain::PriceTicks{100},
                              .remaining_quantity = domain::Quantity{5U},
                              .priority_sequence = domain::Sequence{1U},
                          },
                          OrderSnapshot{
                              .order_id = domain::OrderId{3U},
                              .client_id = domain::ClientId{13U},
                              .instrument_id = instrument_id,
                              .side = domain::Side::buy,
                              .price = domain::PriceTicks{100},
                              .remaining_quantity = domain::Quantity{11U},
                              .priority_sequence = domain::Sequence{3U},
                          },
                      },
              },
          },
      .asks = {},
  };
}

domain::NewOrder limit_order(std::uint64_t order_id, std::uint32_t client_id, domain::Side side,
                             std::int64_t price, std::uint64_t quantity) {
  return {
      .client_id = domain::ClientId{client_id},
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
      .side = side,
      .order_type = domain::OrderType::limit,
      .time_in_force = domain::TimeInForce::gtc,
      .limit_price = domain::PriceTicks{price},
      .quantity = domain::Quantity{quantity},
  };
}

void populate_engine(MatchingEngine& engine) {
  EXPECT_TRUE(engine.execute(limit_order(1U, 11U, domain::Side::buy, 100, 5U)));
  EXPECT_TRUE(engine.execute(limit_order(2U, 12U, domain::Side::buy, 101, 7U)));
  EXPECT_TRUE(engine.execute(limit_order(3U, 13U, domain::Side::buy, 100, 11U)));
  EXPECT_TRUE(engine.execute(limit_order(4U, 14U, domain::Side::sell, 110, 13U)));
  EXPECT_TRUE(engine.execute(limit_order(5U, 15U, domain::Side::sell, 109, 17U)));
}

TEST(Sha256, MatchesPublishedEmptyAndAbcVectors) {
  const std::array<std::uint8_t, 0U> empty{};
  EXPECT_EQ(core::test::sha256_for_testing(empty).hex(),
            "e3b0c44298fc1c149afbf4c8996fb924"
            "27ae41e4649b934ca495991b7852b855");

  const std::array<std::uint8_t, 3U> abc{'a', 'b', 'c'};
  EXPECT_EQ(core::test::sha256_for_testing(abc).hex(),
            "ba7816bf8f01cfea414140de5dae2223"
            "b00361a396177a9cb410ff61f20015ad");
}

TEST(Digest256, RendersLowercaseFixedWidthHex) {
  Digest256 digest;
  for (std::size_t index = 0U; index < digest.bytes.size(); ++index) {
    digest.bytes[index] = static_cast<std::uint8_t>(index);
  }
  EXPECT_EQ(digest.hex(),
            "000102030405060708090a0b0c0d0e0f"
            "101112131415161718191a1b1c1d1e1f");
}

TEST(CanonicalStateDigest, MatchesIndependentEmptyAndRepresentativeGoldens) {
  const BookSnapshot empty{
      .semantics_version = atlaslob_semantics_version,
      .instrument_id = instrument_id,
      .last_sequence = {},
      .sequence_exhausted = false,
      .active_order_count = 0U,
      .bids = {},
      .asks = {},
  };
  EXPECT_EQ(state_digest(empty).hex(),
            "19a8ffaeb1bee1b8aa87123c3508af1b"
            "fa87e3d634a09ba491e1b85fe597b219");
  EXPECT_EQ(state_digest(representative_snapshot()).hex(),
            "fe84a7515664b05af4f390ea77f04088"
            "3c13b2ac5ce1867f8d69b8a37ccbd16f");
}

TEST(CanonicalStateDigest, CoversEverySnapshotFieldAndFifoOrder) {
  const auto original = representative_snapshot();
  const auto digest = state_digest(original);
  const auto expect_changed = [&digest](BookSnapshot changed) {
    EXPECT_NE(state_digest(changed), digest);
  };

  auto changed = original;
  ++changed.semantics_version;
  expect_changed(changed);
  changed = original;
  changed.instrument_id = domain::InstrumentId{8U};
  expect_changed(changed);
  changed = original;
  changed.last_sequence = domain::Sequence{6U};
  expect_changed(changed);
  changed = original;
  changed.sequence_exhausted = true;
  expect_changed(changed);
  changed = original;
  ++changed.active_order_count;
  expect_changed(changed);
  changed = original;
  std::swap(changed.bids[0], changed.bids[1]);
  expect_changed(changed);
  changed = original;
  changed.bids[0].price = domain::PriceTicks{102};
  expect_changed(changed);
  changed = original;
  changed.bids[0].aggregate_quantity = domain::Quantity{8U};
  expect_changed(changed);
  changed = original;
  std::swap(changed.bids[1].orders[0], changed.bids[1].orders[1]);
  expect_changed(changed);
  changed = original;
  changed.bids[1].orders[0].order_id = domain::OrderId{9U};
  expect_changed(changed);
  changed = original;
  changed.bids[1].orders[0].client_id = domain::ClientId{99U};
  expect_changed(changed);
  changed = original;
  changed.bids[1].orders[0].instrument_id = domain::InstrumentId{8U};
  expect_changed(changed);
  changed = original;
  changed.bids[1].orders[0].side = domain::Side::sell;
  expect_changed(changed);
  changed = original;
  changed.bids[1].orders[0].price = domain::PriceTicks{99};
  expect_changed(changed);
  changed = original;
  changed.bids[1].orders[0].remaining_quantity = domain::Quantity{4U};
  expect_changed(changed);
  changed = original;
  changed.bids[1].orders[0].priority_sequence = domain::Sequence{9U};
  expect_changed(changed);
  changed = original;
  changed.asks.push_back(changed.bids.front());
  expect_changed(changed);
}

TEST(CanonicalEventDigest, CoversEveryVariantAndOptionalPayload) {
  const domain::EventBatch batch{every_event()};
  const auto original = event_digest(batch);
  EXPECT_EQ(original.hex(),
            "df99ffaa7ee15c5de7a8106be34c3e0b"
            "2d6be79d8284b3aa76d5d9f63540312e");
  EXPECT_NE(original, state_digest(representative_snapshot()));

  for (std::size_t index = 0U; index < 8U; ++index) {
    auto events = every_event();
    switch (index) {
      case 0U:
        std::get<domain::AcceptedEvent>(events[index]).command_type = domain::CommandType::cancel;
        break;
      case 1U:
        std::get<domain::RejectedEvent>(events[index]).order_id.reset();
        break;
      case 2U:
        std::get<domain::TradeEvent>(events[index]).execution_quantity = domain::Quantity{99U};
        break;
      case 3U:
        std::get<domain::RestedEvent>(events[index]).remaining_quantity = domain::Quantity{99U};
        break;
      case 4U:
        std::get<domain::CanceledEvent>(events[index]).canceled_quantity = domain::Quantity{99U};
        break;
      case 5U:
        std::get<domain::ReplacedEvent>(events[index]).new_order_id = domain::OrderId{999U};
        break;
      case 6U:
        std::get<domain::DoneEvent>(events[index]).reason = domain::DoneReason::filled;
        break;
      case 7U:
        std::get<domain::BookChangedEvent>(events[index]).best_ask = domain::TopOfBookLevel{
            .price = domain::PriceTicks{1'003},
            .aggregate_quantity = domain::Quantity{21U},
        };
        break;
      default:
        FAIL() << "unreachable event index";
    }
    EXPECT_NE(event_digest(domain::EventBatch{std::move(events)}), original);
  }
}

TEST(CanonicalEventDigest, IncludesBatchAndHeaderIdentity) {
  const domain::EventBatch first{std::vector<domain::Event>{
      domain::AcceptedEvent{
          .header = event_header(0U, domain::Sequence{1U}, domain::InstrumentId{7U}),
          .command_type = domain::CommandType::new_order,
      },
  }};
  const domain::EventBatch next_sequence{std::vector<domain::Event>{
      domain::AcceptedEvent{
          .header = event_header(0U, domain::Sequence{2U}, domain::InstrumentId{7U}),
          .command_type = domain::CommandType::new_order,
      },
  }};
  const domain::EventBatch next_instrument{std::vector<domain::Event>{
      domain::AcceptedEvent{
          .header = event_header(0U, domain::Sequence{1U}, domain::InstrumentId{8U}),
          .command_type = domain::CommandType::new_order,
      },
  }};

  EXPECT_NE(event_digest(first), event_digest(next_sequence));
  EXPECT_NE(event_digest(first), event_digest(next_instrument));
}

TEST(MatchingEngineSnapshot, FreshEngineIsCanonicalAndUnsequenced) {
  MatchingEngine engine{instrument_id};

  const BookSnapshot expected{
      .semantics_version = atlaslob_semantics_version,
      .instrument_id = instrument_id,
      .last_sequence = domain::Sequence{0U},
      .sequence_exhausted = false,
      .active_order_count = 0U,
      .bids = {},
      .asks = {},
  };

  EXPECT_EQ(engine.snapshot(), expected);
  EXPECT_EQ(engine.state_digest(), state_digest(expected));
  EXPECT_EQ(engine.state_digest().hex(),
            "19a8ffaeb1bee1b8aa87123c3508af1bfa87e3d634a09ba491e1b85fe597b219");
}

TEST(MatchingEngineSnapshot, MapsNormalAndExhaustedSequenceStateExactly) {
  EXPECT_EQ(core::snapshot_last_sequence(domain::Sequence{1U}, false), domain::Sequence{0U});
  EXPECT_EQ(core::snapshot_last_sequence(domain::Sequence{42U}, false), domain::Sequence{41U});
  EXPECT_EQ(core::snapshot_last_sequence(domain::Sequence{0U}, true),
            domain::Sequence{std::numeric_limits<std::uint64_t>::max()});
}

TEST(MatchingEngineSnapshot, IsExactBestPriceThenFifoAndDeterministic) {
  MatchingEngine first{instrument_id};
  MatchingEngine second{instrument_id};
  populate_engine(first);
  populate_engine(second);

  const auto snapshot = first.snapshot();
  EXPECT_EQ(snapshot.semantics_version, atlaslob_semantics_version);
  EXPECT_EQ(snapshot.instrument_id, instrument_id);
  EXPECT_EQ(snapshot.last_sequence, domain::Sequence{5U});
  EXPECT_FALSE(snapshot.sequence_exhausted);
  EXPECT_EQ(snapshot.active_order_count, 5U);
  ASSERT_EQ(snapshot.bids.size(), 2U);
  EXPECT_EQ(snapshot.bids[0].price, domain::PriceTicks{101});
  ASSERT_EQ(snapshot.bids[0].orders.size(), 1U);
  EXPECT_EQ(snapshot.bids[0].orders[0].order_id, domain::OrderId{2U});
  EXPECT_EQ(snapshot.bids[0].orders[0].client_id, domain::ClientId{12U});
  EXPECT_EQ(snapshot.bids[0].orders[0].instrument_id, instrument_id);
  EXPECT_EQ(snapshot.bids[0].orders[0].side, domain::Side::buy);
  EXPECT_EQ(snapshot.bids[0].orders[0].price, domain::PriceTicks{101});
  EXPECT_EQ(snapshot.bids[0].orders[0].remaining_quantity, domain::Quantity{7U});
  EXPECT_EQ(snapshot.bids[0].orders[0].priority_sequence, domain::Sequence{2U});
  EXPECT_EQ(snapshot.bids[1].price, domain::PriceTicks{100});
  EXPECT_EQ(snapshot.bids[1].aggregate_quantity, domain::Quantity{16U});
  ASSERT_EQ(snapshot.bids[1].orders.size(), 2U);
  EXPECT_EQ(snapshot.bids[1].orders[0].order_id, domain::OrderId{1U});
  EXPECT_EQ(snapshot.bids[1].orders[1].order_id, domain::OrderId{3U});
  ASSERT_EQ(snapshot.asks.size(), 2U);
  EXPECT_EQ(snapshot.asks[0].price, domain::PriceTicks{109});
  EXPECT_EQ(snapshot.asks[0].orders[0].order_id, domain::OrderId{5U});
  EXPECT_EQ(snapshot.asks[1].price, domain::PriceTicks{110});
  EXPECT_EQ(snapshot.asks[1].orders[0].order_id, domain::OrderId{4U});

  EXPECT_EQ(second.snapshot(), snapshot);
  EXPECT_EQ(first.state_digest(), second.state_digest());
  EXPECT_EQ(first.state_digest(), atlaslob::state_digest(snapshot));
  EXPECT_EQ(first.state_digest(), first.state_digest());
}

TEST(MatchingEngineSnapshot, RejectionAdvancesCanonicalSequenceOnly) {
  MatchingEngine engine{instrument_id};
  ASSERT_TRUE(engine.execute(limit_order(1U, 11U, domain::Side::buy, 100, 5U)));
  const auto before = engine.snapshot();
  const auto before_digest = engine.state_digest();

  auto invalid = limit_order(2U, 12U, domain::Side::buy, 101, 0U);
  const auto rejected = engine.execute(invalid);
  ASSERT_TRUE(rejected.rejected());
  const auto after = engine.snapshot();

  auto expected = before;
  expected.last_sequence = domain::Sequence{2U};
  EXPECT_EQ(after, expected);
  EXPECT_NE(engine.state_digest(), before_digest);
  EXPECT_EQ(event_digest(*rejected.batch), event_digest(*rejected.batch));
}

}  // namespace
