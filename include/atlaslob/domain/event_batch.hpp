#pragma once

#include <cstddef>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "atlaslob/domain/events.hpp"

namespace atlaslob::domain {

class EventBatch final {
 public:
  explicit EventBatch(std::vector<Event> events) : events_{std::move(events)} {
    validate_or_throw();
  }

  EventBatch(const EventBatch&) = delete;
  EventBatch& operator=(const EventBatch&) = delete;
  // As with standard-library containers, a moved-from batch is valid only for destruction or
  // assignment; its observer values are otherwise unspecified.
  EventBatch(EventBatch&&) noexcept = default;
  EventBatch& operator=(EventBatch&&) noexcept = default;
  ~EventBatch() = default;

  [[nodiscard]] Sequence command_sequence() const noexcept { return command_sequence_; }
  [[nodiscard]] InstrumentId instrument_id() const noexcept { return instrument_id_; }
  [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }
  [[nodiscard]] bool empty() const noexcept { return events_.empty(); }

  [[nodiscard]] std::span<const Event> events() const noexcept { return events_; }
  [[nodiscard]] const Event& operator[](std::size_t index) const noexcept { return events_[index]; }
  [[nodiscard]] const Event& at(std::size_t index) const { return events_.at(index); }

 private:
  void validate_or_throw() {
    if (events_.empty()) {
      throw std::invalid_argument{"event batch must contain at least one event"};
    }

    command_sequence_ = event_header(events_.front()).command_sequence;
    instrument_id_ = event_header(events_.front()).instrument_id;
    if (command_sequence_.value() == 0U) {
      throw std::invalid_argument{"event batch command sequence must be nonzero"};
    }
    // A structurally invalid command can carry InstrumentId{0}. Preserve that submitted value in
    // its audit event instead of substituting a routed book ID. Zero is therefore valid only for
    // the canonical one-event rejection batch; accepted processing always has a nonzero route.
    if (instrument_id_.value() == 0U &&
        (events_.size() != 1U || event_type(events_.front()) != EventType::rejected)) {
      throw std::invalid_argument{"zero-instrument event batch must contain exactly one rejection"};
    }

    for (std::size_t index = 0; index < events_.size(); ++index) {
      const auto& header = event_header(events_[index]);
      if (header.command_sequence != command_sequence_) {
        throw std::invalid_argument{"event batch command sequences must match"};
      }
      if (header.instrument_id != instrument_id_) {
        throw std::invalid_argument{"event batch instrument IDs must match"};
      }
      if (static_cast<std::size_t>(header.event_index) != index) {
        throw std::invalid_argument{"event batch indices must be contiguous from zero"};
      }
    }
  }

  Sequence command_sequence_{};
  InstrumentId instrument_id_{};
  std::vector<Event> events_;
};

}  // namespace atlaslob::domain
