#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "atlaslob/domain/event_batch.hpp"

namespace {

using namespace atlaslob::domain;

static_assert(!std::is_default_constructible_v<EventBatch>);
static_assert(!std::is_copy_constructible_v<EventBatch>);
static_assert(!std::is_copy_assignable_v<EventBatch>);
static_assert(std::is_nothrow_move_constructible_v<EventBatch>);
static_assert(std::is_nothrow_move_assignable_v<EventBatch>);
static_assert(
    std::is_same_v<decltype(std::declval<const EventBatch&>().events()), std::span<const Event>>);
static_assert(std::is_same_v<decltype(std::declval<const EventBatch&>()[0U]), const Event&>);

EventHeader batch_header(std::uint32_t event_index, Sequence sequence = Sequence{41U},
                         InstrumentId instrument = InstrumentId{7U}) {
  return {
      .command_sequence = sequence,
      .event_index = event_index,
      .instrument_id = instrument,
  };
}

TEST(EventHeaderAccess, ReturnsMutableAndConstHeadersForEveryVariant) {
  std::vector<Event> events{
      AcceptedEvent{}, RejectedEvent{}, TradeEvent{}, RestedEvent{},
      CanceledEvent{}, ReplacedEvent{}, DoneEvent{},  BookChangedEvent{},
  };

  for (std::size_t index = 0; index < events.size(); ++index) {
    event_header(events[index]) = batch_header(static_cast<std::uint32_t>(index));
    const auto& const_event = std::as_const(events[index]);
    const auto& header = event_header(const_event);
    EXPECT_EQ(header.command_sequence, Sequence{41U});
    EXPECT_EQ(header.event_index, static_cast<std::uint32_t>(index));
    EXPECT_EQ(header.instrument_id, InstrumentId{7U});
  }
}

TEST(EventBatch, OwnsAnImmutableContiguousSequenceAndMovesOwnership) {
  std::vector<Event> source{
      AcceptedEvent{
          .header = batch_header(0U),
          .command_type = CommandType::new_order,
      },
      DoneEvent{
          .header = batch_header(1U),
          .order_id = OrderId{91U},
          .reason = DoneReason::filled,
          .remaining_quantity = Quantity{},
      },
  };

  EventBatch batch{std::move(source)};
  EXPECT_EQ(batch.command_sequence(), Sequence{41U});
  EXPECT_EQ(batch.instrument_id(), InstrumentId{7U});
  EXPECT_EQ(batch.size(), 2U);
  EXPECT_FALSE(batch.empty());
  ASSERT_EQ(batch.events().size(), 2U);
  EXPECT_EQ(event_type(batch[0]), EventType::accepted);
  EXPECT_EQ(event_type(batch.at(1U)), EventType::done);
  EXPECT_THROW(static_cast<void>(batch.at(2U)), std::out_of_range);

  EventBatch moved{std::move(batch)};
  EXPECT_EQ(moved.command_sequence(), Sequence{41U});
  EXPECT_EQ(moved.instrument_id(), InstrumentId{7U});
  ASSERT_EQ(moved.size(), 2U);
  EXPECT_EQ(std::get<AcceptedEvent>(moved[0]).command_type, CommandType::new_order);
  EXPECT_EQ(std::get<DoneEvent>(moved[1]).order_id, OrderId{91U});

  EventBatch assigned{std::vector<Event>{AcceptedEvent{
      .header = batch_header(0U, Sequence{99U}),
      .command_type = CommandType::cancel,
  }}};
  assigned = std::move(moved);
  EXPECT_EQ(assigned.command_sequence(), Sequence{41U});
  EXPECT_EQ(assigned.instrument_id(), InstrumentId{7U});
  EXPECT_EQ(assigned.size(), 2U);
}

TEST(EventBatch, RejectsEmptyOrInvalidBatchHeaders) {
  EXPECT_THROW(static_cast<void>(EventBatch{std::vector<Event>{}}), std::invalid_argument);

  EXPECT_THROW(static_cast<void>(EventBatch{std::vector<Event>{AcceptedEvent{
                   .header = batch_header(0U, Sequence{}),
                   .command_type = CommandType::new_order,
               }}}),
               std::invalid_argument);

  EXPECT_THROW(static_cast<void>(EventBatch{std::vector<Event>{AcceptedEvent{
                   .header = batch_header(0U, Sequence{41U}, InstrumentId{}),
                   .command_type = CommandType::new_order,
               }}}),
               std::invalid_argument);

  EventBatch zero_instrument_rejection{std::vector<Event>{RejectedEvent{
      .header = batch_header(0U, Sequence{41U}, InstrumentId{}),
      .command_type = CommandType::new_order,
      .reason = RejectReason::invalid_instrument_id,
      .order_id = OrderId{1U},
  }}};
  EXPECT_EQ(zero_instrument_rejection.instrument_id(), InstrumentId{});
  EXPECT_EQ(event_type(zero_instrument_rejection[0]), EventType::rejected);

  EXPECT_THROW(static_cast<void>(EventBatch{std::vector<Event>{
                   RejectedEvent{
                       .header = batch_header(0U, Sequence{41U}, InstrumentId{}),
                       .command_type = CommandType::new_order,
                       .reason = RejectReason::invalid_instrument_id,
                   },
                   RejectedEvent{
                       .header = batch_header(1U, Sequence{41U}, InstrumentId{}),
                       .command_type = CommandType::cancel,
                       .reason = RejectReason::invalid_instrument_id,
                   },
               }}),
               std::invalid_argument);

  EXPECT_THROW(
      static_cast<void>(EventBatch{std::vector<Event>{
          AcceptedEvent{.header = batch_header(0U), .command_type = CommandType::new_order},
          DoneEvent{
              .header = batch_header(2U),
              .order_id = OrderId{1U},
              .reason = DoneReason::filled,
          },
      }}),
      std::invalid_argument);

  EXPECT_THROW(
      static_cast<void>(EventBatch{std::vector<Event>{
          AcceptedEvent{.header = batch_header(0U), .command_type = CommandType::new_order},
          DoneEvent{
              .header = batch_header(1U, Sequence{42U}),
              .order_id = OrderId{1U},
              .reason = DoneReason::filled,
          },
      }}),
      std::invalid_argument);

  EXPECT_THROW(
      static_cast<void>(EventBatch{std::vector<Event>{
          AcceptedEvent{.header = batch_header(0U), .command_type = CommandType::new_order},
          DoneEvent{
              .header = batch_header(1U, Sequence{41U}, InstrumentId{8U}),
              .order_id = OrderId{1U},
              .reason = DoneReason::filled,
          },
      }}),
      std::invalid_argument);
}

}  // namespace
