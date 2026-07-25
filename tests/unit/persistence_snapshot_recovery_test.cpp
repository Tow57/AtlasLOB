#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "atlaslob/domain/commands.hpp"
#include "atlaslob/persistence/logged_engine.hpp"
#include "atlaslob/persistence/replay.hpp"
#include "atlaslob/persistence/snapshot_store.hpp"
#include "logged_engine_internal.hpp"
#include "multi_instrument_engine_access.hpp"
#include "reports.hpp"
#include "snapshot_codec.hpp"
#include "snapshot_store_internal.hpp"

namespace atlaslob::persistence::tests {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    static std::uint64_t counter{};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ =
        std::filesystem::temp_directory_path() /
        ("atlaslob_snapshot_recovery_" + std::to_string(stamp) + "_" + std::to_string(++counter));
    std::filesystem::create_directory(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove_all(path_, ignored));
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path file(std::string_view name) const { return path_ / name; }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] std::array<InstrumentConfig, 2U> catalog() {
  return {
      InstrumentConfig{
          .instrument_id = domain::InstrumentId{7U},
          .matching =
              {
                  .max_order_quantity = domain::Quantity{1'000U},
                  .tick_increment = domain::PriceTicks{5},
                  .max_active_orders = 32U,
              },
      },
      InstrumentConfig{
          .instrument_id = domain::InstrumentId{9U},
          .matching =
              {
                  .max_order_quantity = domain::Quantity{2'000U},
                  .tick_increment = domain::PriceTicks{10},
                  .max_active_orders = 32U,
              },
      },
  };
}

[[nodiscard]] domain::NewOrder limit_order(std::uint32_t client, std::uint64_t order,
                                           std::uint32_t instrument, domain::Side side,
                                           std::int64_t price, std::uint64_t quantity) {
  return {
      .client_id = domain::ClientId{client},
      .order_id = domain::OrderId{order},
      .instrument_id = domain::InstrumentId{instrument},
      .side = side,
      .order_type = domain::OrderType::limit,
      .time_in_force = domain::TimeInForce::gtc,
      .limit_price = domain::PriceTicks{price},
      .quantity = domain::Quantity{quantity},
  };
}

void flip_final_byte(const std::filesystem::path& path) {
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

void write_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  ASSERT_TRUE(stream);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(stream);
}

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::size_t temporary_file_count(const std::filesystem::path& directory) {
  std::size_t count{};
  for (const auto& entry : std::filesystem::directory_iterator{directory}) {
    if (entry.path().filename().string().find(".tmp-") != std::string::npos) {
      ++count;
    }
  }
  return count;
}

detail::SnapshotPublicationStage failing_publication_stage{
    detail::SnapshotPublicationStage::create_temporary};
bool publication_failure_enabled{};
bool cleanup_failure_enabled{};

[[nodiscard]] SnapshotError inject_publication_failure(detail::SnapshotPublicationStage stage,
                                                       std::uint64_t offset) noexcept {
  if ((publication_failure_enabled && stage == failing_publication_stage) ||
      (cleanup_failure_enabled && stage == detail::SnapshotPublicationStage::cleanup)) {
    return {
        .category = SnapshotErrorCategory::io_failure,
        .byte_offset = offset,
        .system_error = std::make_error_code(std::errc::io_error),
    };
  }
  return {};
}

[[nodiscard]] LogError inject_log_sync_failure(std::uint64_t offset) noexcept {
  return {
      .category = LogErrorCategory::io_failure,
      .byte_offset = offset,
      .system_error = std::make_error_code(std::errc::io_error),
  };
}

class FaultHooksGuard final {
 public:
  FaultHooksGuard() = default;
  FaultHooksGuard(const FaultHooksGuard&) = delete;
  FaultHooksGuard& operator=(const FaultHooksGuard&) = delete;
  ~FaultHooksGuard() {
    publication_failure_enabled = false;
    cleanup_failure_enabled = false;
    detail::set_snapshot_publication_hook_for_testing(nullptr);
    detail::set_logged_engine_synchronize_hook_for_testing(nullptr);
  }
};

TEST(PersistenceSnapshotPublication, PublishesAByteVerifiedUniqueCanonicalFile) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  const LogId log_id{
      .bytes = {0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U, 0x99U, 0xaaU, 0xbbU,
                0xccU, 0xddU, 0xeeU, 0xffU},
  };
  auto logged = LoggedEngine::create_new(log, instruments, log_id);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 5U)));
  ASSERT_TRUE(logged.engine->submit(limit_order(12U, 2U, 9U, domain::Side::sell, 200, 7U)));

  const auto before = logged.engine->engine().snapshot();
  const auto published = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(published);
  EXPECT_EQ(published.covered_sequence, domain::Sequence{2U});
  EXPECT_EQ(published.covered_log_byte_offset, logged.engine->log_offset());
  EXPECT_EQ(published.path.filename(),
            "atlaslob-00112233445566778899aabbccddeeff-00000000000000000002.snapshot");
  EXPECT_TRUE(std::filesystem::exists(published.path));
  EXPECT_EQ(temporary_file_count(temporary.path()), 0U);

  const auto inspected = inspect_snapshot(published.path);
  ASSERT_TRUE(inspected);
  EXPECT_EQ(inspected.input_bytes, published.encoded_bytes);
  EXPECT_EQ(inspected.snapshot->covered_sequence, before.last_sequence);
  EXPECT_EQ(inspected.snapshot->covered_log_byte_offset, published.covered_log_byte_offset);
  EXPECT_EQ(inspected.snapshot->state_digest, state_digest(before));
  ASSERT_EQ(inspected.snapshot->instruments.size(), 2U);
  EXPECT_TRUE(inspected.snapshot->instruments[1].bids.empty());
  EXPECT_EQ(inspected.snapshot->instruments[1].asks.size(), 1U);

  std::ifstream original_stream{published.path, std::ios::binary};
  const std::vector<char> original{std::istreambuf_iterator<char>{original_stream},
                                   std::istreambuf_iterator<char>{}};
  const auto duplicate = logged.engine->write_snapshot(temporary.path());
  EXPECT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error.category, SnapshotErrorCategory::io_failure);
  EXPECT_FALSE(duplicate.final_file_visible);
  EXPECT_EQ(temporary_file_count(temporary.path()), 0U);
  std::ifstream after_stream{published.path, std::ios::binary};
  const std::vector<char> after{std::istreambuf_iterator<char>{after_stream},
                                std::istreambuf_iterator<char>{}};
  EXPECT_EQ(after, original);
}

