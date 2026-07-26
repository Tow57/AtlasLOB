#pragma once

#include <cstdint>

namespace atlaslob::benchmark {

struct AllocationStatistics final {
  std::uint64_t allocation_count{};
  std::uint64_t deallocation_count{};
  std::uint64_t allocated_bytes{};
  std::uint64_t live_bytes{};
  std::uint64_t peak_live_bytes{};
};

// Only the allocation runner links the implementation of these functions and
// its replaceable global new/delete surface.
void begin_allocation_tracking() noexcept;
[[nodiscard]] AllocationStatistics end_allocation_tracking() noexcept;

}  // namespace atlaslob::benchmark
