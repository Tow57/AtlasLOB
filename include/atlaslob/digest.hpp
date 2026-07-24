#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "atlaslob/book_snapshot.hpp"
#include "atlaslob/domain/event_batch.hpp"

namespace atlaslob {

struct Digest256 final {
  std::array<std::uint8_t, 32U> bytes{};

  [[nodiscard]] std::string hex() const;

  bool operator==(const Digest256&) const = default;
};

// Hashes the documented canonical, fixed-width, big-endian encoding. These
// digests are deterministic regression/replay evidence, not authentication.
[[nodiscard]] Digest256 state_digest(const BookSnapshot& snapshot) noexcept;

[[nodiscard]] Digest256 event_digest(const domain::EventBatch& batch) noexcept;

}  // namespace atlaslob
