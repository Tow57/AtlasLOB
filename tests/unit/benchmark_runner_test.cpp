#include "benchmark_runner.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "atlaslob/digest.hpp"
#include "atlaslob/multi_instrument_engine.hpp"
#include "atlaslob/persistence/logged_engine.hpp"
#include "benchmark_log_materializer.hpp"
#include "benchmark_observation.hpp"
#include "benchmark_workload.hpp"
#include "sha256.hpp"

namespace atlaslob::benchmark {
namespace {

std::uint32_t allocation_begin_calls = 0U;
std::uint32_t allocation_end_calls = 0U;

void begin_fake_allocation_tracking() noexcept { ++allocation_begin_calls; }

AllocationStatistics end_fake_allocation_tracking() noexcept {
  ++allocation_end_calls;
  return AllocationStatistics{
      .allocation_count = 2U,
      .deallocation_count = 1U,
      .allocated_bytes = 64U,
      .live_bytes = 32U,
      .peak_live_bytes = 64U,
  };
}

[[nodiscard]] std::string sha256_text(std::string_view text) {
  utility::Sha256 hash;
  hash.update(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()),
                                            text.size()});
  return hash.finish().hex();
}

[[nodiscard]] std::string sha256_file(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return {};
  }
  utility::Sha256 hash;
  std::array<std::uint8_t, 64U * 1024U> buffer{};
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      hash.update(std::span<const std::uint8_t>{buffer.data(), static_cast<std::size_t>(count)});
    }
  }
  return input.eof() ? hash.finish().hex() : std::string{};
}

struct CliFixture final {
  std::filesystem::path path;
  std::string workload_digest;
  std::uint64_t events{};
  std::uint64_t committed{};
  std::uint64_t rejected{};
  std::uint64_t engine_errors{};
  std::string event_digest;
  std::string final_digest;
};

struct CliInvocation final {
  int exit_code{};
  std::string output;
  std::string error;
};

[[nodiscard]] std::optional<CliFixture> make_cli_fixture(std::string_view stem,
                                                         std::string_view workload_text,
                                                         std::uint64_t preload,
                                                         std::uint64_t warmup,
                                                         std::uint64_t measured) {
  const auto total = preload + warmup + measured;
  std::istringstream input{std::string{workload_text}};
  auto parsed =
      read_atlas_diff_v2(input, maximum_benchmark_catalog_count, static_cast<std::size_t>(total));
  const auto* workload = std::get_if<BenchmarkWorkload>(&parsed);
  if (workload == nullptr) {
    return std::nullopt;
  }

  MultiInstrumentEngine engine{
      std::span<const InstrumentConfig>{workload->catalog},
      workload->engine_config,
  };
  utility::Sha256 event_hash;
  std::uint64_t events{};
  std::uint64_t committed{};
  std::uint64_t rejected{};
  std::uint64_t engine_errors{};
  const auto measured_begin = preload + warmup;
  for (std::size_t index = 0U; index < workload->commands.size(); ++index) {
    auto result = engine.execute(workload->commands[index].command);
    if (index < measured_begin || index >= measured_begin + measured) {
      continue;
    }
    const auto* batch = result.batch();
    if (batch == nullptr) {
      ++engine_errors;
      continue;
    }
    events += static_cast<std::uint64_t>(batch->size());
    if (result.committed()) {
      ++committed;
    } else {
      ++rejected;
    }
    event_hash.update(atlaslob::event_digest(*batch).bytes);
  }

  const auto path = std::filesystem::path{testing::TempDir()} /
                    ("atlaslob-benchmark-" + std::string{stem} + ".atlas");
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  if (!output) {
    return std::nullopt;
  }
  output.write(workload_text.data(), static_cast<std::streamsize>(workload_text.size()));
  if (!output) {
    return std::nullopt;
  }
  output.close();
  if (!output) {
    return std::nullopt;
  }

  return CliFixture{
      .path = path,
      .workload_digest = workload->stream_digest.hex(),
      .events = events,
      .committed = committed,
      .rejected = rejected,
      .engine_errors = engine_errors,
      .event_digest = event_hash.finish().hex(),
      .final_digest = engine.state_digest().hex(),
  };
}

[[nodiscard]] std::vector<std::string> timed_cli_arguments(const CliFixture& fixture,
                                                           std::string_view mode,
                                                           std::uint64_t preload,
                                                           std::uint64_t warmup,
                                                           std::uint64_t measured) {
  const auto executable_path = std::filesystem::absolute(testing::internal::GetArgvs().front());
  return {
      executable_path.string(),
      "--workload",
      fixture.path.string(),
      "--workload-id",
      "W00",
      "--measurement-parameter",
      "sweep_depth=0",
      "--measurement-parameter",
      "instrument_count=1",
      "--measurement-parameter",
      "measured_start_active_order_count=0",
      "--workload-manifest-sha256",
      std::string(64U, '7'),
      "--workload-sha256",
      fixture.workload_digest,
      "--binary-sha256",
      sha256_file(executable_path),
      "--environment-sha256",
      std::string(64U, '3'),
      "--host-context-sha256",
      std::string(64U, '4'),
      "--run-label",
      "smoke",
      "--suite-label",
      "smoke-suite",
      "--variant",
      "standalone",
      "--block-index",
      "0",
      "--block-position",
      "0",
      "--preload-count",
      std::to_string(preload),
      "--warmup-count",
      std::to_string(warmup),
      "--measured-count",
      std::to_string(measured),
      "--expected-events",
      std::to_string(fixture.events),
      "--expected-committed",
      std::to_string(fixture.committed),
      "--expected-rejected",
      std::to_string(fixture.rejected),
      "--expected-engine-errors",
      std::to_string(fixture.engine_errors),
      "--expected-event-digest",
      fixture.event_digest,
      "--expected-final-digest",
      fixture.final_digest,
      "--mode",
      std::string{mode},
  };
}

void replace_option(std::vector<std::string>& arguments, std::string_view option,
                    std::string value) {
  const auto found = std::find(arguments.begin(), arguments.end(), option);
  ASSERT_NE(found, arguments.end());
  ASSERT_NE(std::next(found), arguments.end());
  *std::next(found) = std::move(value);
}

[[nodiscard]] CliInvocation invoke_timed_cli(std::vector<std::string>& arguments) {
  std::vector<char*> pointers;
  pointers.reserve(arguments.size());
  for (auto& argument : arguments) {
    pointers.push_back(argument.data());
  }
  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  const auto exit_code =
      run_benchmark_cli(static_cast<int>(pointers.size()), pointers.data(), RunnerFlavor::timed);
  auto error = testing::internal::GetCapturedStderr();
  auto output = testing::internal::GetCapturedStdout();
  return CliInvocation{exit_code, std::move(output), std::move(error)};
}

[[nodiscard]] CliInvocation invoke_allocation_cli(std::vector<std::string>& arguments) {
  std::vector<char*> pointers;
  pointers.reserve(arguments.size());
  for (auto& argument : arguments) {
    pointers.push_back(argument.data());
  }
  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  const auto exit_code = run_benchmark_cli(static_cast<int>(pointers.size()), pointers.data(),
                                           RunnerFlavor::allocation,
                                           AllocationHooks{
                                               .begin = &begin_fake_allocation_tracking,
                                               .end = &end_fake_allocation_tracking,
                                           });
  auto error = testing::internal::GetCapturedStderr();
  auto output = testing::internal::GetCapturedStdout();
  return CliInvocation{exit_code, std::move(output), std::move(error)};
}

[[nodiscard]] std::size_t latency_sample_count(std::string_view output) {
  constexpr std::string_view prefix{"\"latency_ns\":["};
  const auto begin = output.find(prefix);
  if (begin == std::string_view::npos) {
    return std::numeric_limits<std::size_t>::max();
  }
  const auto values_begin = begin + prefix.size();
  const auto end = output.find(']', values_begin);
  if (end == std::string_view::npos) {
    return std::numeric_limits<std::size_t>::max();
  }
  const auto values = output.substr(values_begin, end - values_begin);
  return values.empty()
             ? 0U
             : 1U + static_cast<std::size_t>(std::count(values.begin(), values.end(), ','));
}

