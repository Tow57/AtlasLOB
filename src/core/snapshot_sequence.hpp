#pragma once

#include <cstdint>
#include <limits>

#include "atlaslob/domain/types.hpp"

namespace atlaslob::core {

// Precondition: next_sequence is nonzero unless sequence_exhausted is true.
[[nodiscard]] constexpr domain::Sequence snapshot_last_sequence(domain::Sequence next_sequence,
                                                                bool sequence_exhausted) noexcept {
  if (sequence_exhausted) {
    return domain::Sequence{std::numeric_limits<std::uint64_t>::max()};
  }
  return domain::Sequence{next_sequence.value() - 1U};
}

}  // namespace atlaslob::core
