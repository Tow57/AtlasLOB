#include "engine_fixture.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

#include "atlaslob/matching_engine.hpp"
#include "domain_fixture_formatter.hpp"
#include "domain_fixture_parser.hpp"

namespace atlaslob::cli {
namespace {

struct FixtureCounts final {
  std::size_t commands{};
  std::size_t committed{};
  std::size_t rejected{};
  std::size_t parse_errors{};
  std::size_t engine_errors{};
};

[[nodiscard]] bool parse_instrument_id(std::string_view token,
                                       domain::InstrumentId& destination) noexcept {
  std::uint32_t value{};
  const auto* const begin = token.data();
  const auto* const end = begin + token.size();
  const auto [parsed_end, error] = std::from_chars(begin, end, value, 10);
  if (token.empty() || error != std::errc{} || parsed_end != end || value == 0U) {
    return false;
  }
  destination = domain::InstrumentId{value};
  return true;
}

void write_event_types(std::ostream& output, const domain::EventBatch& batch) {
  for (std::size_t index = 0U; index < batch.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    output << domain::to_string(domain::event_type(batch[index]));
  }
}

void write_summary(std::ostream& output, const MatchingEngine& engine,
                   const FixtureCounts& counts) {
  const auto snapshot = engine.snapshot();
  output << "summary"
         << " commands=" << counts.commands << " committed=" << counts.committed
         << " rejected=" << counts.rejected << " parse_errors=" << counts.parse_errors
         << " engine_errors=" << counts.engine_errors
         << " last_sequence=" << snapshot.last_sequence.value()
         << " state_digest=" << state_digest(snapshot).hex() << '\n';
}

[[nodiscard]] int exit_code(const FixtureCounts& counts) noexcept {
  if (counts.engine_errors != 0U) {
    return engine_fixture_engine_error_exit_code;
  }
  if (counts.parse_errors != 0U) {
    return engine_fixture_input_error_exit_code;
  }
  if (counts.rejected != 0U) {
    return engine_fixture_rejected_exit_code;
  }
  return engine_fixture_committed_exit_code;
}

[[nodiscard]] int run_fixture(MatchingEngine& engine, std::istream& input, std::ostream& output) {
  FixtureCounts counts{};
  std::size_t line_number = 0U;
  std::string line;

  while (std::getline(input, line)) {
    ++line_number;
    auto parsed = detail::parse_fixture_line(line);
    if (std::holds_alternative<detail::IgnoredLine>(parsed)) {
      continue;
    }
    if (const auto* error = std::get_if<detail::ParseError>(&parsed)) {
      detail::write_parse_error(output, line_number, *error);
      ++counts.parse_errors;
      continue;
    }

    ++counts.commands;
    const auto command = detail::to_domain_command(std::get<detail::ParsedCommand>(parsed));
    const auto result = engine.execute(command);
    if (!result) {
      ++counts.engine_errors;
      output << "line=" << line_number << " ENGINE_ERROR reason=";
      if (result.error() == EngineError::none) {
        output << "invalid_result";
      } else {
        output << to_string(result.error());
      }
      output << " state_digest=" << engine.state_digest().hex() << '\n';
      continue;
    }

    const auto outcome =
        result.committed() ? std::string_view{"committed"} : std::string_view{"rejected"};
    if (result.committed()) {
      ++counts.committed;
    } else if (result.rejected()) {
      ++counts.rejected;
    } else {
      ++counts.engine_errors;
      output << "line=" << line_number << " ENGINE_ERROR reason=invalid_batch_outcome"
             << " state_digest=" << engine.state_digest().hex() << '\n';
      continue;
    }

    const auto& batch = *result.batch();
    output << "line=" << line_number << " batch_sequence=" << batch.command_sequence().value()
           << " outcome=" << outcome << " events=";
    write_event_types(output, batch);
    output << " event_digest=" << event_digest(batch).hex()
           << " state_digest=" << engine.state_digest().hex() << '\n';
  }

  if (input.bad() || (input.fail() && !input.eof())) {
    detail::write_parse_error(output, line_number + 1U,
                              detail::ParseError{.reason = "input_read_failure"});
    ++counts.parse_errors;
  }

  write_summary(output, engine, counts);
  return exit_code(counts);
}

}  // namespace

int run_engine_fixture(domain::InstrumentId instrument_id, std::istream& input,
                       std::ostream& output) {
  if (instrument_id.value() == 0U) {
    detail::write_parse_error(output, 0U,
                              detail::ParseError{.reason = "invalid_engine_instrument_id"});
    return engine_fixture_input_error_exit_code;
  }
  MatchingEngine engine{instrument_id};
  return run_fixture(engine, input, output);
}

int run_engine_fixture_file(std::string_view instrument_id, std::string_view path,
                            std::ostream& output) {
  domain::InstrumentId parsed_instrument{};
  if (!parse_instrument_id(instrument_id, parsed_instrument)) {
    detail::write_parse_error(output, 0U,
                              detail::ParseError{.reason = "invalid_engine_instrument_id"});
    return engine_fixture_input_error_exit_code;
  }

  MatchingEngine engine{parsed_instrument};
  std::ifstream input{std::string{path}};
  if (!input.is_open()) {
    FixtureCounts counts{.parse_errors = 1U};
    detail::write_parse_error(output, 0U, detail::ParseError{.reason = "cannot_open_file"});
    write_summary(output, engine, counts);
    return engine_fixture_input_error_exit_code;
  }
  return run_fixture(engine, input, output);
}

}  // namespace atlaslob::cli