[[nodiscard]] std::optional<std::uint64_t> quoted_decimal_field(std::string_view document,
                                                                std::string_view name) noexcept {
  const auto prefix = std::string{"\""} + std::string{name} + "\":\"";
  const auto begin = document.find(prefix);
  if (begin == std::string_view::npos) {
    return std::nullopt;
  }
  const auto value_begin = begin + prefix.size();
  const auto value_end = document.find('"', value_begin);
  if (value_end == std::string_view::npos) {
    return std::nullopt;
  }
  std::uint64_t value{};
  const auto* first = document.data() + value_begin;
  const auto* last = document.data() + value_end;
  const auto parsed = std::from_chars(first, last, value);
  if (parsed.ec != std::errc{} || parsed.ptr != last) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] std::string cancel_workload(std::size_t command_count) {
  std::ostringstream output;
  output << "ATLAS_DIFF_V2 10 1 " << command_count << " 0\n"
         << "I 1 1000 1 10\n";
  for (std::size_t index = 0U; index < command_count; ++index) {
    output << "C 1 " << index + 1U << " 1\n";
  }
  return output.str();
}

[[nodiscard]] std::string w01_churn_workload(std::size_t active_orders,
                                             std::size_t post_preload_commands) {
  std::ostringstream output;
  output << "ATLAS_DIFF_V2 " << active_orders * 2U << " 1 " << active_orders + post_preload_commands
         << " 0\n"
         << "I 1 1000000 1 " << active_orders * 2U << "\n";
  std::vector<std::uint64_t> order_ids(active_orders);
  for (std::size_t index = 0U; index < active_orders; ++index) {
    const auto order_id = index + 1U;
    const auto client_id = 1U + index % 16U;
    const auto side = index % 2U == 0U ? 1U : 2U;
    const auto level = index % 64U;
    const auto price = side == 1U ? 10'000U - level : 20'000U + level;
    order_ids[index] = order_id;
    output << "N " << client_id << ' ' << order_id << " 1 " << side << " 1 1 1 " << price << " 1\n";
  }
  auto next_order_id = active_orders + 1U;
  for (std::size_t command = 0U; command < post_preload_commands; command += 2U) {
    const auto slot = command / 2U % active_orders;
    const auto client_id = 1U + slot % 16U;
    const auto side = slot % 2U == 0U ? 1U : 2U;
    const auto level = slot % 64U;
    const auto price = side == 1U ? 10'000U - level : 20'000U + level;
    output << "C " << client_id << ' ' << order_ids[slot] << " 1\n";
    output << "N " << client_id << ' ' << next_order_id << " 1 " << side << " 1 1 1 " << price
           << " 1\n";
    order_ids[slot] = next_order_id;
    ++next_order_id;
  }
  return output.str();
}

[[nodiscard]] persistence::LogId benchmark_log_id() {
  persistence::LogId result;
  for (std::size_t index = 0U; index < result.bytes.size(); ++index) {
    result.bytes[index] = static_cast<std::uint8_t>(index + 1U);
  }
  return result;
}

[[nodiscard]] std::optional<std::filesystem::path> make_replay_log(std::string_view stem,
                                                                   std::string_view workload_text,
                                                                   std::size_t command_count) {
  std::istringstream input{std::string{workload_text}};
  auto parsed = read_atlas_diff_v2(input, maximum_benchmark_catalog_count, command_count);
  const auto* workload = std::get_if<BenchmarkWorkload>(&parsed);
  if (workload == nullptr) {
    return std::nullopt;
  }

  const auto path = std::filesystem::path{testing::TempDir()} /
                    ("atlaslob-benchmark-" + std::string{stem} + ".atlslg");
  std::error_code ignored;
  static_cast<void>(std::filesystem::remove(path, ignored));
  auto opened = persistence::LoggedEngine::create_new(path, workload->catalog, benchmark_log_id(),
                                                      workload->engine_config);
  if (!opened) {
    return std::nullopt;
  }
  for (const auto& command : workload->commands) {
    if (!opened.engine->submit(command.command)) {
      return std::nullopt;
    }
  }
  if (!opened.engine->synchronize().ok()) {
    return std::nullopt;
  }
  opened.engine.reset();
  return path;
}

[[nodiscard]] std::vector<std::string> replay_cli_arguments(const CliFixture& fixture,
                                                            const std::filesystem::path& log_path,
                                                            std::string_view mode,
                                                            std::uint64_t records) {
  auto arguments = timed_cli_arguments(fixture, mode, 0U, 0U, records);
  const auto log_digest = sha256_file(log_path);
  arguments.emplace_back("--replay-log");
  arguments.emplace_back(log_path.string());
  arguments.emplace_back("--replay-log-sha256");
  arguments.emplace_back(log_digest);
  arguments.emplace_back("--measurement-parameter");
  arguments.emplace_back("record_count=" + std::to_string(records));
  arguments.emplace_back("--measurement-parameter");
  arguments.emplace_back("cache_policy=warm_page_cache");
  arguments.emplace_back("--measurement-parameter");
  arguments.emplace_back("replay_mode=" + std::string{mode == "replay-fast" ? "fast" : "verify"});
  arguments.emplace_back("--measurement-parameter");
  arguments.emplace_back("timed_input_sha256=" + log_digest);
  return arguments;
}

[[nodiscard]] bool corrupt_last_log_byte(const std::filesystem::path& path) {
  std::fstream stream{path, std::ios::binary | std::ios::in | std::ios::out};
  if (!stream) {
    return false;
  }
  stream.seekg(-1, std::ios::end);
  char byte{};
  stream.read(&byte, 1);
  if (!stream) {
    return false;
  }
  byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x01U);
  stream.seekp(-1, std::ios::end);
  stream.write(&byte, 1);
  stream.flush();
  return stream.good();
}

TEST(BenchmarkWorkloadTest, ReadsCanonicalV2WithoutNormalizingCommandEnums) {
  constexpr std::string_view workload_text{
      "ATLAS_DIFF_V2 20 2 3 0\n"
      "I 1 2000 1 10\n"
      "I 2 1000 2 10\n"
      "N 1 10 2 255 1 1 1 100 5\n"
      "C 1 10 2\n"
      "R 1 10 11 2 102 4\n"};
  std::istringstream input{std::string{workload_text}};

  auto parsed = read_atlas_diff_v2(input, 2U, 3U);
  ASSERT_TRUE(std::holds_alternative<BenchmarkWorkload>(parsed));
  const auto& workload = std::get<BenchmarkWorkload>(parsed);
  ASSERT_EQ(workload.catalog.size(), 2U);
  EXPECT_EQ(workload.catalog[0].instrument_id, domain::InstrumentId{1U});
  EXPECT_EQ(workload.catalog[1].instrument_id, domain::InstrumentId{2U});
  ASSERT_EQ(workload.commands.size(), 3U);
  EXPECT_EQ(std::get<domain::NewOrder>(workload.commands[0].command).side,
            static_cast<domain::Side>(255U));
  EXPECT_TRUE(std::holds_alternative<domain::CancelOrder>(workload.commands[1].command));
  EXPECT_TRUE(std::holds_alternative<domain::ReplaceOrder>(workload.commands[2].command));
  EXPECT_EQ(workload.commands[0].source_line, 4U);
  EXPECT_EQ(workload.stream_digest.hex(), sha256_text(workload_text));
}

TEST(BenchmarkWorkloadTest, ExactDigestChangesWhenOneValidInputByteChanges) {
  constexpr std::string_view first_text{
      "ATLAS_DIFF_V2 10 1 1 0\n"
      "I 1 1000 1 10\n"
      "C 1 1 1\n"};
  constexpr std::string_view second_text{
      "ATLAS_DIFF_V2 10 1 1 0\n"
      "I 1 1000 1 10\n"
      "C 1 2 1\n"};
  std::istringstream first_input{std::string{first_text}};
  std::istringstream second_input{std::string{second_text}};

  auto first = read_atlas_diff_v2(first_input, 1U, 1U);
  auto second = read_atlas_diff_v2(second_input, 1U, 1U);
  ASSERT_TRUE(std::holds_alternative<BenchmarkWorkload>(first));
  ASSERT_TRUE(std::holds_alternative<BenchmarkWorkload>(second));
  const auto& first_workload = std::get<BenchmarkWorkload>(first);
  const auto& second_workload = std::get<BenchmarkWorkload>(second);
  EXPECT_EQ(first_workload.stream_digest.hex(), sha256_text(first_text));
  EXPECT_EQ(second_workload.stream_digest.hex(), sha256_text(second_text));
  EXPECT_NE(first_workload.stream_digest, second_workload.stream_digest);
}

