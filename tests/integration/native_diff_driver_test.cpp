#include <gtest/gtest.h>

#include <cstddef>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

#include "native_driver.hpp"

namespace {

using atlaslob::differential::native_driver_engine_error_exit_code;
using atlaslob::differential::native_driver_input_error_exit_code;
using atlaslob::differential::native_driver_success_exit_code;
using atlaslob::differential::OutputMode;
using atlaslob::differential::run_native_driver;

[[nodiscard]] std::vector<std::string> lines(std::string_view text) {
  std::istringstream input{std::string{text}};
  std::vector<std::string> result;
  std::string line;
  while (std::getline(input, line)) {
    result.push_back(line);
  }
  return result;
}

[[nodiscard]] std::size_t occurrences(std::string_view text, std::string_view needle) {
  std::size_t count = 0U;
  std::size_t position = 0U;
  while ((position = text.find(needle, position)) != std::string_view::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

class RejectingOutputBuffer final : public std::streambuf {
 protected:
  int_type overflow(int_type) override { return traits_type::eof(); }

  std::streamsize xsputn(const char_type*, std::streamsize) override { return 0; }
};

constexpr std::string_view all_event_stream{
    "ATLAS_DIFF_V1 7 1000 1 16 2\n"
    "N 11 1 7 1 1 1 1 100 5\n"
    "R 11 1 2 7 101 5\n"
    "N 22 3 7 2 2 2 0 0 2\n"
    "C 11 2 7\n"
    "N 11 4 7 0 1 1 1 99 1\n"};

TEST(NativeDiffDriver, ExactModeSerializesEveryEventAlternativeAndFinalSnapshot) {
  std::istringstream input{std::string{all_event_stream}};
  std::ostringstream output;

  ASSERT_EQ(run_native_driver(input, output, OutputMode::exact), native_driver_success_exit_code);
  const auto records = lines(output.str());
  ASSERT_EQ(records.size(), 7U);
  EXPECT_NE(records.front().find(R"({"schema":"atlas_diff_v1","kind":"config","mode":"exact")"),
            std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"accepted")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"rejected")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"trade")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"rested")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"canceled")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"replaced")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"done")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"book_changed")"), std::string::npos);
  EXPECT_NE(records[5].find(R"("outcome":"rejected")"), std::string::npos);
  EXPECT_NE(records[5].find(R"("reason":"invalid_side")"), std::string::npos);
  EXPECT_NE(records.back().find(R"("kind":"final","commands_processed":"5")"), std::string::npos);
  EXPECT_EQ(occurrences(output.str(), R"("snapshot":{)"), 3U);
}

