#pragma once

#include <cstdint>

#include "allocation_tracker.hpp"

namespace atlaslob::benchmark {

enum class RunnerFlavor : std::uint8_t {
  timed,
  allocation,
};

struct AllocationHooks final {
  void (*begin)() noexcept {};
  AllocationStatistics (*end)() noexcept {};
};

inline constexpr int benchmark_success_exit_code = 0;
inline constexpr int benchmark_invalid_observation_exit_code = 1;
inline constexpr int benchmark_usage_exit_code = 2;
inline constexpr int benchmark_operational_error_exit_code = 3;

[[nodiscard]] int run_benchmark_cli(int argc, char** argv, RunnerFlavor flavor,
                                    AllocationHooks allocation_hooks = {});

// Executable boundary: obtains UTF-8 arguments from the native command line on
// Windows and configures stdout/stderr for canonical LF bytes.
[[nodiscard]] int run_native_benchmark_cli(int argc, char** argv, RunnerFlavor flavor,
                                           AllocationHooks allocation_hooks = {});

}  // namespace atlaslob::benchmark