TEST(BenchmarkWorkloadTest, RejectsMissingFinalLf) {
  std::istringstream input{
      "ATLAS_DIFF_V2 10 1 1 0\n"
      "I 1 1000 1 10\n"
      "C 1 1 1"};

  const auto parsed = read_atlas_diff_v2(input, 1U, 1U);
  ASSERT_TRUE(std::holds_alternative<WorkloadParseError>(parsed));
  const auto error = std::get<WorkloadParseError>(parsed);
  EXPECT_EQ(error.line, 3U);
  EXPECT_EQ(error.code, "invalid_line_ending");
}

TEST(BenchmarkWorkloadTest, RejectsTrailingRecords) {
  std::istringstream input{
      "ATLAS_DIFF_V2 10 1 1 0\n"
      "I 1 1000 1 10\n"
      "C 1 1 1\n"
      "C 1 2 1\n"};

  const auto parsed = read_atlas_diff_v2(input, 1U, 1U);
  ASSERT_TRUE(std::holds_alternative<WorkloadParseError>(parsed));
  const auto error = std::get<WorkloadParseError>(parsed);
  EXPECT_EQ(error.line, 4U);
  EXPECT_EQ(error.code, "unexpected_trailing_input");
}

TEST(BenchmarkWorkloadTest, RejectsCrLfAtTheFirstAffectedRecord) {
  std::istringstream input{
      "ATLAS_DIFF_V2 10 1 1 0\r\n"
      "I 1 1000 1 10\n"
      "C 1 1 1\n"};

  const auto parsed = read_atlas_diff_v2(input, 1U, 1U);
  ASSERT_TRUE(std::holds_alternative<WorkloadParseError>(parsed));
  const auto error = std::get<WorkloadParseError>(parsed);
  EXPECT_EQ(error.line, 1U);
  EXPECT_EQ(error.code, "invalid_line_ending");
}

TEST(BenchmarkWorkloadTest, RejectsEmbeddedControlAndNonAsciiBytes) {
  std::string control_text{
      "ATLAS_DIFF_V2 10 1 1 0\n"
      "I 1 1000 1 10\n"
      "C 1\t1 1\n"};
  std::string non_ascii_text{
      "ATLAS_DIFF_V2 10 1 1 0\n"
      "I 1 1000 1 10\n"
      "C 1 1 "};
  non_ascii_text.push_back(static_cast<char>(0x80U));
  non_ascii_text.push_back('\n');

  for (const auto& text : {control_text, non_ascii_text}) {
    std::istringstream input{text};
    const auto parsed = read_atlas_diff_v2(input, 1U, 1U);
    ASSERT_TRUE(std::holds_alternative<WorkloadParseError>(parsed));
    const auto error = std::get<WorkloadParseError>(parsed);
    EXPECT_EQ(error.line, 3U);
    EXPECT_EQ(error.code, "noncanonical_command");
  }
}

TEST(BenchmarkWorkloadTest, RejectsDuplicateInstrumentIds) {
  std::istringstream input{
      "ATLAS_DIFF_V2 10 2 0 0\n"
      "I 1 1000 1 10\n"
      "I 1 2000 2 20\n"};

  const auto parsed = read_atlas_diff_v2(input, 2U, 0U);
  ASSERT_TRUE(std::holds_alternative<WorkloadParseError>(parsed));
  const auto error = std::get<WorkloadParseError>(parsed);
  EXPECT_EQ(error.line, 3U);
  EXPECT_EQ(error.code, "duplicate_instrument_id");
}

TEST(BenchmarkWorkloadTest, RejectsCatalogCountBeforeReservingCatalogStorage) {
  std::istringstream input{"ATLAS_DIFF_V2 10 4096 0 0\n"};

  const auto parsed = read_atlas_diff_v2(input, 16U, 0U);
  ASSERT_TRUE(std::holds_alternative<WorkloadParseError>(parsed));
  const auto error = std::get<WorkloadParseError>(parsed);
  EXPECT_EQ(error.line, 1U);
  EXPECT_EQ(error.code, "catalog_count_exceeds_limit");
}

TEST(BenchmarkWorkloadTest, RejectsCommandCountBeforeReservingCommandStorage) {
  std::istringstream input{"ATLAS_DIFF_V2 10 1 100000000 0\n"};

  const auto parsed = read_atlas_diff_v2(input, 1U, 3U);
  ASSERT_TRUE(std::holds_alternative<WorkloadParseError>(parsed));
  const auto error = std::get<WorkloadParseError>(parsed);
  EXPECT_EQ(error.line, 1U);
  EXPECT_EQ(error.code, "command_count_mismatch");
}

TEST(BenchmarkWorkloadTest, RejectsCommandsAboveTheHardBenchmarkLimit) {
  std::istringstream input{"ATLAS_DIFF_V2 10 1 100000001 0\n"};

  const auto parsed = read_atlas_diff_v2(input, 1U, maximum_benchmark_command_count + 1U);
  ASSERT_TRUE(std::holds_alternative<WorkloadParseError>(parsed));
  const auto error = std::get<WorkloadParseError>(parsed);
  EXPECT_EQ(error.line, 1U);
  EXPECT_EQ(error.code, "command_count_exceeds_limit");
}

TEST(BenchmarkWorkloadTest, RejectsAnOversizedRecordAtTheFixedLineBound) {
  std::istringstream input{std::string(maximum_benchmark_line_bytes + 1U, 'X') + "\n"};

  const auto parsed = read_atlas_diff_v2(input, 1U, 0U);
  ASSERT_TRUE(std::holds_alternative<WorkloadParseError>(parsed));
  const auto error = std::get<WorkloadParseError>(parsed);
  EXPECT_EQ(error.line, 1U);
  EXPECT_EQ(error.code, "line_exceeds_limit");
}

TEST(BenchmarkWorkloadTest, RejectsNonascendingCatalogInsteadOfNormalizingIt) {
  std::istringstream input{
      "ATLAS_DIFF_V2 20 2 0 0\n"
      "I 2 1000 2 10\n"
      "I 1 2000 1 10\n"};

  const auto parsed = read_atlas_diff_v2(input, 2U, 0U);
  ASSERT_TRUE(std::holds_alternative<WorkloadParseError>(parsed));
  const auto error = std::get<WorkloadParseError>(parsed);
  EXPECT_EQ(error.line, 3U);
  EXPECT_EQ(error.code, "catalog_not_strictly_ascending");
}

TEST(BenchmarkWorkloadTest, RejectsNonzeroCheckpointInterval) {
  std::istringstream input{
      "ATLAS_DIFF_V2 10 1 0 1\n"
      "I 1 1000 1 10\n"};

  const auto parsed = read_atlas_diff_v2(input, 1U, 0U);
  ASSERT_TRUE(std::holds_alternative<WorkloadParseError>(parsed));
  const auto error = std::get<WorkloadParseError>(parsed);
  EXPECT_EQ(error.line, 1U);
  EXPECT_EQ(error.code, "nonzero_checkpoint_interval");
}

TEST(BenchmarkWorkloadTest, RejectsMalformedCommandWithoutPartialSuccess) {
  std::istringstream input{
      "ATLAS_DIFF_V2 10 1 1 0\n"
      "I 1 1000 1 10\n"
      "C 1 01 1\n"};

  const auto parsed = read_atlas_diff_v2(input, 1U, 1U);
  ASSERT_TRUE(std::holds_alternative<WorkloadParseError>(parsed));
  const auto error = std::get<WorkloadParseError>(parsed);
  EXPECT_EQ(error.line, 3U);
  EXPECT_EQ(error.code, "invalid_cancel_order");
}

