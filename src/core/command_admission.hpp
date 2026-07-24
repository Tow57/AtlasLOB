#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "atlaslob/domain/commands.hpp"
#include "command_sequencer.hpp"
#include "execution_policy.hpp"
#include "instrument_book.hpp"

namespace atlaslob::core {

enum class CommandAdmissionError : std::uint8_t {
  none = 0,
  sequence_exhausted = 1,
  invalid_policy = 2,
  book_invariant_violation = 3,
};

[[nodiscard]] constexpr std::string_view to_string(CommandAdmissionError error) noexcept {
  switch (error) {
    case CommandAdmissionError::none:
      return "none";
    case CommandAdmissionError::sequence_exhausted:
      return "sequence_exhausted";
    case CommandAdmissionError::invalid_policy:
      return "invalid_policy";
    case CommandAdmissionError::book_invariant_violation:
      return "book_invariant_violation";
  }
  return "unknown";
}

struct CommandAdmissionResult final {
  domain::Sequence command_sequence{};
  domain::CommandType command_type{};
  domain::RejectReason reject_reason{domain::RejectReason::none};
  std::optional<domain::OrderId> relevant_order_id{};
  CommandAdmissionError internal_error{CommandAdmissionError::none};

  [[nodiscard]] constexpr bool processed() const noexcept {
    return command_sequence.value() != 0U && internal_error == CommandAdmissionError::none;
  }

  [[nodiscard]] constexpr bool accepted() const noexcept {
    return processed() && reject_reason == domain::RejectReason::none;
  }

  [[nodiscard]] constexpr bool rejected() const noexcept {
    return processed() && reject_reason != domain::RejectReason::none;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return accepted(); }

  bool operator==(const CommandAdmissionResult&) const = default;
};

class CommandAdmission final {
 public:
  explicit CommandAdmission(const InstrumentBook& book, ExecutionPolicy policy = {});
  CommandAdmission(const InstrumentBook& book, ExecutionPolicy policy,
                   domain::Sequence first_sequence);

  CommandAdmission(const CommandAdmission&) = delete;
  CommandAdmission& operator=(const CommandAdmission&) = delete;
  CommandAdmission(CommandAdmission&&) = delete;
  CommandAdmission& operator=(CommandAdmission&&) = delete;
  ~CommandAdmission() = default;

  [[nodiscard]] CommandAdmissionResult admit(const domain::NewOrder& order) noexcept;
  [[nodiscard]] CommandAdmissionResult admit(const domain::CancelOrder& order) noexcept;
  [[nodiscard]] CommandAdmissionResult admit(const domain::ReplaceOrder& order) noexcept;
  [[nodiscard]] CommandAdmissionResult admit(const domain::Command& command) noexcept;

  [[nodiscard]] const ExecutionPolicy& policy() const noexcept { return policy_; }
  [[nodiscard]] domain::Sequence next_sequence() const noexcept {
    return sequencer_.next_sequence();
  }
  [[nodiscard]] bool sequence_exhausted() const noexcept { return sequencer_.exhausted(); }

 private:
  const InstrumentBook& book_;
  ExecutionPolicy policy_;
  CommandSequencer sequencer_;
};

}  // namespace atlaslob::core
