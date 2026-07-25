#include "crc32c.hpp"

namespace atlaslob::persistence::detail {

std::uint32_t crc32c(std::span<const std::uint8_t> bytes) noexcept {
  constexpr std::uint32_t reflected_castagnoli_polynomial{0x82f63b78U};
  std::uint32_t checksum = 0xffffffffU;
  for (const auto byte : bytes) {
    checksum ^= static_cast<std::uint32_t>(byte);
    for (unsigned bit = 0U; bit < 8U; ++bit) {
      const auto mask = static_cast<std::uint32_t>(0U - (checksum & 1U));
      checksum = (checksum >> 1U) ^ (reflected_castagnoli_polynomial & mask);
    }
  }
  return checksum ^ 0xffffffffU;
}

}  // namespace atlaslob::persistence::detail
