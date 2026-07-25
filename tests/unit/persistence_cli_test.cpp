#include "persistence_cli.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "atlaslob/domain/commands.hpp"
#include "atlaslob/persistence/logged_engine.hpp"
#include "log_io.hpp"

namespace atlaslob::persistence::tests {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    static std::uint64_t counter{};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("atlaslob_cli_" + std::to_string(stamp) + "_" + std::to_string(++counter));
    std::filesystem::create_directory(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove_all(path_, ignored));
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] std::filesystem::path file(std::string_view name) const { return path_ / name; }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] InstrumentConfig instrument() {
  return {
      .instrument_id = domain::InstrumentId{7U},
      .matching =
          {
              .max_order_quantity = domain::Quantity{std::numeric_limits<std::uint64_t>::max()},
              .tick_increment = domain::PriceTicks{1},
              .max_active_orders = 32U,
          },
  };
}

[[nodiscard]] domain::NewOrder order() {
  return {
      .client_id = domain::ClientId{11U},
      .order_id = domain::OrderId{1U},
      .instrument_id = domain::InstrumentId{7U},
      .side = domain::Side::buy,
      .order_type = domain::OrderType::limit,
      .time_in_force = domain::TimeInForce::gtc,
      .limit_price = domain::PriceTicks{100},
      .quantity = domain::Quantity{5U},
  };
}

struct CliResult final {
  int exit_code{};
  std::string output;
  std::string error;
};

class RejectingStreamBuffer final : public std::streambuf {
 protected:
  int_type overflow(int_type) override { return traits_type::eof(); }

  std::streamsize xsputn(const char_type*, std::streamsize) override { return 0; }
};

[[nodiscard]] CliResult inspect(const std::vector<std::string>& owned_arguments) {
  std::vector<std::string_view> arguments;
  arguments.reserve(owned_arguments.size());
  for (const auto& argument : owned_arguments) {
    arguments.push_back(argument);
  }
  std::ostringstream output;
  std::ostringstream error;
  const int exit_code = detail::run_atlas_inspect(arguments, output, error);
  return {
      .exit_code = exit_code,
      .output = output.str(),
      .error = error.str(),
  };
}

[[nodiscard]] std::error_code fail_cli_cleanup(const std::filesystem::path&) noexcept {
  return std::make_error_code(std::errc::permission_denied);
}

class RemoveHookReset final {
 public:
  RemoveHookReset() = default;
  RemoveHookReset(const RemoveHookReset&) = delete;
  RemoveHookReset& operator=(const RemoveHookReset&) = delete;
  ~RemoveHookReset() { detail::set_remove_file_hook_for_testing(nullptr); }
};

[[nodiscard]] CliResult replay(const std::vector<std::string>& owned_arguments) {
  std::vector<std::string_view> arguments;
  arguments.reserve(owned_arguments.size());
  for (const auto& argument : owned_arguments) {
    arguments.push_back(argument);
  }
  std::ostringstream output;
  std::ostringstream error;
  const int exit_code = detail::run_atlas_replay(arguments, output, error);
  return {
      .exit_code = exit_code,
      .output = output.str(),
      .error = error.str(),
  };
}

void corrupt_final_byte(const std::filesystem::path& path) {
  std::fstream stream{path, std::ios::binary | std::ios::in | std::ios::out};
  ASSERT_TRUE(stream);
  stream.seekg(-1, std::ios::end);
  char byte{};
  stream.read(&byte, 1);
  ASSERT_TRUE(stream);
  byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x01U);
  stream.seekp(-1, std::ios::end);
  stream.write(&byte, 1);
  ASSERT_TRUE(stream);
}

