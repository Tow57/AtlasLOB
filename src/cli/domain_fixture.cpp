#include "domain_fixture.hpp"

#include <fstream>
#include <istream>
#include <ostream>
#include <string>
#include <variant>

#include "domain_fixture_formatter.hpp"
#include "domain_fixture_parser.hpp"

namespace atlaslob::cli {

int run_domain_fixture(std::istream& input, std::ostream& output) {
  bool saw_domain_invalid = false;
  bool saw_parse_error = false;
  std::size_t line_number = 0;
  std::string line;

  while (std::getline(input, line)) {
    ++line_number;
    auto parsed = detail::parse_fixture_line(line);

    if (std::holds_alternative<detail::IgnoredLine>(parsed)) {
      continue;
    }
    if (const auto* error = std::get_if<detail::ParseError>(&parsed)) {
      detail::write_parse_error(output, line_number, *error);
      saw_parse_error = true;
      continue;
    }

    const auto& command = std::get<detail::ParsedCommand>(parsed);
    if (!detail::write_validated_command(output, line_number, command)) {
      saw_domain_invalid = true;
    }
  }

  if (input.bad() || (input.fail() && !input.eof())) {
    detail::write_parse_error(output, line_number + 1U,
                              detail::ParseError{.reason = "input_read_failure"});
    saw_parse_error = true;
  }

  if (saw_parse_error) {
    return fixture_parse_error_exit_code;
  }
  if (saw_domain_invalid) {
    return fixture_domain_invalid_exit_code;
  }
  return fixture_valid_exit_code;
}

int run_domain_fixture_file(std::string_view path, std::ostream& output) {
  std::ifstream input{std::string{path}};
  if (!input.is_open()) {
    output << "line=0 PARSE_ERROR reason=cannot_open_file\n";
    return fixture_parse_error_exit_code;
  }
  return run_domain_fixture(input, output);
}

}  // namespace atlaslob::cli