TEST(PersistenceSnapshotPublication, EveryFileOperationFailureIsAtomicAndRecoverable) {
  FaultHooksGuard hooks;
  detail::set_snapshot_publication_hook_for_testing(&inject_publication_failure);
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 5U)));
  const auto expected = logged.engine->engine().snapshot();
  const auto expected_offset = logged.engine->log_offset();

  const std::array stages{
      detail::SnapshotPublicationStage::create_temporary,
      detail::SnapshotPublicationStage::write,
      detail::SnapshotPublicationStage::flush,
      detail::SnapshotPublicationStage::sync,
      detail::SnapshotPublicationStage::close,
      detail::SnapshotPublicationStage::reread,
      detail::SnapshotPublicationStage::verify,
      detail::SnapshotPublicationStage::rename,
  };
  for (std::size_t index = 0U; index < stages.size(); ++index) {
    const auto directory = temporary.file("failure-" + std::to_string(index));
    std::filesystem::create_directory(directory);
    failing_publication_stage = stages[index];
    publication_failure_enabled = true;
    const auto failed = logged.engine->write_snapshot(directory);
    publication_failure_enabled = false;

    EXPECT_FALSE(failed) << "stage=" << static_cast<int>(stages[index]);
    EXPECT_EQ(failed.error.category, SnapshotErrorCategory::io_failure);
    EXPECT_FALSE(failed.final_file_visible);
    EXPECT_FALSE(std::filesystem::exists(failed.path));
    EXPECT_EQ(temporary_file_count(directory), 0U);
    EXPECT_FALSE(logged.engine->poisoned());
    EXPECT_EQ(logged.engine->log_offset(), expected_offset);
    EXPECT_EQ(logged.engine->engine().snapshot(), expected);
  }
}

