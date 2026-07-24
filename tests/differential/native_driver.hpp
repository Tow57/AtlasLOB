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

}  // namespace atlaslob::differential
