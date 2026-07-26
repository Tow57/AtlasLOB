#pragma once

#include <span>
#include <string_view>

namespace atlaslob::benchmark {

inline constexpr int materializer_success_exit_code = 0;
inline constexpr int materializer_invalid_input_exit_code = 1;
inline constexpr int materializer_usage_exit_code = 2;
inline constexpr int materializer_operational_error_exit_code = 3;

// Writes a new ATLSLG01 file and emits one canonical ASCII JSON result. The
// output path is exclusive-create and is never overwritten.
[[nodiscard]] int run_log_materializer_cli(std::span<const std::string_view> arguments);

// Executable boundary with lossless UTF-8 argument acquisition on Windows.
[[nodiscard]] int run_native_log_materializer_cli(int argc, char** argv);

}  // namespace atlaslob::benchmark