TEST(PersistenceSnapshotPublication, CleanupFailureIsSurfacedAndNeverCreatesAFinalSnapshot) {
  FaultHooksGuard hooks;
  detail::set_snapshot_publication_hook_for_testing(&inject_publication_failure);
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);

  failing_publication_stage = detail::SnapshotPublicationStage::rename;
  publication_failure_enabled = true;
  cleanup_failure_enabled = true;
  const auto failed = logged.engine->write_snapshot(temporary.path());
  publication_failure_enabled = false;
  cleanup_failure_enabled = false;

  EXPECT_FALSE(failed);
  EXPECT_EQ(failed.error.category, SnapshotErrorCategory::io_failure);
  EXPECT_FALSE(failed.final_file_visible);
  EXPECT_FALSE(std::filesystem::exists(failed.path));
  EXPECT_EQ(temporary_file_count(temporary.path()), 1U);
  EXPECT_FALSE(logged.engine->poisoned());
}

TEST(PersistenceSnapshotPublication, LogSynchronizationFailurePoisonsBeforeSnapshotMutation) {
  FaultHooksGuard hooks;
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 5U)));
  const auto expected = logged.engine->engine().snapshot();
  const auto expected_offset = logged.engine->log_offset();

  detail::set_logged_engine_synchronize_hook_for_testing(&inject_log_sync_failure);
  const auto failed = logged.engine->write_snapshot(temporary.path());
  detail::set_logged_engine_synchronize_hook_for_testing(nullptr);

  EXPECT_FALSE(failed);
  EXPECT_EQ(failed.error.category, SnapshotErrorCategory::io_failure);
  EXPECT_TRUE(logged.engine->poisoned());
  EXPECT_EQ(logged.engine->log_offset(), expected_offset);
  EXPECT_EQ(logged.engine->engine().snapshot(), expected);
  EXPECT_EQ(temporary_file_count(temporary.path()), 0U);
  const auto rejected_session =
      logged.engine->submit(limit_order(12U, 2U, 9U, domain::Side::buy, 190, 3U));
  EXPECT_FALSE(rejected_session);
  EXPECT_TRUE(rejected_session.session_poisoned);
  EXPECT_EQ(logged.engine->engine().snapshot(), expected);
}

TEST(PersistenceSnapshotPublication, FailedNewPublicationPreservesThePreviousGoodSnapshot) {
  FaultHooksGuard hooks;
  detail::set_snapshot_publication_hook_for_testing(&inject_publication_failure);
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 5U)));
  const auto previous = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(previous);
  const auto previous_inspection = inspect_snapshot(previous.path);
  ASSERT_TRUE(previous_inspection);

  ASSERT_TRUE(logged.engine->submit(limit_order(12U, 2U, 9U, domain::Side::sell, 200, 7U)));
  failing_publication_stage = detail::SnapshotPublicationStage::rename;
  publication_failure_enabled = true;
  const auto failed = logged.engine->write_snapshot(temporary.path());
  publication_failure_enabled = false;

  EXPECT_FALSE(failed);
  EXPECT_FALSE(std::filesystem::exists(failed.path));
  EXPECT_TRUE(std::filesystem::exists(previous.path));
  EXPECT_EQ(temporary_file_count(temporary.path()), 0U);
  const auto preserved = inspect_snapshot(previous.path);
  ASSERT_TRUE(preserved);
  EXPECT_EQ(preserved.snapshot, previous_inspection.snapshot);
  EXPECT_EQ(preserved.input_bytes, previous_inspection.input_bytes);
}