TEST(BenchmarkWorkloadTest, PreservesMaximumNumericValuesAndRawEnumBytes) {
  const auto maximum_size = std::numeric_limits<std::size_t>::max();
  const auto maximum_u64 = std::numeric_limits<std::uint64_t>::max();
  const auto maximum_u32 = std::numeric_limits<std::uint32_t>::max();
  const auto maximum_i64 = std::numeric_limits<std::int64_t>::max();
  const auto minimum_i64 = std::numeric_limits<std::int64_t>::min();
  const auto text = "ATLAS_DIFF_V2 " + std::to_string(maximum_size) + " 1 1 0\nI " +
                    std::to_string(maximum_u32) + " " + std::to_string(maximum_u64) + " " +
                    std::to_string(maximum_i64) + " " + std::to_string(maximum_size) + "\nN " +
                    std::to_string(maximum_u32) + " " + std::to_string(maximum_u64) + " " +
                    std::to_string(maximum_u32) + " 255 255 255 1 " + std::to_string(minimum_i64) +
                    " " + std::to_string(maximum_u64) + "\n";
  std::istringstream input{text};

  auto parsed = read_atlas_diff_v2(input, 1U, 1U);
  ASSERT_TRUE(std::holds_alternative<BenchmarkWorkload>(parsed));
  const auto& workload = std::get<BenchmarkWorkload>(parsed);
  ASSERT_EQ(workload.catalog.size(), 1U);
  EXPECT_EQ(workload.engine_config.max_total_active_orders, maximum_size);
  EXPECT_EQ(workload.catalog[0].instrument_id, domain::InstrumentId{maximum_u32});
  EXPECT_EQ(workload.catalog[0].matching.max_order_quantity, domain::Quantity{maximum_u64});
  EXPECT_EQ(workload.catalog[0].matching.tick_increment, domain::PriceTicks{maximum_i64});
  EXPECT_EQ(workload.catalog[0].matching.max_active_orders, maximum_size);
  ASSERT_EQ(workload.commands.size(), 1U);
  const auto& command = std::get<domain::NewOrder>(workload.commands[0].command);
  EXPECT_EQ(command.client_id, domain::ClientId{maximum_u32});
  EXPECT_EQ(command.order_id, domain::OrderId{maximum_u64});
  EXPECT_EQ(command.instrument_id, domain::InstrumentId{maximum_u32});
  EXPECT_EQ(command.side, static_cast<domain::Side>(255U));
  EXPECT_EQ(command.order_type, static_cast<domain::OrderType>(255U));
  EXPECT_EQ(command.time_in_force, static_cast<domain::TimeInForce>(255U));
  ASSERT_TRUE(command.limit_price.has_value());
  EXPECT_EQ(*command.limit_price, domain::PriceTicks{minimum_i64});
  EXPECT_EQ(command.quantity, domain::Quantity{maximum_u64});
  EXPECT_EQ(workload.stream_digest.hex(), sha256_text(text));
}

TEST(BenchmarkObservationTest, MatchesPythonCanonicalSortKeysEncoding) {
  BenchmarkObservation observation{
      .boundary = "core_allocation",
      .measurement_parameters =
          {
              {"instrument_count", "16"},
              {"sweep_depth", "1,8,16,32,64"},
          },
      .workload_id = "W00",
      .workload_manifest_sha256 = std::string(64U, '7'),
      .workload_sha256 = std::string(64U, '1'),
      .binary_sha256 = std::string(64U, '2'),
      .environment_sha256 = std::string(64U, '3'),
      .host_context_sha256 = std::string(64U, '4'),
      .run_label = "smoke",
      .suite_label = "smoke-suite",
      .variant = "standalone",
      .timed_input_kind = "none",
      .timed_input_sha256 = std::nullopt,
      .block_index = 0U,
      .block_position = 0U,
      .preload_commands = 5U,
      .warmup_commands = 6U,
      .commands = 7U,
      .events = 8U,
      .committed = 4U,
      .rejected = 3U,
      .engine_errors = 0U,
      .elapsed_ns = 0U,
      .peak_rss_bytes = 9U,
      .rss_before_bytes = 10U,
      .rss_after_bytes = 11U,
      .latency_ns = {},
      .allocations =
          AllocationStatistics{
              .allocation_count = 12U,
              .deallocation_count = 13U,
              .allocated_bytes = 14U,
              .live_bytes = 15U,
              .peak_live_bytes = 16U,
          },
      .event_digest = std::string(64U, '5'),
      .final_digest = std::string(64U, '6'),
      .valid = true,
      .failure_reason = std::nullopt,
  };

  std::ostringstream output;
  write_observation_json(output, observation);
  const auto expected =
      "{\"allocations\":{\"allocated_bytes\":\"14\",\"allocation_count\":\"12\","
      "\"deallocation_count\":\"13\",\"live_bytes\":\"15\",\"peak_live_bytes\":\"16\"},"
      "\"binary_sha256\":\"" +
      std::string(64U, '2') +
      "\",\"block_index\":\"0\",\"block_position\":\"0\",\"boundary\":\"core_allocation\","
      "\"commands\":\"7\",\"committed\":\"4\",\"elapsed_ns\":\"0\",\"engine_errors\":\"0\","
      "\"environment_sha256\":\"" +
      std::string(64U, '3') + "\",\"event_digest\":\"" + std::string(64U, '5') +
      "\",\"events\":\"8\",\"failure_reason\":null,\"final_digest\":\"" + std::string(64U, '6') +
      "\",\"host_context_sha256\":\"" + std::string(64U, '4') +
      "\",\"latency_ns\":null,\"measurement_parameters\":{\"instrument_count\":\"16\","
      "\"sweep_depth\":\"1,8,16,32,64\"},\"peak_rss_bytes\":\"9\","
      "\"preload_commands\":\"5\","
      "\"rejected\":\"3\",\"rss_after_bytes\":\"11\",\"rss_before_bytes\":\"10\","
      "\"run_label\":\"smoke\",\"schema\":\"ATLAS_BENCH_OBSERVATION_V1\","
      "\"suite_label\":\"smoke-suite\","
      "\"timed_input_kind\":\"none\",\"timed_input_sha256\":null,\"valid\":true,"
      "\"variant\":\"standalone\",\"warmup_commands\":\"6\",\"workload_id\":\"W00\","
      "\"workload_manifest_sha256\":\"" +
      std::string(64U, '7') +
      "\","
      "\"workload_sha256\":\"" +
      std::string(64U, '1') + "\"}\n";
  EXPECT_EQ(output.str(), expected);
}

TEST(BenchmarkObservationTest, LatencyBoundaryRetainsAnEmptyArray) {
  BenchmarkObservation observation{
      .boundary = "core_latency",
      .measurement_parameters = {},
      .workload_id = "W00",
      .workload_manifest_sha256 = std::string(64U, '7'),
      .workload_sha256 = std::string(64U, '1'),
      .binary_sha256 = std::string(64U, '2'),
      .environment_sha256 = std::string(64U, '3'),
      .host_context_sha256 = std::string(64U, '4'),
      .run_label = "smoke",
      .suite_label = "smoke-suite",
      .variant = "standalone",
      .timed_input_kind = "none",
      .timed_input_sha256 = std::nullopt,
      .block_index = 0U,
      .block_position = 0U,
      .preload_commands = 0U,
      .warmup_commands = 0U,
      .commands = 0U,
      .events = 0U,
      .committed = 0U,
      .rejected = 0U,
      .engine_errors = 0U,
      .elapsed_ns = 0U,
      .peak_rss_bytes = 0U,
      .rss_before_bytes = 0U,
      .rss_after_bytes = 0U,
      .latency_ns = {},
      .allocations = std::nullopt,
      .event_digest = std::string(64U, '5'),
      .final_digest = std::string(64U, '6'),
      .valid = true,
      .failure_reason = std::nullopt,
  };

  std::ostringstream output;
  write_observation_json(output, observation);
  EXPECT_NE(output.str().find("\"latency_ns\":[]"), std::string::npos);
  EXPECT_NE(output.str().find("\"allocations\":null"), std::string::npos);
}

TEST(BenchmarkObservationTest, RejectsNonAsciiBeforeWritingAnyBytes) {
  BenchmarkObservation observation{};
  observation.boundary = "core_throughput";
  observation.suite_label = "smoke-suite";
  observation.run_label = "bad";
  observation.run_label.push_back(static_cast<char>(0x80U));

  std::ostringstream output;
  write_observation_json(output, observation);

  EXPECT_TRUE(output.bad());
  EXPECT_TRUE(output.str().empty());
}

TEST(BenchmarkObservationTest, EscapesDeleteAsCanonicalAscii) {
  BenchmarkObservation observation{};
  observation.boundary = "core_throughput";
  observation.suite_label = "smoke-suite";
  observation.run_label = "delete";
  observation.run_label.push_back(static_cast<char>(0x7fU));

  std::ostringstream output;
  write_observation_json(output, observation);

  EXPECT_TRUE(output.good());
  EXPECT_NE(output.str().find("\"run_label\":\"delete\\u007f\""), std::string::npos);
}

