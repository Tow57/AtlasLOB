#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>

#include "crc32c.hpp"

namespace {

using atlaslob::persistence::detail::crc32c;

TEST(PersistenceCrc32c, MatchesTheStandardCastagnoliCheckVector) {
  constexpr std::string_view input{"123456789"};
  const auto bytes = std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(input.data()), input.size()};

  EXPECT_EQ(crc32c(bytes), 0xe3069283U);
}

TEST(PersistenceCrc32c, EmptyInputUsesTheSpecifiedInitialAndFinalXor) { EXPECT_EQ(crc32c({}), 0U); }

TEST(PersistenceCrc32c, CoversByteOrderAndEveryInputByte) {
  const std::array<std::uint8_t, 4U> first{0x01U, 0x02U, 0x03U, 0x04U};
  const std::array<std::uint8_t, 4U> reordered{0x04U, 0x03U, 0x02U, 0x01U};
  const std::array<std::uint8_t, 4U> changed{0x01U, 0x02U, 0x03U, 0x05U};

  EXPECT_NE(crc32c(first), crc32c(reordered));
  EXPECT_NE(crc32c(first), crc32c(changed));
}

}  // namespace
