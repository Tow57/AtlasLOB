#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "multi_native_driver.hpp"

namespace {

using atlaslob::differential::multi_native_driver_input_error_exit_code;
using atlaslob::differential::multi_native_driver_success_exit_code;
using atlaslob::differential::MultiOutputMode;
using atlaslob::differential::run_multi_native_driver;

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

constexpr std::string_view all_event_stream{
    "ATLAS_DIFF_V2 16 2 6 2\n"
    "I 9 1000 1 8\n"
    "I 7 1000 1 8\n"
    "N 11 1 7 1 1 1 1 100 5\n"
    "R 11 1 2 7 101 5\n"
    "N 22 3 7 2 2 2 0 0 2\n"
    "C 11 2 7\n"
    "N 33 4 9 1 1 1 1 90 3\n"
    "N 44 5 7 0 1 1 1 99 1\n"};

TEST(MultiNativeDiffDriver, ExactModeSerializesEveryEventAlternativeAndFullEngineState) {
  std::istringstream input{std::string{all_event_stream}};
  std::ostringstream output;

  ASSERT_EQ(run_multi_native_driver(input, output, MultiOutputMode::exact),
            multi_native_driver_success_exit_code);
  const auto records = lines(output.str());
  ASSERT_EQ(records.size(), 8U);
  EXPECT_NE(
      records.front().find(
          R"({"schema":"atlas_diff_v2","kind":"config","mode":"exact","semantics_version":"6","max_total_active_orders":"16","catalog_count":"2","catalog":[{"instrument_id":"7")"),
      std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"accepted")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"rejected")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"trade")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"rested")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"canceled")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"replaced")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"done")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("type":"book_changed")"), std::string::npos);
  EXPECT_NE(records[6].find(R"("reject_reason":"invalid_side")"), std::string::npos);
  EXPECT_NE(
      records.back().find(
          R"("kind":"final","commands_declared":"6","commands_processed":"6","committed":"5","rejected":"1","engine_errors":"0")"),
      std::string::npos);
  EXPECT_EQ(occurrences(output.str(), R"("checkpoint_snapshot":{)"), 3U);
  EXPECT_NE(
      records.back().find(
          R"("instruments":[{"instrument_id":"7","active_order_count":"0","bids":[],"asks":[]},{"instrument_id":"9","active_order_count":"1")"),
      std::string::npos);
}

