#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "atlaslob/book_snapshot.hpp"
#include "atlaslob/digest.hpp"
#include "atlaslob/domain/commands.hpp"
#include "atlaslob/domain/event_batch.hpp"
#include "atlaslob/domain/events.hpp"

namespace atlaslob {

struct MatchingEngineConfig final {
  domain::Quantity max_order_quantity{std::numeric_limits<std::uint64_t>::max()};
  domain::PriceTicks tick_increment{1};
  std::size_t max_active_orders{std::numeric_limits<std::size_t>::max()};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return max_order_quantity.value() != 0U && tick_increment.value() > 0;
  }

  bool operator==(const MatchingEngineConfig&) const = default;
};

enum class EngineError : std::uint8_t {
  none = 0,
  sequence_exhausted = 1,
  internal_failure = 2,
};

[[nodiscard]] constexpr std::string_view to_string(EngineError error) noexcept {
  switch (error) {
    case EngineError::none:
      return "none";
    case EngineError::sequence_exhausted:
      return "sequence_exhausted";
    case EngineError::internal_failure:
      return "internal_failure";
  }
  return "unknown";
}

struct BookTop final {
  std::optional<domain::TopOfBookLevel> best_bid{};
  std::optional<domain::TopOfBookLevel> best_ask{};

  bool operator==(const BookTop&) const = default;
};

struct EngineResult final {
  std::optional<domain::EventBatch> batch{};
  EngineError error{EngineError::none};

  EngineResult() = default;
  EngineResult(const EngineResult&) = delete;
  EngineResult& operator=(const EngineResult&) = delete;
  EngineResult(EngineResult&& other) noexcept : batch{std::move(other.batch)}, error{other.error} {
    other.batch.reset();
    other.error = EngineError::none;
  }
  EngineResult& operator=(EngineResult&& other) noexcept {
    if (this != &other) {
      batch = std::move(other.batch);
      error = other.error;
      other.batch.reset();
      other.error = EngineError::none;
    }
    return *this;
  }
  ~EngineResult() = default;

  [[nodiscard]] bool has_value() const noexcept {
    return batch.has_value() && !batch->empty() && error == EngineError::none;
  }

  [[nodiscard]] bool rejected() const noexcept {
    return has_value() && domain::event_type((*batch)[0]) == domain::EventType::rejected;
  }

  [[nodiscard]] bool committed() const noexcept {
    return has_value() && domain::event_type((*batch)[0]) == domain::EventType::accepted;
  }

  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
};

class MatchingEngine final {
 public:
  explicit MatchingEngine(domain::InstrumentId instrument_id, MatchingEngineConfig config = {});
  ~MatchingEngine() noexcept;

  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine& operator=(const MatchingEngine&) = delete;
  MatchingEngine(MatchingEngine&&) = delete;
  MatchingEngine& operator=(MatchingEngine&&) = delete;

  [[nodiscard]] EngineResult execute(const domain::NewOrder& order);
  [[nodiscard]] EngineResult execute(const domain::CancelOrder& order);
  [[nodiscard]] EngineResult execute(const domain::ReplaceOrder& order);
  [[nodiscard]] EngineResult execute(const domain::Command& command);

  [[nodiscard]] domain::InstrumentId instrument_id() const noexcept;
  [[nodiscard]] std::size_t active_order_count() const noexcept;
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] BookTop top() const noexcept;
  [[nodiscard]] BookSnapshot snapshot() const;
  [[nodiscard]] Digest256 state_digest() const;
  [[nodiscard]] domain::Sequence next_sequence() const noexcept;
  [[nodiscard]] bool sequence_exhausted() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace atlaslob