TEST(BenchmarkObservationTest, RejectsNoncanonicalMeasurementParametersBeforeWriting) {
  BenchmarkObservation observation{};
  observation.boundary = "core_throughput";
  observation.suite_label = "smoke-suite";
  observation.measurement_parameters = {
      {"sweep_depth", "1"},
      {"instrument_count", "16"},
  };

  std::ostringstream output;
  write_observation_json(output, observation);

  EXPECT_TRUE(output.bad());
  EXPECT_TRUE(output.str().empty());
}

TEST(BenchmarkObservationTest, RejectsSuiteLabelsLongerThanThirtyTwoCharacters) {
  BenchmarkObservation observation{};
  observation.boundary = "core_throughput";
  observation.suite_label = std::string(33U, 's');

  std::ostringstream output;
  write_observation_json(output, observation);

  EXPECT_TRUE(output.bad());
  EXPECT_TRUE(output.str().empty());
}

TEST(BenchmarkObservationTest, MeasurementParameterGrammarHasFrozenBounds) {
  EXPECT_TRUE(is_measurement_parameter_name("instrument_count"));
  EXPECT_TRUE(is_measurement_parameter_name("A.b-c_9"));
  EXPECT_TRUE(is_measurement_parameter_name("A" + std::string(127U, 'a')));
  EXPECT_FALSE(is_measurement_parameter_name("_hidden"));
  EXPECT_FALSE(is_measurement_parameter_name("A" + std::string(128U, 'a')));

  EXPECT_TRUE(is_measurement_parameter_value("O3,summary-mode+cache.v1"));
  EXPECT_TRUE(is_measurement_parameter_value(std::string(256U, 'a')));
  EXPECT_FALSE(is_measurement_parameter_value(""));
  EXPECT_FALSE(is_measurement_parameter_value("two words"));
  EXPECT_FALSE(is_measurement_parameter_value("path/value"));
  EXPECT_FALSE(is_measurement_parameter_value(std::string(257U, 'a')));
}

TEST(BenchmarkRunnerCliTest, WorkloadDigestMismatchDoesNotExecuteAnyCommands) {
  const auto workload_text = cancel_workload(32U);
  const auto fixture = make_cli_fixture("workload-digest-mismatch", workload_text, 0U, 0U, 32U);
  ASSERT_TRUE(fixture.has_value());
  auto arguments = timed_cli_arguments(*fixture, "throughput", 0U, 0U, 32U);
  replace_option(arguments, "--workload-sha256", std::string(64U, '0'));

  const auto invocation = invoke_timed_cli(arguments);

  EXPECT_EQ(invocation.exit_code, benchmark_invalid_observation_exit_code);
  EXPECT_TRUE(invocation.error.empty());
  EXPECT_NE(invocation.output.find("\"failure_reason\":\"workload_digest_mismatch\""),
            std::string::npos);
  EXPECT_NE(invocation.output.find("\"commands\":\"0\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"events\":\"0\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"committed\":\"0\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"rejected\":\"0\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"engine_errors\":\"0\""), std::string::npos);
  EXPECT_NE(
      invocation.output.find("\"measurement_parameters\":{\"instrument_count\":\"1\","
                             "\"measured_start_active_order_count\":\"0\",\"sweep_depth\":\"0\"}"),
      std::string::npos);
  EXPECT_NE(
      invocation.output.find("\"workload_manifest_sha256\":\"" + std::string(64U, '7') + "\""),
      std::string::npos);
  EXPECT_NE(invocation.output.find("\"valid\":false"), std::string::npos);

  std::error_code cleanup_error;
  std::filesystem::remove(fixture->path, cleanup_error);
}

TEST(BenchmarkRunnerCliTest, ThroughputModeProducesAValidObservation) {
  const auto workload_text = cancel_workload(32U);
  const auto fixture = make_cli_fixture("throughput-success", workload_text, 0U, 0U, 32U);
  ASSERT_TRUE(fixture.has_value());
  auto arguments = timed_cli_arguments(*fixture, "throughput", 0U, 0U, 32U);

  const auto invocation = invoke_timed_cli(arguments);

  EXPECT_EQ(invocation.exit_code, benchmark_success_exit_code);
  EXPECT_TRUE(invocation.error.empty());
  EXPECT_NE(invocation.output.find("\"boundary\":\"core_throughput\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"commands\":\"32\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"latency_ns\":null"), std::string::npos);
  EXPECT_NE(
      invocation.output.find("\"measurement_parameters\":{\"instrument_count\":\"1\","
                             "\"measured_start_active_order_count\":\"0\",\"sweep_depth\":\"0\"}"),
      std::string::npos);
  EXPECT_NE(invocation.output.find("\"timed_input_kind\":\"none\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"timed_input_sha256\":null"), std::string::npos);
  EXPECT_NE(invocation.output.find("\"valid\":true"), std::string::npos);

  std::error_code cleanup_error;
  std::filesystem::remove(fixture->path, cleanup_error);
}

TEST(BenchmarkRunnerCliTest, EvidenceMismatchRetainsMeasuredCounts) {
  const auto workload_text = cancel_workload(32U);
  const auto fixture = make_cli_fixture("evidence-mismatch", workload_text, 0U, 0U, 32U);
  ASSERT_TRUE(fixture.has_value());
  auto arguments = timed_cli_arguments(*fixture, "throughput", 0U, 0U, 32U);
  replace_option(arguments, "--expected-events", std::to_string(fixture->events + 1U));

  const auto invocation = invoke_timed_cli(arguments);

  EXPECT_EQ(invocation.exit_code, benchmark_invalid_observation_exit_code);
  EXPECT_TRUE(invocation.error.empty());
  EXPECT_NE(invocation.output.find("\"failure_reason\":\"event_count_mismatch\""),
            std::string::npos);
  EXPECT_NE(invocation.output.find("\"commands\":\"32\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"events\":\"" + std::to_string(fixture->events) + "\""),
            std::string::npos);
  EXPECT_NE(invocation.output.find("\"rejected\":\"32\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"valid\":false"), std::string::npos);

  std::error_code cleanup_error;
  std::filesystem::remove(fixture->path, cleanup_error);
}

TEST(BenchmarkRunnerCliTest, RejectsNoncanonicalUnsignedCliValues) {
  const auto workload_text = cancel_workload(1U);
  const auto fixture = make_cli_fixture("noncanonical-cli", workload_text, 0U, 0U, 1U);
  ASSERT_TRUE(fixture.has_value());
  auto arguments = timed_cli_arguments(*fixture, "throughput", 0U, 0U, 1U);
  replace_option(arguments, "--block-index", "00");

  const auto invocation = invoke_timed_cli(arguments);

  EXPECT_EQ(invocation.exit_code, benchmark_usage_exit_code);
  EXPECT_TRUE(invocation.output.empty());
  EXPECT_NE(invocation.error.find("usage: atlas_bench_runner"), std::string::npos);

  std::error_code cleanup_error;
  std::filesystem::remove(fixture->path, cleanup_error);
}

TEST(BenchmarkRunnerCliTest, RejectsNoncanonicalWorkloadManifestDigest) {
  const auto workload_text = cancel_workload(1U);
  const auto fixture = make_cli_fixture("manifest-digest", workload_text, 0U, 0U, 1U);
  ASSERT_TRUE(fixture.has_value());
  auto arguments = timed_cli_arguments(*fixture, "throughput", 0U, 0U, 1U);
  replace_option(arguments, "--workload-manifest-sha256", std::string(64U, 'A'));

  const auto invocation = invoke_timed_cli(arguments);

  EXPECT_EQ(invocation.exit_code, benchmark_usage_exit_code);
  EXPECT_TRUE(invocation.output.empty());
  EXPECT_NE(invocation.error.find("usage: atlas_bench_runner"), std::string::npos);

  std::error_code cleanup_error;
  std::filesystem::remove(fixture->path, cleanup_error);
}