TEST(MultiNativeDiffDriver, CompactModeOmitsEventsButKeepsDigestsAndRequestedCheckpoints) {
  constexpr std::string_view stream{
      "ATLAS_DIFF_V2 8 2 2 1\n"
      "I 7 1000 1 4\n"
      "I 9 1000 1 4\n"
      "N 11 1 7 1 1 1 1 100 5\n"
      "C 11 99 9\n"};
  std::istringstream input{std::string{stream}};
  std::ostringstream output;

  ASSERT_EQ(run_multi_native_driver(input, output, MultiOutputMode::compact),
            multi_native_driver_success_exit_code);
  const auto records = lines(output.str());
  ASSERT_EQ(records.size(), 4U);
  EXPECT_NE(records.front().find(R"("mode":"compact")"), std::string::npos);
  EXPECT_EQ(output.str().find(R"("events":[)"), std::string::npos);
  EXPECT_EQ(occurrences(output.str(), R"("events":null)"), 2U);
  EXPECT_EQ(occurrences(output.str(), R"("checkpoint_snapshot":{)"), 2U);
  EXPECT_NE(records[1].find(R"("event_digest":")"), std::string::npos);
  EXPECT_NE(records[2].find(R"("reject_reason":"unknown_order_id")"), std::string::npos);
}

TEST(MultiNativeDiffDriver, GlobalIdentityAndInstrumentOwnershipAreVisibleInV2Evidence) {
  constexpr std::string_view stream{
      "ATLAS_DIFF_V2 8 2 4 0\n"
      "I 7 1000 1 4\n"
      "I 9 1000 1 4\n"
      "N 11 1 7 1 1 1 1 100 5\n"
      "N 11 1 9 1 1 1 1 90 5\n"
      "C 12 1 7\n"
      "C 11 1 9\n"};
  std::istringstream input{std::string{stream}};
  std::ostringstream output;

  ASSERT_EQ(run_multi_native_driver(input, output), multi_native_driver_success_exit_code);
  EXPECT_NE(output.str().find(R"("reject_reason":"duplicate_order_id")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("reject_reason":"ownership_mismatch")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("reject_reason":"instrument_mismatch")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("command_sequence":"4")"), std::string::npos);
}

TEST(MultiNativeDiffDriver, EntireStreamIsValidatedBeforeEngineConstructionOrSubmission) {
  constexpr std::string_view stream{
      "ATLAS_DIFF_V2 8 1 2 0\n"
      "I 7 1000 1 4\n"
      "N 11 1 7 1 1 1 1 100 5\n"
      "N 11 2 7 1 1 1 0 100 5\n"};
  std::istringstream input{std::string{stream}};
  std::ostringstream output;

  ASSERT_EQ(run_multi_native_driver(input, output), multi_native_driver_input_error_exit_code);
  EXPECT_EQ(output.str(),
            "{\"schema\":\"atlas_diff_v2\",\"kind\":\"error\",\"line\":\"4\","
            "\"code\":\"nonzero_absent_price_placeholder\"}\n");
}

TEST(MultiNativeDiffDriver, ExactRecordCountsAreEnforced) {
  {
    std::istringstream input{
        "ATLAS_DIFF_V2 8 2 0 0\n"
        "I 7 1000 1 4\n"};
    std::ostringstream output;
    EXPECT_EQ(run_multi_native_driver(input, output), multi_native_driver_input_error_exit_code);
    EXPECT_EQ(output.str(),
              "{\"schema\":\"atlas_diff_v2\",\"kind\":\"error\",\"line\":\"3\","
              "\"code\":\"missing_instrument_record\"}\n");
  }
  {
    std::istringstream input{
        "ATLAS_DIFF_V2 8 1 0 0\n"
        "I 7 1000 1 4\n"
        "C 11 1 7\n"};
    std::ostringstream output;
    EXPECT_EQ(run_multi_native_driver(input, output), multi_native_driver_input_error_exit_code);
    EXPECT_EQ(output.str(),
              "{\"schema\":\"atlas_diff_v2\",\"kind\":\"error\",\"line\":\"3\","
              "\"code\":\"unexpected_trailing_input\"}\n");
  }
}

TEST(MultiNativeDiffDriver, CanonicalWhitespaceAndLineEndingsAreRequired) {
  const std::vector<std::string> streams{
      "ATLAS_DIFF_V2 08 1 0 0\nI 7 1000 1 4\n", "ATLAS_DIFF_V2 8  1 0 0\nI 7 1000 1 4\n",
      "ATLAS_DIFF_V2\t8 1 0 0\nI 7 1000 1 4\n", "ATLAS_DIFF_V2 8 1 0 0\r\nI 7 1000 1 4\r\n",
      "ATLAS_DIFF_V2 8 1 0 0\nI 7 1000 1 4",    "ATLAS_DIFF_V2 8 1 0 0\nI 7 1000 -0 4\n",
      "ATLAS_DIFF_V2 +8 1 0 0\nI 7 1000 1 4\n",
  };

  for (const auto& stream : streams) {
    SCOPED_TRACE(stream);
    std::istringstream input{stream};
    std::ostringstream output;
    EXPECT_EQ(run_multi_native_driver(input, output), multi_native_driver_input_error_exit_code);
    EXPECT_EQ(output.str().find(R"("kind":"result")"), std::string::npos);
  }
}

TEST(MultiNativeDiffDriver, RawEnumsAndSignedPriceBoundsReachSequencedDomainValidation) {
  constexpr std::string_view stream{
      "ATLAS_DIFF_V2 8 1 3 0\n"
      "I 7 18446744073709551615 1 4\n"
      "N 11 1 7 255 254 253 1 100 5\n"
      "N 11 2 7 1 1 1 1 -9223372036854775808 5\n"
      "N 11 0 7 1 1 1 1 9223372036854775807 5\n"};
  std::istringstream input{std::string{stream}};
  std::ostringstream output;

  ASSERT_EQ(run_multi_native_driver(input, output), multi_native_driver_success_exit_code);
  EXPECT_NE(output.str().find(R"("reject_reason":"invalid_side")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("reject_reason":"invalid_price")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("reject_reason":"invalid_order_id")"), std::string::npos);
  EXPECT_NE(output.str().find(R"("last_sequence":"3")"), std::string::npos);
}

TEST(MultiNativeDiffDriver, HostRepresentationBoundsAreChecked) {
#if SIZE_MAX < UINT64_MAX
  {
    std::istringstream input{
        "ATLAS_DIFF_V2 18446744073709551615 1 0 0\n"
        "I 7 1000 1 4\n"};
    std::ostringstream output;
    EXPECT_EQ(run_multi_native_driver(input, output), multi_native_driver_input_error_exit_code);
    EXPECT_NE(output.str().find(R"("code":"invalid_header_max_total_active_orders")"),
              std::string::npos);
  }
#endif
}

TEST(MultiNativeDiffDriver, IdenticalStreamsProduceByteIdenticalEvidence) {
  std::istringstream first_input{std::string{all_event_stream}};
  std::istringstream second_input{std::string{all_event_stream}};
  std::ostringstream first_output;
  std::ostringstream second_output;

  ASSERT_EQ(run_multi_native_driver(first_input, first_output),
            multi_native_driver_success_exit_code);
  ASSERT_EQ(run_multi_native_driver(second_input, second_output),
            multi_native_driver_success_exit_code);
  EXPECT_EQ(first_output.str(), second_output.str());
}

TEST(MultiNativeDiffDriver, EmptyCommandStreamStillEmitsCanonicalFinalState) {
  std::istringstream input{
      "ATLAS_DIFF_V2 8 2 0 5\n"
      "I 9 1000 1 4\n"
      "I 7 1000 1 4\n"};
  std::ostringstream output;

  ASSERT_EQ(run_multi_native_driver(input, output), multi_native_driver_success_exit_code);
  const auto records = lines(output.str());
  ASSERT_EQ(records.size(), 2U);
  EXPECT_NE(records.back().find(
                R"("commands_processed":"0","committed":"0","rejected":"0","engine_errors":"0")"),
            std::string::npos);
  EXPECT_NE(
      records.back().find(
          R"("instruments":[{"instrument_id":"7","active_order_count":"0","bids":[],"asks":[]},{"instrument_id":"9","active_order_count":"0","bids":[],"asks":[]}])"),
      std::string::npos);
}

}  // namespace
