#pragma once

#include <iosfwd>

namespace atlaslob::differential {

enum class MultiOutputMode {
  exact,
  compact,
};

inline constexpr int multi_native_driver_success_exit_code = 0;
inline constexpr int multi_native_driver_input_error_exit_code = 2;
inline constexpr int multi_native_driver_engine_error_exit_code = 3;

// Runs the strict, LF-delimited ATLAS_DIFF_V2 adapter. The complete catalog
// and command stream are parsed before the first command enters domain
// processing, so adapter failures never consume an engine sequence.
[[nodiscard]] int run_multi_native_driver(std::istream& input, std::ostream& output,
                                          MultiOutputMode mode = MultiOutputMode::exact);

}  // namespace atlaslob::differential
