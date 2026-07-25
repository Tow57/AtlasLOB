#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "binary_codec.hpp"

namespace {

using namespace atlaslob::persistence::detail;

TEST(PersistenceBinaryCodec, EncodesFixedWidthBigEndianScalarsWithoutPadding) {
  BinaryEncoder encoder;
  encoder.u8(0xabU);
  encoder.u16(0xcdefU);
  encoder.u32(0x01234567U);
  encoder.u64(0x89abcdef01234567ULL);
  encoder.i64(std::numeric_limits<std::int64_t>::min());

  const std::array<std::uint8_t, 23U> expected{
      0xabU, 0xcdU, 0xefU, 0x01U, 0x23U, 0x45U, 0x67U, 0x89U, 0xabU, 0xcdU, 0xefU, 0x01U,
      0x23U, 0x45U, 0x67U, 0x80U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  };
  EXPECT_TRUE(std::ranges::equal(encoder.view(), expected));
}

TEST(PersistenceBinaryCodec, DecodesSignedBoundsByTheirExactTwosComplementBits) {
  const std::array<std::uint8_t, 16U> encoded{
      0x80U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
      0x7fU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
  };
  BinaryDecoder decoder{encoded};
  std::int64_t minimum = 0;
  std::int64_t maximum = 0;

  ASSERT_TRUE(decoder.i64(minimum));
  ASSERT_TRUE(decoder.i64(maximum));
  EXPECT_EQ(minimum, std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(maximum, std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(decoder.remaining(), 0U);
}

TEST(PersistenceBinaryCodec, FailedReadsAreAtomicAndRemainAtTheFailureOffset) {
  const std::array<std::uint8_t, 3U> encoded{0x01U, 0x02U, 0x03U};
  BinaryDecoder decoder{encoded};
  std::uint32_t value = 0xdeadbeefU;

  EXPECT_FALSE(decoder.u32(value));
  EXPECT_EQ(value, 0xdeadbeefU);
  EXPECT_EQ(decoder.position(), 0U);

  std::span<const std::uint8_t> bytes;
  EXPECT_FALSE(decoder.bytes(4U, bytes));
  EXPECT_EQ(decoder.position(), 0U);
}

TEST(PersistenceBinaryCodec, CheckedArithmeticRejectsRepresentationOverflow) {
  std::size_t result = 123U;
  EXPECT_FALSE(checked_add(std::numeric_limits<std::size_t>::max(), 1U, result));
  EXPECT_EQ(result, 123U);
  EXPECT_FALSE(checked_multiply(std::numeric_limits<std::size_t>::max(), 2U, result));
  EXPECT_EQ(result, 123U);

  ASSERT_TRUE(checked_add(40U, 2U, result));
  EXPECT_EQ(result, 42U);
  ASSERT_TRUE(checked_multiply(6U, 7U, result));
  EXPECT_EQ(result, 42U);
}

TEST(PersistenceBinaryCodec, CanonicalCapacityRoundTripsTheUnboundedSentinel) {
  EXPECT_EQ(canonical_capacity(std::numeric_limits<std::size_t>::max()),
            std::numeric_limits<std::uint64_t>::max());
  ASSERT_TRUE(host_capacity(std::numeric_limits<std::uint64_t>::max()).has_value());
  EXPECT_EQ(*host_capacity(std::numeric_limits<std::uint64_t>::max()),
            std::numeric_limits<std::size_t>::max());
  ASSERT_TRUE(host_capacity(17U).has_value());
  EXPECT_EQ(*host_capacity(17U), 17U);
}

}  // namespace
