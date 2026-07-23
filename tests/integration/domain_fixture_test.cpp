#include "domain_fixture.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::filesystem::path fixture_directory() {
  return std::filesystem::path{__FILE__}.parent_path() / "fixtures";
}

[[nodiscard]] std::filesystem::path repository_root() {
  return std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path();
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
  std::ifstream input{path};
  EXPECT_TRUE(input.is_open()) << path;
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

void expect_golden_fixture(const std::filesystem::path& input_path, std::string_view golden_stem,
                           int expected_exit_code) {
  const auto expected_path = fixture_directory() / (std::string{golden_stem} + ".expected");
  std::ostringstream output;

  EXPECT_EQ(atlaslob::cli::run_domain_fixture_file(input_path.string(), output),
            expected_exit_code);
  EXPECT_EQ(output.str(), read_text(expected_path));
}

TEST(DomainFixtureIntegration, AllValidCommandsReturnZeroAndMatchGoldenOutput) {
  expect_golden_fixture(repository_root() / "examples" / "domain-valid.commands", "valid",
                        atlaslob::cli::fixture_valid_exit_code);
}

TEST(DomainFixtureIntegration, DomainInvalidCommandsReturnOneAndMatchGoldenOutput) {
  expect_golden_fixture(fixture_directory() / "domain_invalid.commands", "domain_invalid",
                        atlaslob::cli::fixture_domain_invalid_exit_code);
}

TEST(DomainFixtureIntegration, ParseErrorsReturnTwoAndMatchGoldenOutput) {
  expect_golden_fixture(fixture_directory() / "parse_error.commands", "parse_error",
                        atlaslob::cli::fixture_parse_error_exit_code);
}

TEST(DomainFixtureIntegration, AcceptsExactNumericMaximaAndCrLf) {
  std::istringstream input{
      "NEW 4294967295 18446744073709551615 4294967295 BUY LIMIT GTC "
      "9223372036854775807 18446744073709551615\r\n"};
  std::ostringstream output;

  EXPECT_EQ(atlaslob::cli::run_domain_fixture(input, output),
            atlaslob::cli::fixture_valid_exit_code);
  EXPECT_EQ(output.str(),
            "line=1 NEW client_id=4294967295 order_id=18446744073709551615 "
            "instrument_id=4294967295 side=buy type=limit time_in_force=gtc "
            "price_ticks=9223372036854775807 quantity=18446744073709551615 "
            "validation=valid reason=none\n");
}

TEST(DomainFixtureIntegration, ParsesSignedMinimumBeforeDomainValidation) {
  std::istringstream input{"NEW 1 1 1 BUY LIMIT GTC -9223372036854775808 1\n"};
  std::ostringstream output;

  EXPECT_EQ(atlaslob::cli::run_domain_fixture(input, output),
            atlaslob::cli::fixture_domain_invalid_exit_code);
  EXPECT_EQ(output.str(),
            "line=1 NEW client_id=1 order_id=1 instrument_id=1 side=buy type=limit "
            "time_in_force=gtc price_ticks=-9223372036854775808 quantity=1 "
            "validation=invalid reason=invalid_price\n");
}

TEST(DomainFixtureIntegration, RejectsPartiallyParsedNumbers) {
  std::istringstream input{"CANCEL 1 12x 1\n"};
  std::ostringstream output;

  EXPECT_EQ(atlaslob::cli::run_domain_fixture(input, output),
            atlaslob::cli::fixture_parse_error_exit_code);
  EXPECT_EQ(output.str(), "line=1 PARSE_ERROR reason=invalid_order_id\n");
}

TEST(DomainFixtureIntegration, ReportsInputStreamFailures) {
  std::istringstream input;
  input.setstate(std::ios::badbit);
  std::ostringstream output;

  EXPECT_EQ(atlaslob::cli::run_domain_fixture(input, output),
            atlaslob::cli::fixture_parse_error_exit_code);
  EXPECT_EQ(output.str(), "line=1 PARSE_ERROR reason=input_read_failure\n");
}

TEST(DomainFixtureIntegration, ReportsFailbitOnlyInputStreamFailures) {
  std::istringstream input;
  input.setstate(std::ios::failbit);
  std::ostringstream output;

  EXPECT_EQ(atlaslob::cli::run_domain_fixture(input, output),
            atlaslob::cli::fixture_parse_error_exit_code);
  EXPECT_EQ(output.str(), "line=1 PARSE_ERROR reason=input_read_failure\n");
}

TEST(DomainFixtureIntegration, MissingFileIsReportedAsParseError) {
  std::ostringstream output;
  const auto missing_path = fixture_directory() / "does-not-exist.commands";

  EXPECT_EQ(atlaslob::cli::run_domain_fixture_file(missing_path.string(), output),
            atlaslob::cli::fixture_parse_error_exit_code);
  EXPECT_EQ(output.str(), "line=0 PARSE_ERROR reason=cannot_open_file\n");
}

}  // namespace
