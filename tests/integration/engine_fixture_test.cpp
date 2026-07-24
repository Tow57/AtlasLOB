#include "engine_fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>

#include "atlaslob/matching_engine.hpp"

namespace {

[[nodiscard]] std::filesystem::path fixture_directory() {
  return std::filesystem::path{__FILE__}.parent_path() / "fixtures";
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
  std::ifstream input{path};
  EXPECT_TRUE(input.is_open()) << path;
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

void expect_golden(std::string_view stem, int expected_exit_code) {
  const auto input_path = fixture_directory() / (std::string{stem} + ".commands");
  const auto expected_path = fixture_directory() / (std::string{stem} + ".expected");
  std::ostringstream output;

  EXPECT_EQ(atlaslob::cli::run_engine_fixture_file("7", input_path.string(), output),
            expected_exit_code);
  EXPECT_EQ(output.str(), read_text(expected_path));
}

TEST(EngineFixtureIntegration, CommittedCommandsReturnZeroAndMatchGoldenOutput) {
  expect_golden("engine_valid", atlaslob::cli::engine_fixture_committed_exit_code);
}

TEST(EngineFixtureIntegration, RejectionsReturnOneAndMatchGoldenOutput) {
  expect_golden("engine_rejected", atlaslob::cli::engine_fixture_rejected_exit_code);
}

TEST(EngineFixtureIntegration, ParseErrorsTakePrecedenceAndMatchGoldenOutput) {
  expect_golden("engine_parse_error", atlaslob::cli::engine_fixture_input_error_exit_code);
}

TEST(EngineFixtureIntegration, ParseErrorsDoNotConsumeSequenceAndWrongRouteDoes) {
  std::ostringstream output;
  const auto path = fixture_directory() / "engine_parse_error.commands";

  ASSERT_EQ(atlaslob::cli::run_engine_fixture_file("7", path.string(), output),
            atlaslob::cli::engine_fixture_input_error_exit_code);
  const auto text = output.str();
  EXPECT_NE(text.find("line=2 batch_sequence=1 outcome=committed"), std::string::npos);
  EXPECT_NE(text.find("line=3 PARSE_ERROR reason=unknown_command"), std::string::npos);
  EXPECT_NE(text.find("line=4 batch_sequence=2 outcome=rejected"), std::string::npos);
  EXPECT_NE(text.find("line=5 batch_sequence=3 outcome=committed"), std::string::npos);
  EXPECT_NE(text.find("line=6 PARSE_ERROR reason=invalid_order_id"), std::string::npos);
  EXPECT_NE(text.find("commands=3 committed=2 rejected=1 parse_errors=2 engine_errors=0 "
                      "last_sequence=3"),
            std::string::npos);
}

TEST(EngineFixtureIntegration, MissingFileReturnsTwoWithEmptyEngineSummary) {
  std::ostringstream output;
  const auto missing = fixture_directory() / "does-not-exist-engine.commands";
  const atlaslob::MatchingEngine empty_engine{atlaslob::domain::InstrumentId{7U}};

  EXPECT_EQ(atlaslob::cli::run_engine_fixture_file("7", missing.string(), output),
            atlaslob::cli::engine_fixture_input_error_exit_code);
  EXPECT_EQ(output.str(),
            "line=0 PARSE_ERROR reason=cannot_open_file\n"
            "summary commands=0 committed=0 rejected=0 parse_errors=1 engine_errors=0 "
            "last_sequence=0 state_digest=" +
                empty_engine.state_digest().hex() + "\n");
}

TEST(EngineFixtureIntegration, InstrumentArgumentRequiresStrictNonzeroUint32) {
  constexpr std::array invalid_ids{"", "0", "-1", "+1", "1x", "4294967296"};
  for (const auto* const invalid_id : invalid_ids) {
    SCOPED_TRACE(invalid_id);
    std::ostringstream output;
    EXPECT_EQ(atlaslob::cli::run_engine_fixture_file(
                  invalid_id, (fixture_directory() / "engine_valid.commands").string(), output),
              atlaslob::cli::engine_fixture_input_error_exit_code);
    EXPECT_EQ(output.str(), "line=0 PARSE_ERROR reason=invalid_engine_instrument_id\n");
  }
}

TEST(EngineFixtureIntegration, InputReadFailureReturnsTwoAndKeepsEmptySequence) {
  std::istringstream input;
  input.setstate(std::ios::badbit);
  std::ostringstream output;
  const atlaslob::MatchingEngine empty_engine{atlaslob::domain::InstrumentId{7U}};

  EXPECT_EQ(atlaslob::cli::run_engine_fixture(atlaslob::domain::InstrumentId{7U}, input, output),
            atlaslob::cli::engine_fixture_input_error_exit_code);
  EXPECT_EQ(output.str(),
            "line=1 PARSE_ERROR reason=input_read_failure\n"
            "summary commands=0 committed=0 rejected=0 parse_errors=1 engine_errors=0 "
            "last_sequence=0 state_digest=" +
                empty_engine.state_digest().hex() + "\n");
}

}  // namespace