TEST(PersistenceCli, ExitCodesAndReportsCoverCleanTornAndCorruptLogs) {
  TemporaryDirectory temporary;
  const auto clean = temporary.file("clean.log");
  const std::array catalog{instrument()};
  auto logged = LoggedEngine::create_new(clean, catalog);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(order()));
  logged.engine.reset();

  const auto clean_inspection =
      inspect({"atlas_inspect", "log", clean.string(), "--json", "--records"});
  EXPECT_EQ(clean_inspection.exit_code, 0);
  EXPECT_TRUE(clean_inspection.error.empty());
  EXPECT_NE(clean_inspection.output.find("ATLAS_LOG_REPORT_V1"), std::string::npos);
  EXPECT_NE(clean_inspection.output.find("\"records_scanned\":\"1\""), std::string::npos);
  EXPECT_EQ(clean_inspection.output.find(clean.string()), std::string::npos);

  const auto clean_refused = temporary.file("clean-refused.log");
  EXPECT_EQ(
      inspect({"atlas_inspect", "repair-tail", clean.string(), clean_refused.string(), "--json"})
          .exit_code,
      detail::cli_invalid_data_exit_code);
  EXPECT_FALSE(std::filesystem::exists(clean_refused));

  const auto verified = replay({"atlas_replay", clean.string(), "--mode", "verify", "--json"});
  EXPECT_EQ(verified.exit_code, 0);
  EXPECT_TRUE(verified.error.empty());
  EXPECT_NE(verified.output.find("ATLAS_REPLAY_REPORT_V1"), std::string::npos);
  EXPECT_NE(verified.output.find("\"status\":\"ok\""), std::string::npos);

  const auto torn = temporary.file("torn.log");
  std::filesystem::copy_file(clean, torn);
  const auto torn_size = std::filesystem::file_size(torn);
  ASSERT_GT(torn_size, 0U);
  std::filesystem::resize_file(torn, torn_size - 1U);

  EXPECT_EQ(inspect({"atlas_inspect", "log", torn.string(), "--json"}).exit_code,
            detail::cli_invalid_data_exit_code);
  EXPECT_EQ(replay({"atlas_replay", torn.string(), "--json"}).exit_code,
            detail::cli_invalid_data_exit_code);
  const auto prefix =
      replay({"atlas_replay", torn.string(), "--tail-policy", "valid-prefix", "--json"});
  EXPECT_EQ(prefix.exit_code, 0);
  EXPECT_NE(prefix.output.find("\"status\":\"warning\""), std::string::npos);

  const auto repaired = temporary.file("repaired.log");
  EXPECT_EQ(inspect({"atlas_inspect", "repair-tail", torn.string(), repaired.string(), "--json"})
                .exit_code,
            0);
  EXPECT_EQ(inspect({"atlas_inspect", "log", repaired.string(), "--json"}).exit_code, 0);

  const auto corrupt = temporary.file("corrupt.log");
  std::filesystem::copy_file(clean, corrupt);
  corrupt_final_byte(corrupt);
  EXPECT_EQ(inspect({"atlas_inspect", "log", corrupt.string(), "--json"}).exit_code,
            detail::cli_invalid_data_exit_code);
  const auto refused = temporary.file("refused.log");
  EXPECT_EQ(inspect({"atlas_inspect", "repair-tail", corrupt.string(), refused.string(), "--json"})
                .exit_code,
            detail::cli_invalid_data_exit_code);
  EXPECT_FALSE(std::filesystem::exists(refused));
}

TEST(PersistenceCli, UsageAndOperationalFailuresUseStableExitCodes) {
  TemporaryDirectory temporary;
  EXPECT_EQ(inspect({"atlas_inspect"}).exit_code, detail::cli_usage_exit_code);
  EXPECT_EQ(replay({"atlas_replay"}).exit_code, detail::cli_usage_exit_code);

  const auto missing = temporary.file("missing.log");
  EXPECT_EQ(inspect({"atlas_inspect", "log", missing.string(), "--json"}).exit_code,
            detail::cli_io_failure_exit_code);
  EXPECT_EQ(replay({"atlas_replay", missing.string(), "--json"}).exit_code,
            detail::cli_io_failure_exit_code);
  EXPECT_EQ(inspect({"atlas_inspect", "log", missing.string(), "--json", "--json"}).exit_code,
            detail::cli_usage_exit_code);
  EXPECT_EQ(
      replay({"atlas_replay", missing.string(), "--mode", "verify", "--mode", "fast"}).exit_code,
      detail::cli_usage_exit_code);
}

