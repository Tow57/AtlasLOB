#include <cstddef>
#include <cstdint>
#include <new>

#include "allocation_tracker.hpp"

// Clang with libstdc++ does not declare sized deallocation overloads in
// <new> unless its sized-deallocation language mode is enabled. The
// allocation runner still replaces those ABI entry points, so declare the
// definitions linked from allocation_tracker.cpp explicitly for this
// process-level surface test.
void operator delete(void* pointer, std::size_t size) noexcept;
void operator delete[](void* pointer, std::size_t size) noexcept;
void operator delete(void* pointer, std::size_t size, std::align_val_t alignment) noexcept;
void operator delete[](void* pointer, std::size_t size, std::align_val_t alignment) noexcept;

namespace {

[[nodiscard]] bool is_aligned(const void* pointer, std::size_t alignment) noexcept {
  return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0U;
}

[[nodiscard]] bool equals(const atlaslob::benchmark::AllocationStatistics& actual,
                          std::uint64_t allocations, std::uint64_t deallocations,
                          std::uint64_t allocated_bytes, std::uint64_t live_bytes,
                          std::uint64_t peak_live_bytes) noexcept {
  return actual.allocation_count == allocations && actual.deallocation_count == deallocations &&
         actual.allocated_bytes == allocated_bytes && actual.live_bytes == live_bytes &&
         actual.peak_live_bytes == peak_live_bytes;
}

}  // namespace

int main() {
  using atlaslob::benchmark::begin_allocation_tracking;
  using atlaslob::benchmark::end_allocation_tracking;

  // Allocate once before tracking so the first generation proves that freeing
  // older storage cannot reduce the current generation's live-byte count.
  void* const preexisting = ::operator new(7U);

  begin_allocation_tracking();
  void* const scalar = ::operator new(11U);
  void* const array = ::operator new[](13U);
  void* const aligned = ::operator new(17U, std::align_val_t{64U});
  void* const aligned_array = ::operator new[](61U, std::align_val_t{512U});
  void* const scalar_sized = ::operator new(19U, std::nothrow);
  void* const array_sized = ::operator new[](23U, std::nothrow);
  void* const aligned_sized = ::operator new(29U, std::align_val_t{128U}, std::nothrow);
  void* const aligned_array_sized = ::operator new[](31U, std::align_val_t{256U}, std::nothrow);
  void* const scalar_nothrow = ::operator new(37U, std::nothrow);
  void* const array_nothrow = ::operator new[](41U, std::nothrow);
  void* const aligned_nothrow = ::operator new(43U, std::align_val_t{64U}, std::nothrow);
  void* const aligned_array_nothrow = ::operator new[](47U, std::align_val_t{128U}, std::nothrow);
  void* const generation_survivor = ::operator new(53U);

  if (scalar_sized == nullptr || array_sized == nullptr || aligned_sized == nullptr ||
      aligned_array_sized == nullptr || scalar_nothrow == nullptr || array_nothrow == nullptr ||
      aligned_nothrow == nullptr || aligned_array_nothrow == nullptr || !is_aligned(aligned, 64U) ||
      !is_aligned(aligned_array, 512U) || !is_aligned(aligned_sized, 128U) ||
      !is_aligned(aligned_array_sized, 256U) || !is_aligned(aligned_nothrow, 64U) ||
      !is_aligned(aligned_array_nothrow, 128U)) {
    return 1;
  }

  ::operator delete(preexisting);
  ::operator delete(scalar);
  ::operator delete[](array);
  ::operator delete(aligned, std::align_val_t{64U});
  ::operator delete[](aligned_array, std::align_val_t{512U});
  ::operator delete(scalar_sized, 19U);
  ::operator delete[](array_sized, 23U);
  ::operator delete(aligned_sized, 29U, std::align_val_t{128U});
  ::operator delete[](aligned_array_sized, 31U, std::align_val_t{256U});
  ::operator delete(scalar_nothrow, std::nothrow);
  ::operator delete[](array_nothrow, std::nothrow);
  ::operator delete(aligned_nothrow, std::align_val_t{64U}, std::nothrow);
  ::operator delete[](aligned_array_nothrow, std::align_val_t{128U}, std::nothrow);

  const auto first = end_allocation_tracking();
  if (!equals(first, 13U, 13U, 425U, 53U, 425U)) {
    return 2;
  }

  begin_allocation_tracking();
  void* const current_generation = ::operator new[](59U);
  ::operator delete(generation_survivor);
  ::operator delete[](current_generation, 59U);
  const auto second = end_allocation_tracking();
  if (!equals(second, 1U, 2U, 59U, 0U, 59U)) {
    return 3;
  }

  return 0;
}
