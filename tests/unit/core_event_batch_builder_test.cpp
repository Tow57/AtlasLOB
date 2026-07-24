#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "event_batch_builder.hpp"
#include "top_of_book.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::core;

constexpr domain::Sequence command_sequence{71U};
constexpr domain::InstrumentId instrument_id{11U};

static_assert(!std::is_copy_constructible_v<EventBatchBuilder>);
static_assert(!std::is_move_constructible_v<EventBatchBuilder>);
static_assert(!std::is_copy_constructible_v<EventBatchBuilderResult>);
static_assert(std::is_move_constructible_v<EventBatchBuilderResult>);
static_assert(noexcept(std::declval<EventBatchBuilder&>().append(domain::AcceptedEvent{})));
static_assert(noexcept(std::declval<EventBatchBuilder&&>().finish()));

domain::EventHeader wrong_header() {
  return {
      .command_sequence = domain::Sequence{999U},
      .event_index = 999U,
      .instrument_id = domain::InstrumentId{999U},
  };
}

OrderNodeSpec order_spec(std::uint64_t order_id, domain::Side side, std::int64_t price,
                         std::uint64_t quantity, std::uint64_t priority) {
  return {
      .order_id = domain::OrderId{order_id},
      .client_id = domain::ClientId{7U},
      .instrument_id = instrument_id,
      .side = side,
      .price = domain::PriceTicks{price},
      .remaining_quantity = domain::Quantity{quantity},
      .priority_sequence = domain::Sequence{priority},
  };
}

