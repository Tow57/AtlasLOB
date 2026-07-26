#include "allocation_tracker.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace {

struct AllocationHeader final {
  void* raw{};
  std::size_t requested{};
  std::uint64_t tracking_generation{};
  std::uint64_t magic{};
};

constexpr std::uint64_t live_allocation_magic{0xa71a510ba110ca7eULL};
constexpr std::uint64_t freed_allocation_magic{0xdead510bdeadca7eULL};

struct Tracker final {
  std::atomic<bool> active{false};
  std::atomic<std::uint64_t> generation{0U};
  std::atomic<std::uint64_t> allocation_count{0U};
  std::atomic<std::uint64_t> deallocation_count{0U};
  std::atomic<std::uint64_t> allocated_bytes{0U};
  std::atomic<std::uint64_t> live_bytes{0U};
  std::atomic<std::uint64_t> peak_live_bytes{0U};
};

Tracker tracker;

#if defined(__STDCPP_DEFAULT_NEW_ALIGNMENT__)
constexpr std::size_t default_new_alignment{__STDCPP_DEFAULT_NEW_ALIGNMENT__};
#else
constexpr std::size_t default_new_alignment{alignof(std::max_align_t)};
#endif

[[nodiscard]] bool checked_total(std::size_t requested, std::size_t alignment,
                                 std::size_t& total) noexcept {
  constexpr auto maximum = std::numeric_limits<std::size_t>::max();
  const auto overhead = sizeof(AllocationHeader) + alignment - 1U;
  if (requested > maximum - overhead) {
    return false;
  }
  total = requested + overhead;
  return true;
}

void update_peak(std::uint64_t live) noexcept {
  auto peak = tracker.peak_live_bytes.load(std::memory_order_relaxed);
  while (peak < live && !tracker.peak_live_bytes.compare_exchange_weak(
                            peak, live, std::memory_order_relaxed, std::memory_order_relaxed)) {
  }
}

[[nodiscard]] void* try_allocate(std::size_t requested, std::size_t alignment) noexcept {
  std::size_t total{};
  if (alignment == 0U || (alignment & (alignment - 1U)) != 0U ||
      !checked_total(requested, alignment, total)) {
    return nullptr;
  }
  void* const raw = std::malloc(total);
  if (raw == nullptr) {
    return nullptr;
  }

  const auto start = reinterpret_cast<std::uintptr_t>(raw) + sizeof(AllocationHeader);
  const auto aligned = (start + alignment - 1U) & ~(static_cast<std::uintptr_t>(alignment) - 1U);
  auto* const header = reinterpret_cast<AllocationHeader*>(aligned) - 1;
  header->raw = raw;
  header->requested = requested;
  header->tracking_generation = 0U;
  header->magic = live_allocation_magic;

  if (tracker.active.load(std::memory_order_relaxed)) {
    const auto generation = tracker.generation.load(std::memory_order_relaxed);
    header->tracking_generation = generation;
    tracker.allocation_count.fetch_add(1U, std::memory_order_relaxed);
    tracker.allocated_bytes.fetch_add(static_cast<std::uint64_t>(requested),
                                      std::memory_order_relaxed);
    const auto live = tracker.live_bytes.fetch_add(static_cast<std::uint64_t>(requested),
                                                   std::memory_order_relaxed) +
                      static_cast<std::uint64_t>(requested);
    update_peak(live);
  }
  return reinterpret_cast<void*>(aligned);
}

[[nodiscard]] void* allocate_throwing(std::size_t requested, std::size_t alignment) {
  while (true) {
    if (void* result = try_allocate(requested, alignment); result != nullptr) {
      return result;
    }
    const auto handler = std::get_new_handler();
    if (handler == nullptr) {
      throw std::bad_alloc{};
    }
    handler();
  }
}

void deallocate(void* pointer) noexcept {
  if (pointer == nullptr) {
    return;
  }
  auto* const header = reinterpret_cast<AllocationHeader*>(pointer) - 1;
  if (header->magic != live_allocation_magic) {
    std::abort();
  }
  if (tracker.active.load(std::memory_order_relaxed)) {
    tracker.deallocation_count.fetch_add(1U, std::memory_order_relaxed);
    const auto generation = tracker.generation.load(std::memory_order_relaxed);
    if (header->tracking_generation == generation) {
      tracker.live_bytes.fetch_sub(static_cast<std::uint64_t>(header->requested),
                                   std::memory_order_relaxed);
    }
  }
  void* const raw = header->raw;
  header->magic = freed_allocation_magic;
  std::free(raw);
}

}  // namespace

namespace atlaslob::benchmark {

void begin_allocation_tracking() noexcept {
  tracker.active.store(false, std::memory_order_seq_cst);
  tracker.allocation_count.store(0U, std::memory_order_relaxed);
  tracker.deallocation_count.store(0U, std::memory_order_relaxed);
  tracker.allocated_bytes.store(0U, std::memory_order_relaxed);
  tracker.live_bytes.store(0U, std::memory_order_relaxed);
  tracker.peak_live_bytes.store(0U, std::memory_order_relaxed);
  tracker.generation.fetch_add(1U, std::memory_order_relaxed);
  tracker.active.store(true, std::memory_order_seq_cst);
}

AllocationStatistics end_allocation_tracking() noexcept {
  tracker.active.store(false, std::memory_order_seq_cst);
  return AllocationStatistics{
      .allocation_count = tracker.allocation_count.load(std::memory_order_relaxed),
      .deallocation_count = tracker.deallocation_count.load(std::memory_order_relaxed),
      .allocated_bytes = tracker.allocated_bytes.load(std::memory_order_relaxed),
      .live_bytes = tracker.live_bytes.load(std::memory_order_relaxed),
      .peak_live_bytes = tracker.peak_live_bytes.load(std::memory_order_relaxed),
  };
}

}  // namespace atlaslob::benchmark

void* operator new(std::size_t size) { return allocate_throwing(size, default_new_alignment); }

void* operator new[](std::size_t size) { return allocate_throwing(size, default_new_alignment); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate_throwing(size, default_new_alignment);
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate_throwing(size, default_new_alignment);
  } catch (...) {
    return nullptr;
  }
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocate_throwing(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocate_throwing(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try {
    return allocate_throwing(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try {
    return allocate_throwing(size, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* pointer) noexcept { deallocate(pointer); }
void operator delete[](void* pointer) noexcept { deallocate(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { deallocate(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { deallocate(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { deallocate(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { deallocate(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { deallocate(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { deallocate(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { deallocate(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  deallocate(pointer);
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  deallocate(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  deallocate(pointer);
}