TEST(PersistenceCli, RepairCleanupFailureReportsAnArtifactWithoutLeakingAHostPath) {
  TemporaryDirectory temporary;
  const auto input = temporary.file("torn.log");
  const std::array catalog{instrument()};
  auto logged = LoggedEngine::create_new(input, catalog);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(order()));
  logged.engine.reset();
  const auto size = std::filesystem::file_size(input);
  ASSERT_GT(size, 0U);
  std::filesystem::resize_file(input, size - 1U);

  const auto existing_output = temporary.file("existing.log");
  {
    std::ofstream stream{existing_output, std::ios::binary};
    stream << "sentinel";
    ASSERT_TRUE(stream);
  }

  RemoveHookReset reset;
  detail::set_remove_file_hook_for_testing(fail_cli_cleanup);
  const auto result =
      inspect({"atlas_inspect", "repair-tail", input.string(), existing_output.string(), "--json"});
  EXPECT_EQ(result.exit_code, detail::cli_io_failure_exit_code);
  EXPECT_NE(result.output.find("\"unpublished_artifact_present\":true"), std::string::npos);
  EXPECT_EQ(result.output.find(existing_output.string()), std::string::npos);
  EXPECT_EQ(result.error, "repair-tail left an unpublished artifact beside the requested output\n");
  EXPECT_EQ(result.error.find(existing_output.string()), std::string::npos);

  detail::set_remove_file_hook_for_testing(nullptr);
  bool artifact_found{};
  for (const auto& entry : std::filesystem::directory_iterator{temporary.path()}) {
    const auto filename = entry.path().filename().string();
    if (filename.starts_with("existing.log.atlaslob-repair-tmp-")) {
      artifact_found = true;
      std::error_code ignored;
      static_cast<void>(std::filesystem::remove(entry.path(), ignored));
    }
  }
  EXPECT_TRUE(artifact_found);
}

TEST(PersistenceCli, OutputFailuresAreOperationalFailures) {
  TemporaryDirectory temporary;
  const auto clean = temporary.file("clean.log");
  const std::array catalog{instrument()};
  auto logged = LoggedEngine::create_new(clean, catalog);
  ASSERT_TRUE(logged);
  logged.engine.reset();

  const auto clean_text = clean.string();
  const std::array<std::string_view, 4U> arguments{"atlas_inspect", "log", clean_text, "--json"};
  RejectingStreamBuffer buffer;
  std::ostream rejected_output{&buffer};
  std::ostringstream error;
  EXPECT_EQ(detail::run_atlas_inspect(arguments, rejected_output, error),
            detail::cli_io_failure_exit_code);
}

TEST(PersistenceCli, SnapshotInspectionAndRecoveryUseVersionedReportsAndStableExitCodes) {
  TemporaryDirectory temporary;
  const auto clean = temporary.file("clean.log");
  const std::array catalog{instrument()};
  auto logged = LoggedEngine::create_new(clean, catalog);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(order()));
  const auto published = logged.engine->write_snapshot(temporary.file(".").parent_path());
  ASSERT_TRUE(published);
  logged.engine.reset();

  const auto inspected = inspect({"atlas_inspect", "snapshot", published.path.string(), "--json"});
  EXPECT_EQ(inspected.exit_code, 0);
  EXPECT_TRUE(inspected.error.empty());
  EXPECT_NE(inspected.output.find("\"schema\":\"ATLAS_SNAPSHOT_REPORT_V1\""), std::string::npos);
  EXPECT_NE(inspected.output.find("\"status\":\"ok\""), std::string::npos);
  EXPECT_EQ(inspected.output.find(published.path.string()), std::string::npos);

  const auto recovered = replay({"atlas_replay", clean.string(), "--snapshot",
                                 published.path.string(), "--mode", "verify", "--json"});
  EXPECT_EQ(recovered.exit_code, 0);
  EXPECT_TRUE(recovered.error.empty());
  EXPECT_NE(recovered.output.find("\"schema\":\"ATLAS_REPLAY_REPORT_V2\""), std::string::npos);
  EXPECT_NE(recovered.output.find("\"recovery_source\":\"explicit_snapshot\""), std::string::npos);
  EXPECT_EQ(recovered.output.find(published.path.string()), std::string::npos);

  EXPECT_EQ(replay({"atlas_replay", clean.string(), "--snapshot", published.path.string(),
                    "--snapshot-dir", temporary.file(".").parent_path().string()})
                .exit_code,
            detail::cli_usage_exit_code);
  EXPECT_EQ(
      inspect({"atlas_inspect", "snapshot", temporary.file("missing.snapshot").string(), "--json"})
          .exit_code,
      detail::cli_io_failure_exit_code);
  EXPECT_EQ(replay({"atlas_replay", clean.string(), "--snapshot",
                    temporary.file("missing.snapshot").string(), "--json"})
                .exit_code,
            detail::cli_io_failure_exit_code);
}

}  // namespace
}  // namespace atlaslob::persistence::tests
