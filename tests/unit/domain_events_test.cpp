#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <variant>

#include "atlaslob/domain/events.hpp"

namespace {

using namespace atlaslob::domain;

static_assert(std::variant_size_v<Event> == 8U);

EventHeader header(std::uint32_t event_index) {
  return {
      .command_sequence = Sequence{41U},
      .event_index = event_index,
      .instrument_id = InstrumentId{7U},
  };
}

TEST(EventVocabulary, DerivesEveryVariantTypeAndPreservesEveryPayload) {
  const AcceptedEvent accepted{
      .header = header(0U),
      .command_type = CommandType::new_order,
  };
  const RejectedEvent rejected{
      .header = header(1U),
      .command_type = CommandType::cancel,
      .reason = RejectReason::unknown_order_id,
      .order_id = OrderId{101U},
  };
  const TradeEvent trade{
      .header = header(2U),
      .aggressor_order_id = OrderId{200U},
      .resting_order_id = OrderId{100U},
      .aggressor_client_id = ClientId{20U},
      .resting_client_id = ClientId{10U},
      .aggressor_side = Side::buy,
      .execution_price = PriceTicks{10'100},
      .execution_quantity = Quantity{4U},
      .aggressor_remaining = Quantity{6U},
      .resting_remaining = Quantity{0U},
  };
  const RestedEvent rested{
      .header = header(3U),
      .order_id = OrderId{200U},
      .client_id = ClientId{20U},
      .side = Side::buy,
      .price = PriceTicks{10'100},
      .remaining_quantity = Quantity{6U},
  };
  const CanceledEvent canceled{
      .header = header(4U),
      .order_id = OrderId{100U},
      .canceled_quantity = Quantity{3U},
  };
  const ReplacedEvent replaced{
      .header = header(5U),
      .old_order_id = OrderId{100U},
      .new_order_id = OrderId{201U},
  };
  const DoneEvent done{
      .header = header(6U),
      .order_id = OrderId{100U},
      .reason = DoneReason::filled,
      .remaining_quantity = Quantity{0U},
  };
  const BookChangedEvent book_changed{
      .header = header(7U),
      .best_bid =
          TopOfBookLevel{
              .price = PriceTicks{10'099},
              .aggregate_quantity = Quantity{12U},
          },
      .best_ask =
          TopOfBookLevel{
              .price = PriceTicks{10'101},
              .aggregate_quantity = Quantity{8U},
          },
  };

  const std::array<Event, 8U> events{
      accepted, rejected, trade, rested, canceled, replaced, done, book_changed,
  };

  const std::array<EventType, 8U> expected{
      EventType::accepted, EventType::rejected, EventType::trade, EventType::rested,
      EventType::canceled, EventType::replaced, EventType::done,  EventType::book_changed,
  };

  for (std::size_t index = 0; index < events.size(); ++index) {
    EXPECT_EQ(event_type(events[index]), expected[index]);
    std::visit(
        [index](const auto& value) {
          EXPECT_EQ(value.header.command_sequence, Sequence{41U});
          EXPECT_EQ(value.header.event_index, static_cast<std::uint32_t>(index));
          EXPECT_EQ(value.header.instrument_id, InstrumentId{7U});
        },
        events[index]);
  }

  EXPECT_EQ(std::get<AcceptedEvent>(events[0]), accepted);
  EXPECT_EQ(std::get<RejectedEvent>(events[1]), rejected);
  EXPECT_EQ(std::get<TradeEvent>(events[2]), trade);
  EXPECT_EQ(std::get<RestedEvent>(events[3]), rested);
  EXPECT_EQ(std::get<CanceledEvent>(events[4]), canceled);
  EXPECT_EQ(std::get<ReplacedEvent>(events[5]), replaced);
  EXPECT_EQ(std::get<DoneEvent>(events[6]), done);
  EXPECT_EQ(std::get<BookChangedEvent>(events[7]), book_changed);
}

TEST(EventVocabulary, RepresentsAnEmptySideWithoutAnOrphanQuantity) {
  const BookChangedEvent event{
      .header = header(0U),
      .best_bid = std::nullopt,
      .best_ask =
          TopOfBookLevel{
              .price = PriceTicks{10'101},
              .aggregate_quantity = Quantity{8U},
          },
  };

  EXPECT_FALSE(event.best_bid.has_value());
  ASSERT_TRUE(event.best_ask.has_value());
  EXPECT_EQ(event.best_ask->price, PriceTicks{10'101});
  EXPECT_EQ(event.best_ask->aggregate_quantity, Quantity{8U});
}

TEST(EventVocabulary, StringRepresentationsAreStable) {
  EXPECT_EQ(to_string(CommandType::new_order), "new");
  EXPECT_EQ(to_string(EventType::book_changed), "book_changed");
  EXPECT_EQ(to_string(DoneReason::ioc_residual_canceled), "ioc_residual_canceled");
  EXPECT_EQ(to_string(RejectReason::invalid_replacement_id), "invalid_replacement_id");
}

}  // namespace
