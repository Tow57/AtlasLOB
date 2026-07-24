#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "atlaslob/domain/event_batch.hpp"

namespace atlaslob::core {

enum class EventBatchBuilderError : std::uint8_t {
  none = 0,
  capacity_overflow = 1,
  capacity_underfill = 2,
  builder_finished = 3,
  invalid_zero_instrument_batch = 4,
};

[[nodiscard]] constexpr std::string_view to_string(EventBatchBuilderError error) noexcept {
  switch (error) {
    case EventBatchBuilderError::none:
      return "none";
    case EventBatchBuilderError::capacity_overflow:
      return "capacity_overflow";
    case EventBatchBuilderError::capacity_underfill:
      return "capacity_underfill";
    case EventBatchBuilderError::builder_finished:
      return "builder_finished";
    case EventBatchBuilderError::invalid_zero_instrument_batch:
      return "invalid_zero_instrument_batch";
  }
  return "unknown";
}

struct EventBatchBuilderResult final {
  // A moved-from result is valid only for destruction or assignment; its observers are otherwise
  // unspecified because std::optional keeps a moved-from contained object engaged.
  std::optional<domain::EventBatch> batch{};
  EventBatchBuilderError error{EventBatchBuilderError::none};

  [[nodiscard]] bool has_value() const noexcept {
    return batch.has_value() && error == EventBatchBuilderError::none;
  }

  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
};

class EventBatchBuilder final {
 public:
  EventBatchBuilder(domain::Sequence command_sequence, domain::InstrumentId instrument_id,
                    std::size_t expected_event_count);

  EventBatchBuilder(const EventBatchBuilder&) = delete;
  EventBatchBuilder& operator=(const EventBatchBuilder&) = delete;
  EventBatchBuilder(EventBatchBuilder&&) = delete;
  EventBatchBuilder& operator=(EventBatchBuilder&&) = delete;
  ~EventBatchBuilder() = default;

  [[nodiscard]] domain::Sequence command_sequence() const noexcept { return command_sequence_; }
  [[nodiscard]] domain::InstrumentId instrument_id() const noexcept { return instrument_id_; }
  [[nodiscard]] std::size_t expected_event_count() const noexcept { return events_.size(); }
  [[nodiscard]] std::size_t appended_event_count() const noexcept { return appended_event_count_; }
  [[nodiscard]] EventBatchBuilderError error() const noexcept { return error_; }

  [[nodiscard]] EventBatchBuilderError append(domain::AcceptedEvent event) & noexcept;
  [[nodiscard]] EventBatchBuilderError append(domain::RejectedEvent event) & noexcept;
  [[nodiscard]] EventBatchBuilderError append(domain::TradeEvent event) & noexcept;
  [[nodiscard]] EventBatchBuilderError append(domain::RestedEvent event) & noexcept;
  [[nodiscard]] EventBatchBuilderError append(domain::CanceledEvent event) & noexcept;
  [[nodiscard]] EventBatchBuilderError append(domain::ReplacedEvent event) & noexcept;
  [[nodiscard]] EventBatchBuilderError append(domain::DoneEvent event) & noexcept;
  [[nodiscard]] EventBatchBuilderError append(domain::BookChangedEvent event) & noexcept;

  [[nodiscard]] EventBatchBuilderResult finish() && noexcept;

 private:
  [[nodiscard]] EventBatchBuilderError append_event(domain::Event event) noexcept;

  domain::Sequence command_sequence_{};
  domain::InstrumentId instrument_id_{};
  std::vector<domain::Event> events_;
  std::size_t appended_event_count_{};
  EventBatchBuilderError error_{EventBatchBuilderError::none};
  bool finished_{};
};

static_assert(std::is_nothrow_move_constructible_v<domain::Event>);
static_assert(std::is_nothrow_move_assignable_v<domain::Event>);
static_assert(std::is_nothrow_constructible_v<domain::Event, domain::AcceptedEvent&&>);
static_assert(std::is_nothrow_constructible_v<domain::Event, domain::RejectedEvent&&>);
static_assert(std::is_nothrow_constructible_v<domain::Event, domain::TradeEvent&&>);
static_assert(std::is_nothrow_constructible_v<domain::Event, domain::RestedEvent&&>);
static_assert(std::is_nothrow_constructible_v<domain::Event, domain::CanceledEvent&&>);
static_assert(std::is_nothrow_constructible_v<domain::Event, domain::ReplacedEvent&&>);
static_assert(std::is_nothrow_constructible_v<domain::Event, domain::DoneEvent&&>);
static_assert(std::is_nothrow_constructible_v<domain::Event, domain::BookChangedEvent&&>);

}  // namespace atlaslob::core