TEST(NativeDiffDriver, CompactModeOmitsEventsAndEmitsOnlyRequestedCheckpointSnapshots) {
  constexpr std::string_view stream{
      "ATLAS_DIFF_V1 7 1000 1 16 2\n"
      "N 11 18446744073709551615 7 1 1 1 1 9223372036854775807 5\n"
      "C 11 1 7\n"};
  std::istringstream input{std::string{stream}};
  std::ostringstream output;

  ASSERT_EQ(run_native_driver(input, output, OutputMode::compact), native_driver_success_exit_code);
  const auto records = lines(output.str());
  ASSERT_EQ(records.size(), 4U);
  EXPECT_NE(records[1].find(R"("command_index":"0")"), std::string::npos);
  EXPECT_NE(records[1].find(R"("event_digest":")"), std::string::npos);
  EXPECT_NE(records[1].find(R"("events":null)"), std::string::npos);
  EXPECT_NE(records[1].find(R"("snapshot":null)"), std::string::npos);
  EXPECT_NE(records[2].find(R"("snapshot":{"semantics_version":6)"), std::string::npos);
  EXPECT_EQ(output.str().find(R"("events":[)"), std::string::npos);
  EXPECT_NE(output.str().find(R"("order_id":"18446744073709551615")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("price":"9223372036854775807")"), std::string::npos);
}

TEST(NativeDiffDriver, RawEnumsSignedPricesAndAbsentIdsReachDomainValidation) {
  constexpr std::string_view stream{
      "ATLAS_DIFF_V1 7 1000 1 16 0\n"
      "N 11 1 7 255 254 253 1 100 5\n"
      "N 11 2 7 1 1 1 1 -9223372036854775808 5\n"
      "N 11 0 7 1 1 1 1 100 5\n"};
  std::istringstream input{std::string{stream}};
  std::ostringstream output;

  ASSERT_EQ(run_native_driver(input, output), native_driver_success_exit_code);
  EXPECT_NE(output.str().find(R"("outcome":"rejected","command_sequence":"1")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("reason":"invalid_side")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("reason":"invalid_price","order_id":"2")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("reason":"invalid_order_id","order_id":null)"), std::string::npos);
  EXPECT_NE(output.str().find(R"("next_sequence":"4")"), std::string::npos);
}

TEST(NativeDiffDriver, ValidationPrecedencePreservesIndependentRawEnumCodes) {
  constexpr std::string_view stream{
      "ATLAS_DIFF_V1 7 1000 1 16 0\n"
      "N 11 1 7 1 255 1 1 100 5\n"
      "N 11 2 7 1 1 255 1 100 5\n"};
  std::istringstream input{std::string{stream}};
  std::ostringstream output;

  ASSERT_EQ(run_native_driver(input, output), native_driver_success_exit_code);
  EXPECT_NE(output.str().find(R"("reason":"invalid_order_type","order_id":"1")"),
            std::string::npos);
  EXPECT_NE(output.str().find(R"("reason":"invalid_time_in_force","order_id":"2")"),
            std::string::npos);
}

TEST(NativeDiffDriver, DecimalJsonIsIndependentOfCallerStreamFormatting) {
  constexpr std::string_view stream{
      "ATLAS_DIFF_V1 10 1000 1 16 0\n"
      "N 11 10 10 1 1 1 1 100 5\n"};
  std::istringstream input{std::string{stream}};
  std::ostringstream output;
  output << std::hex;

  ASSERT_EQ(run_native_driver(input, output), native_driver_success_exit_code);
  EXPECT_NE(output.str().find(R"("semantics_version":6)"), std::string::npos);
  EXPECT_NE(output.str().find(R"("event_index":0)"), std::string::npos);
  EXPECT_NE(output.str().find(R"("instrument_id":"10")"), std::string::npos);
  EXPECT_EQ(output.str().find(R"("instrument_id":"a")"), std::string::npos);
}

TEST(NativeDiffDriver, MalformedAdapterRecordStopsBeforeDomainSubmission) {
  constexpr std::string_view stream{
      "ATLAS_DIFF_V1 7 1000 1 16 1\n"
      "N 11 1 7 1 1 1 0 100 5\n"};
  std::istringstream input{std::string{stream}};
  std::ostringstream output;

  ASSERT_EQ(run_native_driver(input, output), native_driver_input_error_exit_code);
  const auto records = lines(output.str());
  ASSERT_EQ(records.size(), 2U);
  EXPECT_NE(
      records[1].find(
          R"({"schema":"atlas_diff_v1","kind":"error","line":"2","code":"nonzero_absent_price_placeholder"})"),
      std::string::npos);
  EXPECT_EQ(output.str().find(R"("kind":"result")"), std::string::npos);
  EXPECT_EQ(output.str().find(R"("kind":"final")"), std::string::npos);
}

TEST(NativeDiffDriver, RejectsInvalidHeaderAndInputReadFailureDeterministically) {
  {
    std::istringstream input{"ATLAS_DIFF_V2 7 1000 1 16 1\n"};
    std::ostringstream output;
    EXPECT_EQ(run_native_driver(input, output), native_driver_input_error_exit_code);
    EXPECT_EQ(output.str(),
              "{\"schema\":\"atlas_diff_v1\",\"kind\":\"error\",\"line\":\"1\","
              "\"code\":\"unsupported_header\"}\n");
  }
  {
    std::istringstream input;
    input.setstate(std::ios::badbit);
    std::ostringstream output;
    EXPECT_EQ(run_native_driver(input, output), native_driver_input_error_exit_code);
    EXPECT_EQ(output.str(),
              "{\"schema\":\"atlas_diff_v1\",\"kind\":\"error\",\"line\":\"1\","
              "\"code\":\"input_read_failure\"}\n");
  }
}

TEST(NativeDiffDriver, IdenticalStreamsProduceByteIdenticalEvidence) {
  std::istringstream first_input{std::string{all_event_stream}};
  std::istringstream second_input{std::string{all_event_stream}};
  std::ostringstream first_output;
  std::ostringstream second_output;

  ASSERT_EQ(run_native_driver(first_input, first_output), native_driver_success_exit_code);
  ASSERT_EQ(run_native_driver(second_input, second_output), native_driver_success_exit_code);
  EXPECT_EQ(first_output.str(), second_output.str());
}

TEST(NativeDiffDriver, OutputFailureStopsAsAProcessFailure) {
  std::istringstream input{"ATLAS_DIFF_V1 7 1000 1 16 0\n"};
  RejectingOutputBuffer buffer;
  std::ostream output{&buffer};

  EXPECT_EQ(run_native_driver(input, output), native_driver_engine_error_exit_code);
  EXPECT_FALSE(output);
}

}  // namespace