TEST(PersistenceSnapshotRecovery, SnapshotPlusSuffixMatchesFullReplayInEveryMode) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);

  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 9U)));
  ASSERT_TRUE(logged.engine->submit(limit_order(12U, 2U, 9U, domain::Side::sell, 200, 7U)));
  const auto published = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(published);

  ASSERT_TRUE(logged.engine->submit(limit_order(13U, 3U, 7U, domain::Side::sell, 100, 4U)));
  ASSERT_TRUE(logged.engine->submit(domain::CancelOrder{
      .client_id = domain::ClientId{12U},
      .order_id = domain::OrderId{2U},
      .instrument_id = domain::InstrumentId{9U},
  }));
  ASSERT_TRUE(logged.engine->submit(limit_order(14U, 1U, 9U, domain::Side::buy, 190, 3U)));
  logged.engine.reset();

  for (const auto mode : {ReplayMode::fast, ReplayMode::verify, ReplayMode::diagnostic}) {
    auto full = replay_log(log, {.mode = mode});
    ASSERT_TRUE(full);
    auto recovered = recover_log_from_snapshot(log, published.path, {.mode = mode});
    ASSERT_TRUE(recovered);

    EXPECT_EQ(recovered.report.covered_sequence, domain::Sequence{2U});
    EXPECT_EQ(recovered.report.replay.records_scanned, 5U);
    EXPECT_EQ(recovered.report.replay.records_replayed, 3U);
    EXPECT_EQ(recovered.report.replay.committed, 4U);
    EXPECT_EQ(recovered.report.replay.rejected, 1U);
    EXPECT_EQ(recovered.report.replay.final_state_digest, full.report.final_state_digest);
    EXPECT_EQ(recovered.engine->snapshot(), full.engine->snapshot());
    EXPECT_EQ(recovered.engine->next_sequence(), full.engine->next_sequence());
    EXPECT_TRUE(core::MultiInstrumentEngineAccess::validate_invariants(*recovered.engine));

    const auto next = limit_order(15U, 20U, 9U, domain::Side::buy, 180, 6U);
    auto full_result = full.engine->execute(next);
    auto recovered_result = recovered.engine->execute(next);
    ASSERT_NE(full_result.batch(), nullptr);
    ASSERT_NE(recovered_result.batch(), nullptr);
    EXPECT_EQ(event_digest(*recovered_result.batch()), event_digest(*full_result.batch()));
    EXPECT_EQ(recovered.engine->snapshot(), full.engine->snapshot());
  }
}

TEST(PersistenceLoggedRecovery, FullLogRecoveryResumesAtTheValidatedExtent) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 9U)));
  ASSERT_TRUE(logged.engine->submit(limit_order(12U, 2U, 9U, domain::Side::sell, 200, 7U)));
  const auto log_id = logged.engine->log_id();
  const auto recovered_extent = logged.engine->log_offset();
  logged.engine.reset();

  auto resumed = LoggedEngine::recover(log, {.mode = ReplayMode::verify});
  ASSERT_TRUE(resumed);
  EXPECT_EQ(resumed.report.tail, ReplayTail::clean);
  EXPECT_EQ(resumed.report.valid_end_offset, recovered_extent);
  EXPECT_EQ(resumed.engine->log_offset(), recovered_extent);
  EXPECT_EQ(resumed.engine->log_id(), log_id);
  ASSERT_TRUE(resumed.engine->submit(limit_order(13U, 3U, 7U, domain::Side::buy, 95, 4U)));
  EXPECT_GT(resumed.engine->log_offset(), recovered_extent);
  const auto expected = resumed.engine->engine().snapshot();
  const auto final_extent = resumed.engine->log_offset();
  resumed.engine.reset();

  auto replayed = replay_log(log, {.mode = ReplayMode::verify});
  ASSERT_TRUE(replayed);
  EXPECT_EQ(replayed.engine->snapshot(), expected);
  EXPECT_EQ(replayed.report.records_replayed, 3U);
  EXPECT_EQ(replayed.report.valid_end_offset, final_extent);
}

TEST(PersistenceLoggedRecovery,
     SnapshotSuffixRecoveryCanAppendAndPublishAReplayEquivalentSnapshot) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 9U)));
  const auto first_snapshot = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(first_snapshot);
  ASSERT_TRUE(logged.engine->submit(limit_order(12U, 2U, 9U, domain::Side::sell, 200, 7U)));
  logged.engine.reset();

  auto resumed =
      LoggedEngine::recover_from_snapshot(log, first_snapshot.path, {.mode = ReplayMode::verify});
  ASSERT_TRUE(resumed);
  EXPECT_EQ(resumed.report.recovery_source, RecoverySource::explicit_snapshot);
  EXPECT_EQ(resumed.report.replay.records_replayed, 1U);
  ASSERT_TRUE(resumed.engine->submit(limit_order(13U, 3U, 7U, domain::Side::sell, 110, 4U)));
  const auto second_snapshot = resumed.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(second_snapshot);
  const auto expected = resumed.engine->engine().snapshot();
  const auto expected_digest = resumed.engine->engine().state_digest();
  resumed.engine.reset();

  auto replayed = replay_log(log, {.mode = ReplayMode::verify});
  ASSERT_TRUE(replayed);
  EXPECT_EQ(replayed.engine->snapshot(), expected);
  EXPECT_EQ(replayed.engine->state_digest(), expected_digest);

  auto inspected = inspect_snapshot(second_snapshot.path);
  ASSERT_TRUE(inspected);
  EXPECT_EQ(inspected.snapshot->state_digest, expected_digest);

  auto directory_resumed = LoggedEngine::recover_from_snapshot_directory(
      log, temporary.path(), {.mode = ReplayMode::verify});
  ASSERT_TRUE(directory_resumed);
  EXPECT_EQ(directory_resumed.report.recovery_source, RecoverySource::directory_snapshot);
  EXPECT_EQ(directory_resumed.report.selected_snapshot, second_snapshot.path);
  EXPECT_EQ(directory_resumed.engine->engine().snapshot(), expected);
}

