#pragma once

#if !defined(ATLAS_ENABLE_TEST_ACCESS) || !ATLAS_ENABLE_TEST_ACCESS
#error "Canonical digest test access is available only through atlas_core_testable"
#endif

#include <cstdint>
#include <span>

#include "atlaslob/digest.hpp"

namespace atlaslob::core::test {

[[nodiscard]] Digest256 sha256_for_testing(std::span<const std::uint8_t> bytes) noexcept;

}  // namespace atlaslob::core::test