TEST(EventBatchBuilder, PreallocatesExactSlotsAndStampsEveryEventKind) {
  EventBatchBuilder builder{command_sequence, instrument_id, 8U};
  EXPECT_EQ(builder.command_sequence(), command_sequence);
  EXPECT_EQ(builder.instrument_id(), instrument_id);
  EXPECT_EQ(builder.expected_event_count(), 8U);
  EXPECT_EQ(builder.appended_event_count(), 0U);

  EXPECT_EQ(builder.append(domain::AcceptedEvent{
                .header = wrong_header(),
                .command_type = domain::CommandType::new_order,
            }),
            EventBatchBuilderError::none);
  EXPECT_EQ(builder.append(domain::RejectedEvent{
                .header = wrong_header(),
                .command_type = domain::CommandType::cancel,
                .reason = domain::RejectReason::unknown_order_id,
                .order_id = domain::OrderId{100U},
            }),
            EventBatchBuilderError::none);
  EXPECT_EQ(builder.append(domain::TradeEvent{
                .header = wrong_header(),
                .aggressor_order_id = domain::OrderId{200U},
                .resting_order_id = domain::OrderId{100U},
                .aggressor_client_id = domain::ClientId{20U},
                .resting_client_id = domain::ClientId{10U},
                .aggressor_side = domain::Side::buy,
                .execution_price = domain::PriceTicks{10'100},
                .execution_quantity = domain::Quantity{4U},
                .aggressor_remaining = domain::Quantity{6U},
                .resting_remaining = domain::Quantity{},
            }),
            EventBatchBuilderError::none);
  EXPECT_EQ(builder.append(domain::RestedEvent{
                .header = wrong_header(),
                .order_id = domain::OrderId{200U},
                .client_id = domain::ClientId{20U},
                .side = domain::Side::buy,
                .price = domain::PriceTicks{10'100},
                .remaining_quantity = domain::Quantity{6U},
            }),
            EventBatchBuilderError::none);
  EXPECT_EQ(builder.append(domain::CanceledEvent{
                .header = wrong_header(),
                .order_id = domain::OrderId{100U},
                .canceled_quantity = domain::Quantity{3U},
            }),
            EventBatchBuilderError::none);
  EXPECT_EQ(builder.append(domain::ReplacedEvent{
                .header = wrong_header(),
                .old_order_id = domain::OrderId{100U},
                .new_order_id = domain::OrderId{201U},
            }),
            EventBatchBuilderError::none);
  EXPECT_EQ(builder.append(domain::DoneEvent{
                .header = wrong_header(),
                .order_id = domain::OrderId{100U},
                .reason = domain::DoneReason::filled,
                .remaining_quantity = domain::Quantity{},
            }),
            EventBatchBuilderError::none);
  EXPECT_EQ(builder.append(domain::BookChangedEvent{
                .header = wrong_header(),
                .best_bid =
                    domain::TopOfBookLevel{
                        .price = domain::PriceTicks{10'099},
                        .aggregate_quantity = domain::Quantity{12U},
                    },
                .best_ask =
                    domain::TopOfBookLevel{
                        .price = domain::PriceTicks{10'101},
                        .aggregate_quantity = domain::Quantity{8U},
                    },
            }),
            EventBatchBuilderError::none);
  EXPECT_EQ(builder.appended_event_count(), 8U);

  auto result = std::move(builder).finish();
  ASSERT_TRUE(result);
  ASSERT_TRUE(result.batch.has_value());
  EXPECT_EQ(result.error, EventBatchBuilderError::none);
  const auto& batch = *result.batch;
  EXPECT_EQ(batch.size(), 8U);

  constexpr std::array expected_types{
      domain::EventType::accepted, domain::EventType::rejected,     domain::EventType::trade,
      domain::EventType::rested,   domain::EventType::canceled,     domain::EventType::replaced,
      domain::EventType::done,     domain::EventType::book_changed,
  };
  for (std::size_t index = 0; index < batch.size(); ++index) {
    const auto& header = domain::event_header(batch[index]);
    EXPECT_EQ(header.command_sequence, command_sequence);
    EXPECT_EQ(header.event_index, static_cast<std::uint32_t>(index));
    EXPECT_EQ(header.instrument_id, instrument_id);
    EXPECT_EQ(domain::event_type(batch[index]), expected_types[index]);
  }

  EXPECT_EQ(std::get<domain::TradeEvent>(batch[2]).execution_quantity, domain::Quantity{4U});
  EXPECT_EQ(std::get<domain::BookChangedEvent>(batch[7]).best_ask->aggregate_quantity,
            domain::Quantity{8U});
}

TEST(EventBatchBuilder, RejectsInvalidConstructionBeforeAnySlotsAreUsable) {
  EXPECT_THROW(static_cast<void>(EventBatchBuilder{{}, instrument_id, 1U}), std::invalid_argument);
  EXPECT_THROW(static_cast<void>(EventBatchBuilder{command_sequence, instrument_id, 0U}),
               std::invalid_argument);

  if constexpr (std::numeric_limits<std::size_t>::max() >
                std::numeric_limits<std::uint32_t>::max()) {
    constexpr auto too_many =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 2U;
    EXPECT_THROW(static_cast<void>(EventBatchBuilder{command_sequence, instrument_id, too_many}),
                 std::length_error);
  }
}

TEST(EventBatchBuilder, PreservesZeroInstrumentOnlyForASingleRejectedCommand) {
  EventBatchBuilder rejected{command_sequence, {}, 1U};
  ASSERT_EQ(rejected.append(domain::RejectedEvent{
                .command_type = domain::CommandType::new_order,
                .reason = domain::RejectReason::invalid_instrument_id,
                .order_id = domain::OrderId{8U},
            }),
            EventBatchBuilderError::none);
  auto rejected_result = std::move(rejected).finish();
  ASSERT_TRUE(rejected_result);
  EXPECT_EQ(rejected_result.batch->instrument_id(), domain::InstrumentId{});
  EXPECT_EQ(domain::event_type((*rejected_result.batch)[0]), domain::EventType::rejected);

  EventBatchBuilder accepted{command_sequence, {}, 1U};
  EXPECT_EQ(accepted.append(domain::AcceptedEvent{
                .command_type = domain::CommandType::new_order,
            }),
            EventBatchBuilderError::invalid_zero_instrument_batch);
  auto accepted_result = std::move(accepted).finish();
  EXPECT_FALSE(accepted_result);
  EXPECT_EQ(accepted_result.error, EventBatchBuilderError::invalid_zero_instrument_batch);

  EXPECT_THROW(static_cast<void>(EventBatchBuilder{command_sequence, {}, 2U}),
               std::invalid_argument);
}

TEST(EventBatchBuilder, ReportsUnderfillWithoutPublishingAPartialBatch) {
  EventBatchBuilder builder{command_sequence, instrument_id, 2U};
  ASSERT_EQ(builder.append(domain::AcceptedEvent{
                .command_type = domain::CommandType::cancel,
            }),
            EventBatchBuilderError::none);

  auto result = std::move(builder).finish();
  EXPECT_FALSE(result);
  EXPECT_FALSE(result.batch.has_value());
  EXPECT_EQ(result.error, EventBatchBuilderError::capacity_underfill);
  EXPECT_EQ(builder.error(), EventBatchBuilderError::capacity_underfill);
  EXPECT_EQ(builder.append(domain::DoneEvent{}), EventBatchBuilderError::builder_finished);

  auto second_finish = std::move(builder).finish();
  EXPECT_FALSE(second_finish);
  EXPECT_EQ(second_finish.error, EventBatchBuilderError::builder_finished);
}

TEST(EventBatchBuilder, ReportsOverflowWithoutOverwritingThePlannedSlot) {
  EventBatchBuilder builder{command_sequence, instrument_id, 1U};
  ASSERT_EQ(builder.append(domain::AcceptedEvent{
                .command_type = domain::CommandType::new_order,
            }),
            EventBatchBuilderError::none);
  EXPECT_EQ(builder.append(domain::DoneEvent{
                .order_id = domain::OrderId{8U},
                .reason = domain::DoneReason::filled,
            }),
            EventBatchBuilderError::capacity_overflow);
  EXPECT_EQ(builder.error(), EventBatchBuilderError::capacity_overflow);
  EXPECT_EQ(builder.appended_event_count(), 1U);

  auto result = std::move(builder).finish();
  EXPECT_FALSE(result);
  EXPECT_FALSE(result.batch.has_value());
  EXPECT_EQ(result.error, EventBatchBuilderError::capacity_overflow);
}

TEST(EventBatchBuilder, FinishesAndMovesASingleOwnedEvent) {
  EventBatchBuilder builder{command_sequence, instrument_id, 1U};
  ASSERT_EQ(builder.append(domain::DoneEvent{
                .order_id = domain::OrderId{8U},
                .reason = domain::DoneReason::canceled,
                .remaining_quantity = domain::Quantity{5U},
            }),
            EventBatchBuilderError::none);

  auto result = std::move(builder).finish();
  ASSERT_TRUE(result);
  domain::EventBatch batch = std::move(*result.batch);
  ASSERT_EQ(batch.size(), 1U);
  EXPECT_EQ(std::get<domain::DoneEvent>(batch[0]).order_id, domain::OrderId{8U});
  EXPECT_EQ(batch.command_sequence(), command_sequence);
  EXPECT_EQ(batch.instrument_id(), instrument_id);
}

TEST(EventBatchBuilderErrorVocabulary, HasStableStringRepresentations) {
  EXPECT_EQ(to_string(EventBatchBuilderError::capacity_overflow), "capacity_overflow");
  EXPECT_EQ(to_string(EventBatchBuilderError::capacity_underfill), "capacity_underfill");
  EXPECT_EQ(to_string(EventBatchBuilderError::builder_finished), "builder_finished");
  EXPECT_EQ(to_string(EventBatchBuilderError::invalid_zero_instrument_batch),
            "invalid_zero_instrument_batch");
  EXPECT_EQ(to_string(static_cast<EventBatchBuilderError>(255U)), "unknown");
}

TEST(TopOfBookSnapshot, CapturesEmptyOneSidedAndTwoSidedBooks) {
  InstrumentBook book{instrument_id};
  const TopOfBookSnapshot empty{};
  EXPECT_EQ(snapshot_top_of_book(book), empty);

  ASSERT_TRUE(book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U)));
  const auto bid_only = snapshot_top_of_book(book);
  EXPECT_EQ(bid_only.best_bid, (domain::TopOfBookLevel{
                                   .price = domain::PriceTicks{10'000},
                                   .aggregate_quantity = domain::Quantity{5U},
                               }));
  EXPECT_EQ(bid_only.best_ask, std::nullopt);

  ASSERT_TRUE(book.rest(order_spec(2U, domain::Side::sell, 10'200, 7U, 2U)));
  const auto both = snapshot_top_of_book(book);
  EXPECT_EQ(both.best_bid, bid_only.best_bid);
  EXPECT_EQ(both.best_ask, (domain::TopOfBookLevel{
                               .price = domain::PriceTicks{10'200},
                               .aggregate_quantity = domain::Quantity{7U},
                           }));
}

TEST(TopOfBookSnapshot, IgnoresNonbestChangesAndDetectsBestAggregateChanges) {
  InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(order_spec(1U, domain::Side::buy, 10'000, 5U, 1U)));
  ASSERT_TRUE(book.rest(order_spec(2U, domain::Side::sell, 10'200, 7U, 2U)));
  const auto original = snapshot_top_of_book(book);

  ASSERT_TRUE(book.rest(order_spec(3U, domain::Side::buy, 9'900, 11U, 3U)));
  EXPECT_EQ(snapshot_top_of_book(book), original);

  ASSERT_TRUE(book.rest(order_spec(4U, domain::Side::buy, 10'000, 3U, 4U)));
  const auto aggregate_changed = snapshot_top_of_book(book);
  EXPECT_NE(aggregate_changed, original);
  EXPECT_EQ(aggregate_changed.best_bid, (domain::TopOfBookLevel{
                                            .price = domain::PriceTicks{10'000},
                                            .aggregate_quantity = domain::Quantity{8U},
                                        }));
  EXPECT_EQ(aggregate_changed.best_ask, original.best_ask);
}

}  // namespace