TEST(BenchmarkRunnerCliTest, RejectsDuplicateOrUnsafeMeasurementParameters) {
  const auto workload_text = cancel_workload(1U);
  const auto fixture = make_cli_fixture("invalid-parameters", workload_text, 0U, 0U, 1U);
  ASSERT_TRUE(fixture.has_value());
  constexpr std::array invalid_parameters{
      "instrument_count=2", "_hidden=1",          "empty=",
      "space=two words",    "slash=path/value",   "backslash=path\\value",
      "colon=host:1",       "email=user@example", "quote=\"value\"",
  };

  for (const auto invalid : invalid_parameters) {
    auto arguments = timed_cli_arguments(*fixture, "throughput", 0U, 0U, 1U);
    arguments.emplace_back("--measurement-parameter");
    arguments.emplace_back(invalid);

    const auto invocation = invoke_timed_cli(arguments);

    EXPECT_EQ(invocation.exit_code, benchmark_usage_exit_code) << invalid;
    EXPECT_TRUE(invocation.output.empty()) << invalid;
    EXPECT_NE(invocation.error.find("usage: atlas_bench_runner"), std::string::npos) << invalid;
  }

  std::error_code cleanup_error;
  std::filesystem::remove(fixture->path, cleanup_error);
}

TEST(BenchmarkRunnerCliTest, RejectsSuiteLabelsLongerThanThirtyTwoCharacters) {
  const auto workload_text = cancel_workload(1U);
  const auto fixture = make_cli_fixture("suite-label-bound", workload_text, 0U, 0U, 1U);
  ASSERT_TRUE(fixture.has_value());
  auto arguments = timed_cli_arguments(*fixture, "throughput", 0U, 0U, 1U);
  replace_option(arguments, "--suite-label", std::string(33U, 's'));

  const auto invocation = invoke_timed_cli(arguments);

  EXPECT_EQ(invocation.exit_code, benchmark_usage_exit_code);
  EXPECT_TRUE(invocation.output.empty());
  EXPECT_NE(invocation.error.find("usage: atlas_bench_runner"), std::string::npos);

  std::error_code cleanup_error;
  std::filesystem::remove(fixture->path, cleanup_error);
}

TEST(BenchmarkRunnerCliTest, LatencySamplesEveryThirtySecondMeasuredCommand) {
  struct SampleCase final {
    std::size_t measured;
    std::size_t expected_samples;
  };
  constexpr std::array cases{
      SampleCase{.measured = 31U, .expected_samples = 0U},
      SampleCase{.measured = 32U, .expected_samples = 1U},
      SampleCase{.measured = 63U, .expected_samples = 1U},
      SampleCase{.measured = 64U, .expected_samples = 2U},
  };

  for (const auto& sample_case : cases) {
    const auto workload_text = cancel_workload(sample_case.measured);
    const auto fixture = make_cli_fixture("latency-" + std::to_string(sample_case.measured),
                                          workload_text, 0U, 0U, sample_case.measured);
    ASSERT_TRUE(fixture.has_value());
    auto arguments = timed_cli_arguments(*fixture, "latency", 0U, 0U, sample_case.measured);

    const auto invocation = invoke_timed_cli(arguments);

    EXPECT_EQ(invocation.exit_code, benchmark_success_exit_code);
    EXPECT_TRUE(invocation.error.empty());
    EXPECT_NE(invocation.output.find("\"boundary\":\"core_latency\""), std::string::npos);
    EXPECT_EQ(latency_sample_count(invocation.output), sample_case.expected_samples);
    EXPECT_NE(invocation.output.find("\"valid\":true"), std::string::npos);

    std::error_code cleanup_error;
    std::filesystem::remove(fixture->path, cleanup_error);
  }
}

TEST(BenchmarkRunnerCliTest, ReplayFastAndVerifyProduceBoundCanonicalObservations) {
  constexpr std::string_view workload_text{
      "ATLAS_DIFF_V2 10 1 2 0\n"
      "I 1 1000 1 10\n"
      "N 1 1 1 1 1 1 1 100 1\n"
      "N 1 2 99 1 1 1 1 100 1\n"};
  const auto fixture = make_cli_fixture("replay-success", workload_text, 0U, 0U, 2U);
  ASSERT_TRUE(fixture.has_value());
  const auto log_path = make_replay_log("replay-success", workload_text, 2U);
  ASSERT_TRUE(log_path.has_value());
  const auto log_digest = sha256_file(*log_path);
  ASSERT_FALSE(log_digest.empty());

  for (const auto mode : {"replay-fast", "replay-verify"}) {
    auto arguments = replay_cli_arguments(*fixture, *log_path, mode, 2U);

    const auto invocation = invoke_timed_cli(arguments);

    EXPECT_EQ(invocation.exit_code, benchmark_success_exit_code) << mode;
    EXPECT_TRUE(invocation.error.empty()) << mode;
    EXPECT_NE(
        invocation.output.find(
            "\"boundary\":\"" +
            std::string{mode == std::string_view{"replay-fast"} ? "replay_fast" : "replay_verify"} +
            "\""),
        std::string::npos)
        << mode;
    EXPECT_NE(invocation.output.find("\"commands\":\"2\""), std::string::npos) << mode;
    EXPECT_NE(invocation.output.find("\"committed\":\"1\""), std::string::npos) << mode;
    EXPECT_NE(invocation.output.find("\"rejected\":\"1\""), std::string::npos) << mode;
    EXPECT_NE(invocation.output.find("\"timed_input_kind\":\"atlslg01\""), std::string::npos)
        << mode;
    EXPECT_NE(invocation.output.find("\"timed_input_sha256\":\"" + log_digest + "\""),
              std::string::npos)
        << mode;
    EXPECT_NE(invocation.output.find("\"final_digest\":\"" + fixture->final_digest + "\""),
              std::string::npos)
        << mode;
    EXPECT_NE(invocation.output.find("\"valid\":true"), std::string::npos) << mode;
  }

  std::error_code cleanup_error;
  std::filesystem::remove(fixture->path, cleanup_error);
  std::filesystem::remove(*log_path, cleanup_error);
}

TEST(BenchmarkRunnerCliTest, ReplayDigestMismatchIsRejectedBeforeTiming) {
  constexpr std::string_view workload_text{
      "ATLAS_DIFF_V2 10 1 1 0\n"
      "I 1 1000 1 10\n"
      "N 1 1 1 1 1 1 1 100 1\n"};
  const auto fixture = make_cli_fixture("replay-digest", workload_text, 0U, 0U, 1U);
  ASSERT_TRUE(fixture.has_value());
  const auto log_path = make_replay_log("replay-digest", workload_text, 1U);
  ASSERT_TRUE(log_path.has_value());
  const auto real_digest = sha256_file(*log_path);
  auto arguments = replay_cli_arguments(*fixture, *log_path, "replay-fast", 1U);
  const std::string wrong_digest(64U, '0');
  replace_option(arguments, "--replay-log-sha256", wrong_digest);
  const auto timed_parameter =
      std::find(arguments.begin(), arguments.end(), "timed_input_sha256=" + real_digest);
  ASSERT_NE(timed_parameter, arguments.end());
  *timed_parameter = "timed_input_sha256=" + wrong_digest;

  const auto invocation = invoke_timed_cli(arguments);

  EXPECT_EQ(invocation.exit_code, benchmark_invalid_observation_exit_code);
  EXPECT_TRUE(invocation.error.empty());
  EXPECT_NE(invocation.output.find("\"failure_reason\":\"replay_log_digest_mismatch\""),
            std::string::npos);
  EXPECT_NE(invocation.output.find("\"commands\":\"0\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"elapsed_ns\":\"0\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"timed_input_sha256\":\"" + wrong_digest + "\""),
            std::string::npos);

  std::error_code cleanup_error;
  std::filesystem::remove(fixture->path, cleanup_error);
  std::filesystem::remove(*log_path, cleanup_error);
}

