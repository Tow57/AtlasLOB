#pragma once

#include <iosfwd>
#include <string_view>

#include "atlaslob/domain/types.hpp"

namespace atlaslob::cli {

inline constexpr int engine_fixture_committed_exit_code = 0;
inline constexpr int engine_fixture_rejected_exit_code = 1;
inline constexpr int engine_fixture_input_error_exit_code = 2;
inline constexpr int engine_fixture_engine_error_exit_code = 3;

[[nodiscard]] int run_engine_fixture(domain::InstrumentId instrument_id, std::istream& input,
                                     std::ostream& output);

// The text overload is the CLI boundary: it accepts only a complete, nonzero
// base-10 uint32 value before constructing an engine.
[[nodiscard]] int run_engine_fixture_file(std::string_view instrument_id, std::string_view path,
                                          std::ostream& output);

}  // namespace atlaslob::cli