TEST(PersistenceLoggedRecovery, TornTailsRemainReadOnlyUnderBothTailPolicies) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 9U)));
  ASSERT_TRUE(logged.engine->submit(limit_order(12U, 2U, 9U, domain::Side::sell, 200, 7U)));
  logged.engine.reset();

  const auto complete_size = std::filesystem::file_size(log);
  ASSERT_GT(complete_size, 1U);
  std::filesystem::resize_file(log, complete_size - 1U);
  const auto torn_bytes = read_bytes(log);

  auto strict =
      LoggedEngine::recover(log, {.mode = ReplayMode::verify, .tail_policy = TailPolicy::strict});
  EXPECT_FALSE(strict);
  EXPECT_EQ(strict.engine, nullptr);
  EXPECT_EQ(strict.report.tail, ReplayTail::torn);
  EXPECT_FALSE(strict.report.used_valid_prefix);
  EXPECT_EQ(read_bytes(log), torn_bytes);

  auto prefix = LoggedEngine::recover(
      log, {.mode = ReplayMode::verify, .tail_policy = TailPolicy::valid_prefix});
  EXPECT_FALSE(prefix);
  EXPECT_EQ(prefix.engine, nullptr);
  EXPECT_EQ(prefix.report.tail, ReplayTail::torn);
  EXPECT_TRUE(prefix.report.used_valid_prefix);
  EXPECT_EQ(read_bytes(log), torn_bytes);
}

TEST(PersistenceLoggedRecovery, MissingLogOpenFailureCreatesNoFile) {
  TemporaryDirectory temporary;
  const auto missing = temporary.file("missing.log");

  auto recovered = LoggedEngine::recover(missing);

  EXPECT_FALSE(recovered);
  EXPECT_EQ(recovered.engine, nullptr);
  EXPECT_EQ(recovered.report.error.category, LogErrorCategory::io_failure);
  EXPECT_FALSE(std::filesystem::exists(missing));
}

TEST(PersistenceSnapshotRecovery, SequenceZeroSnapshotRecoversAtTheExactHeaderBoundary) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  const auto expected = logged.engine->engine().snapshot();
  const auto published = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(published);
  EXPECT_EQ(published.covered_sequence, domain::Sequence{});
  logged.engine.reset();

  auto recovered = recover_log_from_snapshot(log, published.path);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered.report.covered_sequence, domain::Sequence{});
  EXPECT_EQ(recovered.report.replay.records_scanned, 0U);
  EXPECT_EQ(recovered.report.replay.records_replayed, 0U);
  EXPECT_EQ(recovered.engine->snapshot(), expected);
  EXPECT_EQ(recovered.engine->next_sequence(), domain::Sequence{1U});
}

TEST(PersistenceSnapshotRecovery, ExplicitCorruptionFailsWithoutFallingBackToTheLog) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 5U)));
  const auto published = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(published);
  logged.engine.reset();
  flip_final_byte(published.path);

  auto recovered = recover_log_from_snapshot(log, published.path);
  EXPECT_FALSE(recovered);
  EXPECT_EQ(recovered.engine, nullptr);
  EXPECT_EQ(recovered.report.snapshot_error.category, SnapshotErrorCategory::bad_checksum);
  EXPECT_EQ(recovered.report.replay.records_scanned, 1U);
  EXPECT_EQ(recovered.report.replay.committed, 1U);
  EXPECT_EQ(recovered.report.replay.rejected, 0U);
  EXPECT_EQ(recovered.report.replay.records_replayed, 0U);
}

