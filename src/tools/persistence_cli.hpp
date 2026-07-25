#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace atlaslob::persistence::detail {

inline constexpr int cli_invalid_data_exit_code{1};
inline constexpr int cli_usage_exit_code{2};
inline constexpr int cli_io_failure_exit_code{3};

[[nodiscard]] int run_atlas_inspect(std::span<const std::string_view> arguments,
                                    std::ostream& output, std::ostream& error);

[[nodiscard]] int run_atlas_replay(std::span<const std::string_view> arguments,
                                   std::ostream& output, std::ostream& error);

}  // namespace atlaslob::persistence::detail
