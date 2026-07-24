#include "event_batch_builder.hpp"

#include <limits>
#include <stdexcept>

namespace atlaslob::core {
namespace {

[[nodiscard]] std::size_t validated_event_count(domain::Sequence command_sequence,
                                                domain::InstrumentId instrument_id,
                                                std::size_t expected_event_count) {
  if (command_sequence.value() == 0U) {
    throw std::invalid_argument{"event batch builder command sequence must be nonzero"};
  }
  if (expected_event_count == 0U) {
    throw std::invalid_argument{"event batch builder requires at least one event slot"};
  }
  if (instrument_id.value() == 0U && expected_event_count != 1U) {
    throw std::invalid_argument{
        "zero-instrument event batch builder requires exactly one event slot"};
  }
  if (expected_event_count - 1U > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error{"event batch builder event count exceeds index range"};
  }
  return expected_event_count;
}

}  // namespace

EventBatchBuilder::EventBatchBuilder(domain::Sequence command_sequence,
                                     domain::InstrumentId instrument_id,
                                     std::size_t expected_event_count)
    : command_sequence_{command_sequence},
      instrument_id_{instrument_id},
      events_{validated_event_count(command_sequence, instrument_id, expected_event_count)} {}

EventBatchBuilderError EventBatchBuilder::append(domain::AcceptedEvent event) & noexcept {
  return append_event(domain::Event{std::move(event)});
}

EventBatchBuilderError EventBatchBuilder::append(domain::RejectedEvent event) & noexcept {
  return append_event(domain::Event{std::move(event)});
}

EventBatchBuilderError EventBatchBuilder::append(domain::TradeEvent event) & noexcept {
  return append_event(domain::Event{std::move(event)});
}

EventBatchBuilderError EventBatchBuilder::append(domain::RestedEvent event) & noexcept {
  return append_event(domain::Event{std::move(event)});
}

EventBatchBuilderError EventBatchBuilder::append(domain::CanceledEvent event) & noexcept {
  return append_event(domain::Event{std::move(event)});
}

EventBatchBuilderError EventBatchBuilder::append(domain::ReplacedEvent event) & noexcept {
  return append_event(domain::Event{std::move(event)});
}

EventBatchBuilderError EventBatchBuilder::append(domain::DoneEvent event) & noexcept {
  return append_event(domain::Event{std::move(event)});
}

EventBatchBuilderError EventBatchBuilder::append(domain::BookChangedEvent event) & noexcept {
  return append_event(domain::Event{std::move(event)});
}

EventBatchBuilderError EventBatchBuilder::append_event(domain::Event event) noexcept {
  if (finished_) {
    return EventBatchBuilderError::builder_finished;
  }
  if (error_ != EventBatchBuilderError::none) {
    return error_;
  }
  if (appended_event_count_ == events_.size()) {
    error_ = EventBatchBuilderError::capacity_overflow;
    return error_;
  }
  if (instrument_id_.value() == 0U &&
      (events_.size() != 1U || domain::event_type(event) != domain::EventType::rejected)) {
    error_ = EventBatchBuilderError::invalid_zero_instrument_batch;
    return error_;
  }

  auto& header = domain::event_header(event);
  header.command_sequence = command_sequence_;
  header.event_index = static_cast<std::uint32_t>(appended_event_count_);
  header.instrument_id = instrument_id_;
  events_[appended_event_count_] = std::move(event);
  ++appended_event_count_;
  return EventBatchBuilderError::none;
}

EventBatchBuilderResult EventBatchBuilder::finish() && noexcept {
  if (finished_) {
    return {
        .batch = std::nullopt,
        .error = EventBatchBuilderError::builder_finished,
    };
  }
  finished_ = true;

  if (error_ != EventBatchBuilderError::none) {
    return {
        .batch = std::nullopt,
        .error = error_,
    };
  }
  if (appended_event_count_ != events_.size()) {
    error_ = EventBatchBuilderError::capacity_underfill;
    return {
        .batch = std::nullopt,
        .error = error_,
    };
  }

  EventBatchBuilderResult result{};
  result.batch.emplace(std::move(events_));
  return result;
}

}  // namespace atlaslob::core