TEST(PersistenceSnapshotRecovery, RejectsAValidSnapshotPairedToTheWrongLogBoundary) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto mismatched = temporary.file("mismatched.snapshot");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 5U)));
  const auto published = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(published);
  logged.engine.reset();

  auto inspected = inspect_snapshot(published.path);
  ASSERT_TRUE(inspected);
  ++inspected.snapshot->covered_log_byte_offset;
  auto encoded = detail::encode_snapshot(*inspected.snapshot);
  ASSERT_TRUE(encoded);
  write_bytes(mismatched, *encoded.value);

  auto recovered = recover_log_from_snapshot(log, mismatched);
  EXPECT_FALSE(recovered);
  EXPECT_EQ(recovered.report.snapshot_error.category, SnapshotErrorCategory::log_boundary_mismatch);
}

TEST(PersistenceSnapshotRecovery, DirectorySkipsFilenameSequenceThatDisagreesWithContents) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 5U)));
  const auto published = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(published);
  const auto log_id = logged.engine->log_id();
  logged.engine.reset();

  const auto mismatched =
      temporary.file("atlaslob-" + log_id.hex() + "-00000000000000000002.snapshot");
  std::filesystem::copy_file(published.path, mismatched);

  auto recovered = recover_log_from_snapshot_directory(log, temporary.path());
  ASSERT_TRUE(recovered);
  ASSERT_TRUE(recovered.report.selected_snapshot.has_value());
  EXPECT_EQ(*recovered.report.selected_snapshot, published.path);
  ASSERT_EQ(recovered.report.skipped_snapshots.size(), 1U);
  EXPECT_EQ(recovered.report.skipped_snapshots[0].path, mismatched);
  EXPECT_EQ(recovered.report.skipped_snapshots[0].error.category,
            SnapshotErrorCategory::sequence_mismatch);
}

TEST(PersistenceSnapshotRecovery, DirectorySkipsCandidateLocalIoAndUsesAnOlderSnapshot) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 5U)));
  const auto published = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(published);
  const auto log_id = logged.engine->log_id();
  logged.engine.reset();

  const auto unreadable_candidate =
      temporary.file("atlaslob-" + log_id.hex() + "-00000000000000000002.snapshot");
  std::filesystem::create_directory(unreadable_candidate);

  auto recovered = recover_log_from_snapshot_directory(log, temporary.path());
  ASSERT_TRUE(recovered);
  ASSERT_TRUE(recovered.report.selected_snapshot.has_value());
  EXPECT_EQ(*recovered.report.selected_snapshot, published.path);
  ASSERT_EQ(recovered.report.skipped_snapshots.size(), 1U);
  EXPECT_EQ(recovered.report.skipped_snapshots[0].path, unreadable_candidate);
  EXPECT_EQ(recovered.report.skipped_snapshots[0].error.category,
            SnapshotErrorCategory::io_failure);
}

TEST(PersistenceSnapshotRecovery, DirectoryDoesNotFollowCanonicalSnapshotSymlinks) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 5U)));
  const auto published = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(published);
  const auto log_id = logged.engine->log_id();
  logged.engine.reset();

  const auto linked_candidate =
      temporary.file("atlaslob-" + log_id.hex() + "-00000000000000000002.snapshot");
  std::error_code link_error;
  std::filesystem::create_symlink(published.path, linked_candidate, link_error);
  if (link_error) {
    GTEST_SKIP() << "symbolic links unavailable: " << link_error.message();
  }

  auto recovered = recover_log_from_snapshot_directory(log, temporary.path());
  ASSERT_TRUE(recovered);
  ASSERT_TRUE(recovered.report.selected_snapshot.has_value());
  EXPECT_EQ(*recovered.report.selected_snapshot, published.path);
  ASSERT_EQ(recovered.report.skipped_snapshots.size(), 1U);
  EXPECT_EQ(recovered.report.skipped_snapshots[0].path, linked_candidate);
  EXPECT_EQ(recovered.report.skipped_snapshots[0].error.category,
            SnapshotErrorCategory::io_failure);
}

