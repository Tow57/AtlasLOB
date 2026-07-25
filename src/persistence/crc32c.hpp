#pragma once

#include <cstdint>
#include <span>

namespace atlaslob::persistence::detail {

[[nodiscard]] std::uint32_t crc32c(std::span<const std::uint8_t> bytes) noexcept;

}  // namespace atlaslob::persistence::detail