TEST(BenchmarkRunnerCliTest, ReplayRequiresExactModeBoundMeasurementParameters) {
  constexpr std::string_view workload_text{
      "ATLAS_DIFF_V2 10 1 1 0\n"
      "I 1 1000 1 10\n"
      "N 1 1 1 1 1 1 1 100 1\n"};
  const auto fixture = make_cli_fixture("replay-parameters", workload_text, 0U, 0U, 1U);
  ASSERT_TRUE(fixture.has_value());
  const auto log_path = make_replay_log("replay-parameters", workload_text, 1U);
  ASSERT_TRUE(log_path.has_value());

  auto wrong_mode = replay_cli_arguments(*fixture, *log_path, "replay-fast", 1U);
  const auto replay_mode = std::find(wrong_mode.begin(), wrong_mode.end(), "replay_mode=fast");
  ASSERT_NE(replay_mode, wrong_mode.end());
  *replay_mode = "replay_mode=verify";
  const auto wrong_mode_invocation = invoke_timed_cli(wrong_mode);
  EXPECT_EQ(wrong_mode_invocation.exit_code, benchmark_usage_exit_code);
  EXPECT_TRUE(wrong_mode_invocation.output.empty());

  auto extra_parameter = replay_cli_arguments(*fixture, *log_path, "replay-fast", 1U);
  extra_parameter.emplace_back("--measurement-parameter");
  extra_parameter.emplace_back("unexpected=1");
  const auto extra_parameter_invocation = invoke_timed_cli(extra_parameter);
  EXPECT_EQ(extra_parameter_invocation.exit_code, benchmark_usage_exit_code);
  EXPECT_TRUE(extra_parameter_invocation.output.empty());

  std::error_code cleanup_error;
  std::filesystem::remove(fixture->path, cleanup_error);
  std::filesystem::remove(*log_path, cleanup_error);
}

TEST(BenchmarkRunnerCliTest, ReplayCorruptionAndIoFailureHaveDistinctExitClasses) {
  constexpr std::string_view workload_text{
      "ATLAS_DIFF_V2 10 1 1 0\n"
      "I 1 1000 1 10\n"
      "N 1 1 1 1 1 1 1 100 1\n"};
  const auto fixture = make_cli_fixture("replay-errors", workload_text, 0U, 0U, 1U);
  ASSERT_TRUE(fixture.has_value());
  const auto log_path = make_replay_log("replay-errors", workload_text, 1U);
  ASSERT_TRUE(log_path.has_value());
  ASSERT_TRUE(corrupt_last_log_byte(*log_path));
  auto corrupted_arguments = replay_cli_arguments(*fixture, *log_path, "replay-verify", 1U);

  const auto corrupted = invoke_timed_cli(corrupted_arguments);

  EXPECT_EQ(corrupted.exit_code, benchmark_invalid_observation_exit_code);
  EXPECT_NE(corrupted.output.find("\"failure_reason\":\"replay_log_invalid\""), std::string::npos);
  EXPECT_NE(corrupted.output.find("\"elapsed_ns\":\"0\""), std::string::npos);

  const auto missing_path =
      std::filesystem::path{testing::TempDir()} / "atlaslob-benchmark-missing.atlslg";
  std::error_code cleanup_error;
  static_cast<void>(std::filesystem::remove(missing_path, cleanup_error));
  auto missing_arguments = timed_cli_arguments(*fixture, "replay-fast", 0U, 0U, 1U);
  const std::string missing_digest(64U, '0');
  missing_arguments.emplace_back("--replay-log");
  missing_arguments.emplace_back(missing_path.string());
  missing_arguments.emplace_back("--replay-log-sha256");
  missing_arguments.emplace_back(missing_digest);
  missing_arguments.emplace_back("--measurement-parameter");
  missing_arguments.emplace_back("record_count=1");
  missing_arguments.emplace_back("--measurement-parameter");
  missing_arguments.emplace_back("cache_policy=warm_page_cache");
  missing_arguments.emplace_back("--measurement-parameter");
  missing_arguments.emplace_back("replay_mode=fast");
  missing_arguments.emplace_back("--measurement-parameter");
  missing_arguments.emplace_back("timed_input_sha256=" + missing_digest);

  const auto missing = invoke_timed_cli(missing_arguments);

  EXPECT_EQ(missing.exit_code, benchmark_operational_error_exit_code);
  EXPECT_NE(missing.output.find("\"failure_reason\":\"replay_log_io_failure\""), std::string::npos);

  std::filesystem::remove(fixture->path, cleanup_error);
  std::filesystem::remove(*log_path, cleanup_error);
}

TEST(BenchmarkRunnerCliTest, CoreModesRejectReplayOnlyArguments) {
  const auto workload_text = cancel_workload(1U);
  const auto fixture = make_cli_fixture("core-replay-args", workload_text, 0U, 0U, 1U);
  ASSERT_TRUE(fixture.has_value());
  auto arguments = timed_cli_arguments(*fixture, "throughput", 0U, 0U, 1U);
  arguments.emplace_back("--replay-log");
  arguments.emplace_back("unused.log");
  arguments.emplace_back("--replay-log-sha256");
  arguments.emplace_back(std::string(64U, '0'));

  const auto invocation = invoke_timed_cli(arguments);

  EXPECT_EQ(invocation.exit_code, benchmark_usage_exit_code);
  EXPECT_TRUE(invocation.output.empty());
  EXPECT_NE(invocation.error.find("usage: atlas_bench_runner"), std::string::npos);

  std::error_code cleanup_error;
  std::filesystem::remove(fixture->path, cleanup_error);
}

TEST(BenchmarkRunnerCliTest, AllocationFlavorRejectsTimedModeBeforeCallingHooks) {
  allocation_begin_calls = 0U;
  allocation_end_calls = 0U;
  const AllocationHooks hooks{
      .begin = &begin_fake_allocation_tracking,
      .end = &end_fake_allocation_tracking,
  };
  std::array arguments{
      const_cast<char*>("atlas_bench_alloc_runner"),
      const_cast<char*>("--mode"),
      const_cast<char*>("throughput"),
  };

  testing::internal::CaptureStderr();
  const auto exit_code = run_benchmark_cli(static_cast<int>(arguments.size()), arguments.data(),
                                           RunnerFlavor::allocation, hooks);
  const auto error = testing::internal::GetCapturedStderr();

  EXPECT_EQ(exit_code, benchmark_usage_exit_code);
  EXPECT_EQ(allocation_begin_calls, 0U);
  EXPECT_EQ(allocation_end_calls, 0U);
  EXPECT_NE(error.find("usage: atlas_bench_alloc_runner"), std::string::npos);
}

