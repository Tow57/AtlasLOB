#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "checked_arithmetic.hpp"

#ifndef ATLAS_ENABLE_INVARIANTS
#error "atlas_core must publish ATLAS_ENABLE_INVARIANTS to its consumers"
#endif

namespace {

using atlaslob::core::detail::checked_add;
using atlaslob::core::detail::checked_subtract;

TEST(CoreConfiguration, InvariantDefinitionIsBoolean) {
  EXPECT_TRUE(ATLAS_ENABLE_INVARIANTS == 0 || ATLAS_ENABLE_INVARIANTS == 1);
}

TEST(CheckedArithmetic, AddsWithoutOverflow) {
  std::uint64_t result = 99U;

  EXPECT_TRUE(checked_add<std::uint64_t>(40U, 2U, result));
  EXPECT_EQ(result, 42U);
}

TEST(CheckedArithmetic, DetectsOverflowWithoutChangingTheDestination) {
  std::uint64_t result = 99U;

  EXPECT_FALSE(checked_add(std::numeric_limits<std::uint64_t>::max(), std::uint64_t{1U}, result));
  EXPECT_EQ(result, 99U);
}

TEST(CheckedArithmetic, SubtractsWithoutUnderflow) {
  std::uint64_t result = 99U;

  EXPECT_TRUE(checked_subtract<std::uint64_t>(42U, 2U, result));
  EXPECT_EQ(result, 40U);
}

TEST(CheckedArithmetic, DetectsUnderflowWithoutChangingTheDestination) {
  std::uint64_t result = 99U;

  EXPECT_FALSE(checked_subtract<std::uint64_t>(1U, 2U, result));
  EXPECT_EQ(result, 99U);
}

}  // namespace
