#pragma once

#include <iosfwd>
#include <string_view>

namespace atlaslob::cli {

inline constexpr int fixture_valid_exit_code = 0;
inline constexpr int fixture_domain_invalid_exit_code = 1;
inline constexpr int fixture_parse_error_exit_code = 2;

[[nodiscard]] int run_domain_fixture(std::istream& input, std::ostream& output);

[[nodiscard]] int run_domain_fixture_file(std::string_view path, std::ostream& output);

}  // namespace atlaslob::cli
