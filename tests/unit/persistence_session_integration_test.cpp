#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

#include "atlaslob/domain/commands.hpp"
#include "atlaslob/persistence/inspection.hpp"
#include "atlaslob/persistence/logged_engine.hpp"
#include "atlaslob/persistence/replay.hpp"

namespace atlaslob::persistence::tests {
namespace {

class TemporaryLog final {
 public:
  explicit TemporaryLog(std::string_view label) {
    static std::uint64_t counter{};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("atlaslob_persistence_" + std::to_string(stamp) + "_" + std::to_string(++counter) +
             "_" + std::string{label});
  }

  ~TemporaryLog() {
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(path_, ignored));
  }

  TemporaryLog(const TemporaryLog&) = delete;
  TemporaryLog& operator=(const TemporaryLog&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] InstrumentConfig instrument(std::uint32_t value) {
  return {
      .instrument_id = domain::InstrumentId{value},
      .matching =
          {
              .max_order_quantity = domain::Quantity{std::numeric_limits<std::uint64_t>::max()},
              .tick_increment = domain::PriceTicks{1},
              .max_active_orders = 32U,
          },
  };
}

[[nodiscard]] domain::NewOrder new_order(std::uint64_t order_id, std::uint32_t instrument_id) {
  return {
      .client_id = domain::ClientId{11U},
      .order_id = domain::OrderId{order_id},
      .instrument_id = domain::InstrumentId{instrument_id},
      .side = domain::Side::buy,
      .order_type = domain::OrderType::limit,
      .time_in_force = domain::TimeInForce::gtc,
      .limit_price = domain::PriceTicks{100},
      .quantity = domain::Quantity{5U},
  };
}

[[nodiscard]] LogId fixed_log_id() {
  LogId result;
  for (std::size_t index = 0U; index < result.bytes.size(); ++index) {
    result.bytes[index] = static_cast<std::uint8_t>(index + 1U);
  }
  return result;
}

TEST(PersistenceSessionIntegration, FileBackedSubmissionInspectionAndVerifiedReplayAgree) {
  TemporaryLog log{"verified.log"};
  const std::array catalog{instrument(7U), instrument(9U)};
  auto opened =
      LoggedEngine::create_new(log.path(), catalog, fixed_log_id(),
                               MultiInstrumentEngineConfig{.max_total_active_orders = 64U});
  ASSERT_TRUE(opened) << opened.error.system_error.message();

  const auto committed = opened.engine->submit(new_order(1U, 7U));
  ASSERT_TRUE(committed);
  ASSERT_TRUE(committed.engine_result->committed());

  const auto rejected = opened.engine->submit(new_order(2U, 99U));
  ASSERT_TRUE(rejected);
  ASSERT_TRUE(rejected.engine_result->rejected());

  const auto expected_digest = opened.engine->engine().state_digest();
  EXPECT_EQ(opened.engine->engine().next_sequence(), domain::Sequence{3U});
  EXPECT_EQ(opened.engine->log_id(), fixed_log_id());
  opened.engine.reset();

  const auto inspected = inspect_log(log.path(), true);
  ASSERT_TRUE(inspected.clean());
  ASSERT_TRUE(inspected.header.has_value());
  ASSERT_TRUE(inspected.records.has_value());
  EXPECT_EQ(inspected.header->catalog.size(), 2U);
  EXPECT_EQ(inspected.records_scanned, 2U);
  EXPECT_EQ(inspected.last_sequence, domain::Sequence{2U});
  EXPECT_EQ(inspected.records->at(0).outcome, RecordOutcome::committed);
  EXPECT_EQ(inspected.records->at(1).outcome, RecordOutcome::rejected);
  EXPECT_EQ(inspected.records->at(1).rejection_reason, domain::RejectReason::unknown_instrument);

  const auto replayed = replay_log(log.path(), ReplayOptions{.mode = ReplayMode::verify});
  ASSERT_TRUE(replayed);
  ASSERT_NE(replayed.engine, nullptr);
  EXPECT_EQ(replayed.report.records_replayed, 2U);
  EXPECT_EQ(replayed.report.committed, 1U);
  EXPECT_EQ(replayed.report.rejected, 1U);
  EXPECT_EQ(replayed.report.final_state_digest, expected_digest);
  EXPECT_EQ(replayed.engine->state_digest(), expected_digest);
  EXPECT_EQ(replayed.engine->next_sequence(), domain::Sequence{3U});
}

TEST(PersistenceSessionIntegration, ExistingDestinationIsNeverOverwritten) {
  TemporaryLog log{"exclusive.log"};
  const std::array catalog{instrument(7U)};
  auto first = LoggedEngine::create_new(log.path(), catalog, fixed_log_id());
  ASSERT_TRUE(first);
  const auto original_size = first.engine->log_offset();

  auto second = LoggedEngine::create_new(log.path(), catalog, fixed_log_id());
  EXPECT_FALSE(second);
  EXPECT_EQ(second.error.category, LogErrorCategory::io_failure);
  EXPECT_EQ(first.engine->log_offset(), original_size);
  EXPECT_FALSE(first.engine->poisoned());
}

TEST(PersistenceSessionIntegration, SnapshotBoundsDoNotAffectLogCreationOrSubmission) {
  TemporaryLog log{"snapshot-bound-independent.log"};
  const std::array catalog{instrument(7U)};
  LoggedEngineOptions options;
  options.codec_limits.max_snapshot_bytes = 0U;
  ASSERT_TRUE(options.valid());

  auto opened = LoggedEngine::create_new(log.path(), catalog, fixed_log_id(), {}, options);

  ASSERT_TRUE(opened) << opened.error.system_error.message();
  const auto submitted = opened.engine->submit(new_order(1U, 7U));
  ASSERT_TRUE(submitted);
  EXPECT_TRUE(submitted.engine_result->committed());
}

}  // namespace
}  // namespace atlaslob::persistence::tests