TEST(BenchmarkRunnerCliTest, AllocationFlavorProducesValidUntimedObservation) {
  constexpr std::string_view workload_text{
      "ATLAS_DIFF_V2 10 1 1 0\n"
      "I 1 1000 1 10\n"
      "N 1 1 1 1 1 1 1 100 1\n"};
  const auto workload_path =
      std::filesystem::path{testing::TempDir()} / "atlaslob-benchmark-allocation-smoke.atlas";
  {
    std::ofstream output{workload_path, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output);
    output.write(workload_text.data(), static_cast<std::streamsize>(workload_text.size()));
    ASSERT_TRUE(output);
  }

  const std::array catalog{
      InstrumentConfig{
          .instrument_id = domain::InstrumentId{1U},
          .matching =
              {
                  .max_order_quantity = domain::Quantity{1'000U},
                  .tick_increment = domain::PriceTicks{1},
                  .max_active_orders = 10U,
              },
      },
  };
  MultiInstrumentEngine engine{
      std::span<const InstrumentConfig>{catalog},
      MultiInstrumentEngineConfig{.max_total_active_orders = 10U},
  };
  const domain::Command command{
      domain::NewOrder{
          .client_id = domain::ClientId{1U},
          .order_id = domain::OrderId{1U},
          .instrument_id = domain::InstrumentId{1U},
          .side = domain::Side::buy,
          .order_type = domain::OrderType::limit,
          .time_in_force = domain::TimeInForce::gtc,
          .limit_price = domain::PriceTicks{100},
          .quantity = domain::Quantity{1U},
      },
  };
  const auto expected_result = engine.execute(command);
  const auto* const expected_batch = expected_result.batch();
  ASSERT_NE(expected_batch, nullptr);
  utility::Sha256 event_hash;
  event_hash.update(atlaslob::event_digest(*expected_batch).bytes);

  const auto executable_path = std::filesystem::absolute(testing::internal::GetArgvs().front());
  const auto binary_digest = sha256_file(executable_path);
  ASSERT_FALSE(binary_digest.empty());
  const auto workload_digest = sha256_text(workload_text);
  const auto event_digest = event_hash.finish().hex();
  const auto final_digest = engine.state_digest().hex();
  const std::string environment_digest(64U, '3');
  const std::string host_context_digest(64U, '4');

  std::vector<std::string> arguments{
      executable_path.string(),
      "--workload",
      workload_path.string(),
      "--workload-id",
      "W00",
      "--workload-manifest-sha256",
      std::string(64U, '7'),
      "--workload-sha256",
      workload_digest,
      "--binary-sha256",
      binary_digest,
      "--environment-sha256",
      environment_digest,
      "--host-context-sha256",
      host_context_digest,
      "--run-label",
      "smoke",
      "--suite-label",
      "smoke-suite",
      "--variant",
      "standalone",
      "--block-index",
      "0",
      "--block-position",
      "0",
      "--preload-count",
      "0",
      "--warmup-count",
      "0",
      "--measured-count",
      "1",
      "--expected-events",
      std::to_string(expected_batch->size()),
      "--expected-committed",
      "1",
      "--expected-rejected",
      "0",
      "--expected-engine-errors",
      "0",
      "--expected-event-digest",
      event_digest,
      "--expected-final-digest",
      final_digest,
  };
  const auto invoke = [&arguments]() {
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(arguments.size());
    for (auto& argument : arguments) {
      argument_pointers.push_back(argument.data());
    }
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();
    const auto exit_code = run_benchmark_cli(static_cast<int>(argument_pointers.size()),
                                             argument_pointers.data(), RunnerFlavor::allocation,
                                             AllocationHooks{
                                                 .begin = &begin_fake_allocation_tracking,
                                                 .end = &end_fake_allocation_tracking,
                                             });
    auto error = testing::internal::GetCapturedStderr();
    auto output = testing::internal::GetCapturedStdout();
    return CliInvocation{exit_code, std::move(output), std::move(error)};
  };

  allocation_begin_calls = 0U;
  allocation_end_calls = 0U;
  const auto ordinary = invoke();
  EXPECT_EQ(ordinary.exit_code, benchmark_success_exit_code);
  EXPECT_TRUE(ordinary.error.empty());

  arguments.emplace_back("--diagnostic-phases");
  arguments.emplace_back("yes");
  const auto diagnostic = invoke();

  EXPECT_EQ(diagnostic.exit_code, benchmark_success_exit_code);
  EXPECT_EQ(diagnostic.output.find("ATLAS_DIAGNOSTIC_PHASE"), std::string::npos);
  EXPECT_NE(ordinary.output.find("\"allocation_count\":\"2\""), std::string::npos);
  EXPECT_EQ(allocation_begin_calls, 2U);
  EXPECT_EQ(allocation_end_calls, 2U);
  EXPECT_NE(diagnostic.output.find("\"boundary\":\"core_allocation\""), std::string::npos);
  EXPECT_NE(diagnostic.output.find("\"elapsed_ns\":\"0\""), std::string::npos);
  EXPECT_NE(diagnostic.output.find("\"allocation_count\":\"2\""), std::string::npos);
  EXPECT_NE(diagnostic.output.find("\"valid\":true"), std::string::npos);
  const auto rss_before = quoted_decimal_field(diagnostic.output, "rss_before_bytes");
  const auto rss_after = quoted_decimal_field(diagnostic.output, "rss_after_bytes");
  const auto peak_rss = quoted_decimal_field(diagnostic.output, "peak_rss_bytes");
  ASSERT_TRUE(rss_before.has_value());
  ASSERT_TRUE(rss_after.has_value());
  ASSERT_TRUE(peak_rss.has_value());
  EXPECT_GE(*peak_rss, std::max(*rss_before, *rss_after));
  const auto measured_enter =
      diagnostic.error.find("ATLAS_DIAGNOSTIC_PHASE measured-region-enter\n");
  const auto measured_exit = diagnostic.error.find("ATLAS_DIAGNOSTIC_PHASE measured-region-exit\n");
  EXPECT_NE(measured_enter, std::string::npos);
  EXPECT_NE(measured_exit, std::string::npos);
  EXPECT_LT(measured_enter, measured_exit);
  EXPECT_NE(diagnostic.error.find("ATLAS_DIAGNOSTIC_PHASE validation-replay-enter\n"),
            std::string::npos);
  EXPECT_NE(diagnostic.error.find("ATLAS_DIAGNOSTIC_PHASE observation-ready\n"), std::string::npos);

  std::error_code cleanup_error;
  std::filesystem::remove(workload_path, cleanup_error);
}

TEST(BenchmarkRunnerCliTest, AllocationFlavorExecutesRepresentativeW01Churn) {
  constexpr std::size_t active_orders = 64U;
  constexpr std::uint64_t preload = active_orders;
  constexpr std::uint64_t warmup = 64U;
  constexpr std::uint64_t measured = 128U;
  const auto workload = w01_churn_workload(active_orders, warmup + measured);
  const auto fixture =
      make_cli_fixture("allocation-w01-churn", workload, preload, warmup, measured);
  ASSERT_TRUE(fixture.has_value());

  auto arguments = timed_cli_arguments(*fixture, "throughput", preload, warmup, measured);
  ASSERT_GE(arguments.size(), 2U);
  ASSERT_EQ(arguments[arguments.size() - 2U], "--mode");
  ASSERT_EQ(arguments.back(), "throughput");
  arguments.resize(arguments.size() - 2U);
  replace_option(arguments, "--measurement-parameter", "sweep_depth=0");
  const auto measured_active =
      std::find(arguments.begin(), arguments.end(), "measured_start_active_order_count=0");
  ASSERT_NE(measured_active, arguments.end());
  *measured_active = "measured_start_active_order_count=64";

  allocation_begin_calls = 0U;
  allocation_end_calls = 0U;
  const auto invocation = invoke_allocation_cli(arguments);

  EXPECT_EQ(invocation.exit_code, benchmark_success_exit_code);
  EXPECT_TRUE(invocation.error.empty());
  EXPECT_EQ(allocation_begin_calls, 1U);
  EXPECT_EQ(allocation_end_calls, 1U);
  EXPECT_NE(invocation.output.find("\"boundary\":\"core_allocation\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"commands\":\"128\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"allocation_count\":\"2\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"deallocation_count\":\"1\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"allocated_bytes\":\"64\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"live_bytes\":\"32\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"peak_live_bytes\":\"64\""), std::string::npos);
  EXPECT_NE(invocation.output.find("\"valid\":true"), std::string::npos);

  std::error_code cleanup_error;
  std::filesystem::remove(fixture->path, cleanup_error);
}

TEST(BenchmarkLogMaterializerTest, CreatesAPreviouslyMissingDestination) {
  constexpr std::string_view workload_text{
      "ATLAS_DIFF_V2 10 1 1 0\n"
      "I 1 1000 1 10\n"
      "N 1 1 1 1 1 1 1 100 1\n"};
  const auto workload_path =
      std::filesystem::path{testing::TempDir()} / "atlaslob-materializer-missing-path.atlas";
  const auto output_path =
      std::filesystem::path{testing::TempDir()} / "atlaslob-materializer-missing-path.atlslg";
  {
    std::error_code cleanup_error;
    static_cast<void>(std::filesystem::remove(output_path, cleanup_error));
    std::ofstream output{workload_path, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output);
    output.write(workload_text.data(), static_cast<std::streamsize>(workload_text.size()));
    ASSERT_TRUE(output);
  }

  const auto workload_digest = sha256_text(workload_text);
  const auto workload_path_text = workload_path.string();
  const auto output_path_text = output_path.string();
  const std::array<std::string_view, 7U> arguments{
      "atlas_bench_log_materializer",
      "--workload",
      workload_path_text,
      "--workload-sha256",
      workload_digest,
      "--output",
      output_path_text,
  };

  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  const auto exit_code = run_log_materializer_cli(arguments);
  const auto error = testing::internal::GetCapturedStderr();
  const auto output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(exit_code, materializer_success_exit_code);
  EXPECT_TRUE(error.empty());
  EXPECT_TRUE(std::filesystem::is_regular_file(output_path));
  EXPECT_NE(output.find("\"schema\":\"ATLAS_BENCH_LOG_MATERIALIZATION_V1\""), std::string::npos);

  std::error_code cleanup_error;
  static_cast<void>(std::filesystem::remove(workload_path, cleanup_error));
  cleanup_error.clear();
  static_cast<void>(std::filesystem::remove(output_path, cleanup_error));
}

}  // namespace
}  // namespace atlaslob::benchmark
