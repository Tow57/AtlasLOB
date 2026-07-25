#pragma once

#include <iosfwd>

namespace atlaslob::differential {

enum class OutputMode {
  exact,
  compact,
};

inline constexpr int native_driver_success_exit_code = 0;
inline constexpr int native_driver_input_error_exit_code = 2;
inline constexpr int native_driver_engine_error_exit_code = 3;

// Runs the versioned differential evidence adapter over one complete command
// stream. Harness syntax failures do not enter domain processing and therefore
// do not consume an engine sequence.
[[nodiscard]] int run_native_driver(std::istream& input, std::ostream& output,
                                    OutputMode mode = OutputMode::exact);

#if defined(ATLAS_DIFF_NATIVE_NO_MAIN)
enum class NativeDriverConstructionFailureForTest {
  allocation,
  exception,
};

[[nodiscard]] int run_native_driver_with_construction_failure_for_test(
    std::istream& input, std::ostream& output, NativeDriverConstructionFailureForTest failure,
    OutputMode mode = OutputMode::exact);
#endif

}  // namespace atlaslob::differential