TEST(PersistenceSnapshotRecovery, DirectorySkipsANewerBadCandidateAndUsesTheNewestValidOne) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 5U)));
  const auto older = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(older);
  ASSERT_TRUE(logged.engine->submit(limit_order(12U, 2U, 9U, domain::Side::sell, 200, 7U)));
  const auto newer = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(newer);
  ASSERT_TRUE(logged.engine->submit(limit_order(13U, 3U, 7U, domain::Side::buy, 95, 3U)));
  const auto expected = logged.engine->engine().snapshot();
  logged.engine.reset();
  flip_final_byte(newer.path);

  auto recovered = recover_log_from_snapshot_directory(log, temporary.path());
  ASSERT_TRUE(recovered);
  ASSERT_TRUE(recovered.report.selected_snapshot.has_value());
  EXPECT_EQ(*recovered.report.selected_snapshot, older.path);
  ASSERT_EQ(recovered.report.skipped_snapshots.size(), 1U);
  EXPECT_EQ(recovered.report.skipped_snapshots[0].path, newer.path);
  EXPECT_EQ(recovered.report.skipped_snapshots[0].error.category,
            SnapshotErrorCategory::bad_checksum);
  EXPECT_EQ(recovered.report.replay.records_replayed, 2U);
  EXPECT_EQ(recovered.engine->snapshot(), expected);
}

TEST(PersistenceSnapshotRecovery, DirectoryFallsBackToFullReplayWhenNoCandidateIsValid) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 5U)));
  const auto published = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(published);
  const auto expected = logged.engine->engine().snapshot();
  logged.engine.reset();
  flip_final_byte(published.path);
  const auto unrelated_unicode =
      temporary.path() / std::filesystem::path{u8"unrelated-\u2603.snapshot"};
  write_bytes(unrelated_unicode, std::span<const std::uint8_t>{});

  auto recovered = recover_log_from_snapshot_directory(log, temporary.path());
  ASSERT_TRUE(recovered);
  EXPECT_FALSE(recovered.report.selected_snapshot.has_value());
  ASSERT_EQ(recovered.report.skipped_snapshots.size(), 1U);
  EXPECT_EQ(recovered.report.replay.records_replayed, 1U);
  EXPECT_EQ(recovered.engine->snapshot(), expected);
}

TEST(PersistenceSnapshotRecovery, ARejectedBoundaryRestoresTheAuthoritativeSequence) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 5U)));
  auto duplicate = logged.engine->submit(limit_order(12U, 1U, 9U, domain::Side::buy, 190, 3U));
  ASSERT_TRUE(duplicate);
  ASSERT_TRUE(duplicate.engine_result->rejected());
  const auto published = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(published);
  logged.engine.reset();

  auto recovered = recover_log_from_snapshot(log, published.path);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered.report.replay.records_replayed, 0U);
  EXPECT_EQ(recovered.engine->snapshot().last_sequence, domain::Sequence{2U});
  EXPECT_EQ(recovered.engine->active_order_count(), 1U);
  EXPECT_EQ(recovered.engine->next_sequence(), domain::Sequence{3U});
}

TEST(PersistenceSnapshotRecovery, SnapshotSuffixHonorsStrictAndValidPrefixTailPolicies) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 5U)));
  const auto published = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(published);
  const auto expected = logged.engine->engine().snapshot();
  ASSERT_TRUE(logged.engine->submit(limit_order(12U, 2U, 9U, domain::Side::sell, 200, 7U)));
  logged.engine.reset();

  const auto complete_size = std::filesystem::file_size(log);
  ASSERT_GT(complete_size, published.covered_log_byte_offset);
  std::filesystem::resize_file(log, complete_size - 1U);

  auto strict = recover_log_from_snapshot(
      log, published.path, {.mode = ReplayMode::verify, .tail_policy = TailPolicy::strict});
  EXPECT_FALSE(strict);
  EXPECT_EQ(strict.engine, nullptr);
  EXPECT_EQ(strict.report.replay.tail, ReplayTail::torn);
  EXPECT_FALSE(strict.report.replay.used_valid_prefix);

  auto prefix = recover_log_from_snapshot(
      log, published.path, {.mode = ReplayMode::verify, .tail_policy = TailPolicy::valid_prefix});
  ASSERT_TRUE(prefix);
  EXPECT_EQ(prefix.report.replay.tail, ReplayTail::torn);
  EXPECT_TRUE(prefix.report.replay.used_valid_prefix);
  EXPECT_EQ(prefix.report.replay.records_replayed, 0U);
  EXPECT_EQ(prefix.engine->snapshot(), expected);
}

