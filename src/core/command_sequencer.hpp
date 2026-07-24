#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "atlaslob/domain/types.hpp"

namespace atlaslob::core {

enum class CommandSequencerError : std::uint8_t {
  none = 0,
  exhausted = 1,
};

[[nodiscard]] constexpr std::string_view to_string(CommandSequencerError error) noexcept {
  switch (error) {
    case CommandSequencerError::none:
      return "none";
    case CommandSequencerError::exhausted:
      return "exhausted";
  }
  return "unknown";
}

struct SequenceIssueResult final {
  domain::Sequence sequence{};
  CommandSequencerError error{CommandSequencerError::none};

  [[nodiscard]] constexpr bool has_value() const noexcept {
    return sequence.value() != 0U && error == CommandSequencerError::none;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }

  bool operator==(const SequenceIssueResult&) const = default;
};

class CommandSequencer final {
 public:
  CommandSequencer() noexcept = default;

  explicit CommandSequencer(domain::Sequence first_sequence) : next_sequence_{first_sequence} {
    if (first_sequence.value() == 0U) {
      throw std::invalid_argument{"CommandSequencer requires a nonzero first sequence"};
    }
  }

  CommandSequencer(const CommandSequencer&) = delete;
  CommandSequencer& operator=(const CommandSequencer&) = delete;
  CommandSequencer(CommandSequencer&&) = delete;
  CommandSequencer& operator=(CommandSequencer&&) = delete;
  ~CommandSequencer() = default;

  [[nodiscard]] SequenceIssueResult issue() noexcept {
    if (exhausted_) {
      return {
          .sequence = {},
          .error = CommandSequencerError::exhausted,
      };
    }

    const auto issued = next_sequence_;
    if (issued.value() == std::numeric_limits<std::uint64_t>::max()) {
      next_sequence_ = {};
      exhausted_ = true;
    } else {
      next_sequence_ = domain::Sequence{issued.value() + 1U};
    }

    return {
        .sequence = issued,
        .error = CommandSequencerError::none,
    };
  }

  [[nodiscard]] domain::Sequence next_sequence() const noexcept { return next_sequence_; }
  [[nodiscard]] bool exhausted() const noexcept { return exhausted_; }

 private:
  domain::Sequence next_sequence_{1U};
  bool exhausted_{false};
};

}  // namespace atlaslob::core
