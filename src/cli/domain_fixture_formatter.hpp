#pragma once

#include <cstddef>
#include <iosfwd>

#include "domain_fixture_parser.hpp"

namespace atlaslob::cli::detail {

void write_parse_error(std::ostream& output, std::size_t line_number, const ParseError& error);

[[nodiscard]] bool write_validated_command(std::ostream& output, std::size_t line_number,
                                           const ParsedCommand& command);

}  // namespace atlaslob::cli::detail