TEST(PersistenceSnapshotInspection, AppliesTheCallerBoundBeforeDecode) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  const auto published = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(published);
  logged.engine.reset();

  auto limits = CodecLimits{};
  limits.max_snapshot_bytes = published.encoded_bytes - 1U;
  const auto inspected = inspect_snapshot(published.path, limits);
  EXPECT_FALSE(inspected);
  EXPECT_EQ(inspected.error.category, SnapshotErrorCategory::excessive_length);
}

TEST(PersistenceSnapshotReports, RenderStableSchemasWithoutHostPaths) {
  TemporaryDirectory temporary;
  const auto log = temporary.file("orders.log");
  const auto instruments = catalog();
  auto logged = LoggedEngine::create_new(log, instruments);
  ASSERT_TRUE(logged);
  ASSERT_TRUE(logged.engine->submit(limit_order(11U, 1U, 7U, domain::Side::buy, 100, 5U)));
  const auto published = logged.engine->write_snapshot(temporary.path());
  ASSERT_TRUE(published);
  ASSERT_TRUE(logged.engine->submit(limit_order(12U, 2U, 9U, domain::Side::sell, 200, 7U)));
  logged.engine.reset();

  const auto inspected = inspect_snapshot(published.path);
  ASSERT_TRUE(inspected);
  const auto snapshot_json = detail::render_snapshot_report_json(inspected);
  EXPECT_TRUE(snapshot_json.starts_with(
      "{\"schema\":\"ATLAS_SNAPSHOT_REPORT_V1\",\"operation\":\"inspect_snapshot\","
      "\"status\":\"ok\""));
  EXPECT_NE(snapshot_json.find("\"covered_sequence\":\"1\""), std::string::npos);
  EXPECT_NE(snapshot_json.find("\"catalog_count\":\"2\""), std::string::npos);
  EXPECT_NE(snapshot_json.find("\"sequence_exhausted\":false"), std::string::npos);
  EXPECT_TRUE(snapshot_json.ends_with("\"error\":null}\n"));
  EXPECT_EQ(snapshot_json.find(published.path.string()), std::string::npos);

  const auto recovered =
      recover_log_from_snapshot(log, published.path, {.mode = ReplayMode::verify});
  ASSERT_TRUE(recovered);
  const auto recovery_json = detail::render_snapshot_replay_report_json(recovered.report);
  EXPECT_TRUE(recovery_json.starts_with(
      "{\"schema\":\"ATLAS_REPLAY_REPORT_V2\",\"status\":\"ok\",\"mode\":\"verify\","
      "\"tail_policy\":\"strict\""));
  EXPECT_NE(recovery_json.find("\"records_available\":\"2\""), std::string::npos);
  EXPECT_NE(recovery_json.find("\"records_covered_by_snapshot\":\"1\""), std::string::npos);
  EXPECT_NE(recovery_json.find("\"records_replayed\":\"1\""), std::string::npos);
  EXPECT_NE(recovery_json.find("\"recovery_source\":\"explicit_snapshot\""), std::string::npos);
  EXPECT_TRUE(recovery_json.ends_with("\"error\":null,\"divergence\":null}\n"));
  EXPECT_EQ(recovery_json.find(published.path.string()), std::string::npos);

  flip_final_byte(published.path);
  const auto failed = recover_log_from_snapshot(log, published.path);
  ASSERT_FALSE(failed);
  const auto failure_json = detail::render_snapshot_replay_report_json(failed.report);
  EXPECT_NE(failure_json.find("\"status\":\"invalid\""), std::string::npos);
  EXPECT_NE(failure_json.find("\"records_available\":\"2\""), std::string::npos);
  EXPECT_NE(failure_json.find("\"committed\":\"2\",\"rejected\":\"0\""), std::string::npos);
  EXPECT_NE(failure_json.find("\"records_replayed\":\"0\""), std::string::npos);
  EXPECT_EQ(failure_json.find("snapshot_fallback_full_log"), std::string::npos);
  EXPECT_EQ(failure_json.find(published.path.string()), std::string::npos);
}

}  // namespace
}  // namespace atlaslob::persistence::tests
