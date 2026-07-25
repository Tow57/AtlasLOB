#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "atlaslob/domain/commands.hpp"
#include "atlaslob/multi_instrument_engine.hpp"
#include "atlaslob/persistence/logged_engine.hpp"
#include "atlaslob/persistence/replay.hpp"
#include "crc32c.hpp"
#include "reports.hpp"

namespace atlaslob::persistence::tests {
namespace {

class TemporaryLog final {
 public:
  TemporaryLog() {
    std::random_device random;
    const auto suffix =
        (static_cast<std::uint64_t>(random()) << 32U) | static_cast<std::uint64_t>(random());
    path_ = std::filesystem::temp_directory_path() /
            ("atlaslob-log-" + std::to_string(suffix) + ".bin");
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

std::array<InstrumentConfig, 2U> catalog() {
  return {
      InstrumentConfig{
          .instrument_id = domain::InstrumentId{7U},
          .matching =
              {
                  .max_order_quantity = domain::Quantity{1000U},
                  .tick_increment = domain::PriceTicks{5},
                  .max_active_orders = 16U,
              },
      },
      InstrumentConfig{
          .instrument_id = domain::InstrumentId{9U},
          .matching =
              {
                  .max_order_quantity = domain::Quantity{1000U},
                  .tick_increment = domain::PriceTicks{1},
                  .max_active_orders = 16U,
              },
      },
  };
}

domain::NewOrder order(std::uint64_t order_id, std::uint32_t instrument_id, std::int64_t price) {
  return {
      .client_id = domain::ClientId{11U},
      .order_id = domain::OrderId{order_id},
      .instrument_id = domain::InstrumentId{instrument_id},
      .side = domain::Side::buy,
      .order_type = domain::OrderType::limit,
      .time_in_force = domain::TimeInForce::gtc,
      .limit_price = domain::PriceTicks{price},
      .quantity = domain::Quantity{5U},
  };
}

domain::NewOrder crossing_sell(std::uint64_t order_id, std::uint32_t instrument_id,
                               std::int64_t price, std::uint64_t quantity) {
  return {
      .client_id = domain::ClientId{22U},
      .order_id = domain::OrderId{order_id},
      .instrument_id = domain::InstrumentId{instrument_id},
      .side = domain::Side::sell,
      .order_type = domain::OrderType::limit,
      .time_in_force = domain::TimeInForce::gtc,
      .limit_price = domain::PriceTicks{price},
      .quantity = domain::Quantity{quantity},
  };
}

LogId fixed_log_id() {
  LogId result;
  for (std::size_t index = 0U; index < result.bytes.size(); ++index) {
    result.bytes[index] = static_cast<std::uint8_t>(index);
  }
  return result;
}

ReplayOptions replay_options(ReplayMode mode = ReplayMode::verify,
                             TailPolicy tail_policy = TailPolicy::strict) {
  ReplayOptions result;
  result.mode = mode;
  result.tail_policy = tail_policy;
  return result;
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(stream.good());
}

std::uint32_t decode_u32(std::span<const std::uint8_t, 4U> bytes) noexcept {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

void encode_u32(std::span<std::uint8_t, 4U> bytes, std::uint32_t value) noexcept {
  bytes[0] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
  bytes[1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
  bytes[2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
  bytes[3] = static_cast<std::uint8_t>(value & 0xffU);
}

void encode_u16(std::span<std::uint8_t, 2U> bytes, std::uint16_t value) noexcept {
  bytes[0] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
  bytes[1] = static_cast<std::uint8_t>(value & 0xffU);
}

void encode_u64(std::span<std::uint8_t, 8U> bytes, std::uint64_t value) noexcept {
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    const auto shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
    bytes[index] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
  }
}

void refresh_record_checksum(std::vector<std::uint8_t>& bytes, std::size_t record_begin,
                             std::size_t record_length) {
  const auto checksum = detail::crc32c(
      std::span<const std::uint8_t>{bytes.data() + record_begin, record_length - 4U});
  encode_u32(std::span<std::uint8_t, 4U>{bytes.data() + record_begin + record_length - 4U, 4U},
             checksum);
}

TEST(CommandLogReplay, AllModesReconstructTheSameMultiInstrumentState) {
  TemporaryLog file;
  const auto instruments = catalog();
  auto opened =
      LoggedEngine::create_new(file.path(), instruments, fixed_log_id(),
                               MultiInstrumentEngineConfig{.max_total_active_orders = 32U});
  ASSERT_TRUE(opened);

  ASSERT_TRUE(opened.engine->submit(order(1U, 7U, 100)));
  ASSERT_TRUE(opened.engine->submit(order(2U, 999U, 100)));
  ASSERT_TRUE(opened.engine->submit(order(3U, 9U, 101)));
  const auto expected_snapshot = opened.engine->engine().snapshot();
  const auto expected_digest = opened.engine->engine().state_digest();
  opened.engine.reset();

  for (const auto mode : {ReplayMode::fast, ReplayMode::verify, ReplayMode::diagnostic}) {
    auto replayed = replay_log(file.path(), replay_options(mode));
    ASSERT_TRUE(replayed);
    EXPECT_EQ(replayed.report.records_scanned, 3U);
    EXPECT_EQ(replayed.report.records_replayed, 3U);
    EXPECT_EQ(replayed.report.committed, 2U);
    EXPECT_EQ(replayed.report.rejected, 1U);
    EXPECT_EQ(replayed.report.final_state_digest, expected_digest);
    EXPECT_EQ(replayed.engine->snapshot(), expected_snapshot);
  }
}

TEST(CommandLogReplay, IndependentVerifiedReplaysProduceIdenticalReportsStateAndNextEvents) {
  TemporaryLog file;
  const auto instruments = catalog();
  auto opened =
      LoggedEngine::create_new(file.path(), instruments, fixed_log_id(),
                               MultiInstrumentEngineConfig{.max_total_active_orders = 32U});
  ASSERT_TRUE(opened);
  ASSERT_TRUE(opened.engine->submit(order(1U, 7U, 100)));
  ASSERT_TRUE(opened.engine->submit(order(2U, 999U, 100)));
  ASSERT_TRUE(opened.engine->submit(order(3U, 9U, 101)));
  const auto expected_snapshot = opened.engine->engine().snapshot();
  const auto expected_digest = opened.engine->engine().state_digest();
  opened.engine.reset();

  auto first = replay_log(file.path(), replay_options(ReplayMode::verify));
  auto second = replay_log(file.path(), replay_options(ReplayMode::verify));

  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  const auto first_json = detail::render_replay_report_json(first.report);
  const auto second_json = detail::render_replay_report_json(second.report);
  const auto first_text = detail::render_replay_report_text(first.report);
  const auto second_text = detail::render_replay_report_text(second.report);
  EXPECT_EQ(first_json, second_json);
  EXPECT_EQ(first_text, second_text);
  EXPECT_EQ(first.report.records_scanned, 3U);
  EXPECT_EQ(first.report.records_replayed, 3U);
  EXPECT_EQ(first.report.committed, 2U);
  EXPECT_EQ(first.report.rejected, 1U);
  EXPECT_EQ(second.report.records_scanned, first.report.records_scanned);
  EXPECT_EQ(second.report.records_replayed, first.report.records_replayed);
  EXPECT_EQ(second.report.committed, first.report.committed);
  EXPECT_EQ(second.report.rejected, first.report.rejected);
  EXPECT_EQ(first.report.final_state_digest, expected_digest);
  EXPECT_EQ(second.report.final_state_digest, expected_digest);
  EXPECT_EQ(first.engine->snapshot(), expected_snapshot);
  EXPECT_EQ(second.engine->snapshot(), expected_snapshot);
  EXPECT_EQ(first.engine->snapshot(), second.engine->snapshot());
  EXPECT_EQ(first.engine->state_digest(), second.engine->state_digest());

  const auto next_command = crossing_sell(4U, 7U, 100, 3U);
  const auto first_next = first.engine->execute(next_command);
  const auto second_next = second.engine->execute(next_command);
  EXPECT_EQ(first_next.error(), second_next.error());
  EXPECT_EQ(first_next.committed(), second_next.committed());
  EXPECT_EQ(first_next.rejected(), second_next.rejected());
  ASSERT_NE(first_next.batch(), nullptr);
  ASSERT_NE(second_next.batch(), nullptr);
  ASSERT_EQ(first_next.batch()->size(), second_next.batch()->size());
  EXPECT_TRUE(std::equal(first_next.batch()->events().begin(), first_next.batch()->events().end(),
                         second_next.batch()->events().begin()));
  EXPECT_TRUE(std::any_of(first_next.batch()->events().begin(), first_next.batch()->events().end(),
                          [](const domain::Event& event) {
                            return std::holds_alternative<domain::TradeEvent>(event);
                          }));
  EXPECT_EQ(first.engine->snapshot(), second.engine->snapshot());
  EXPECT_EQ(first.engine->state_digest(), second.engine->state_digest());
}

TEST(CommandLogReplay, SnapshotBoundsDoNotAffectReplay) {
  TemporaryLog file;
  const auto instruments = catalog();
  auto opened = LoggedEngine::create_new(file.path(), instruments, fixed_log_id());
  ASSERT_TRUE(opened);
  ASSERT_TRUE(opened.engine->submit(order(1U, 7U, 100)));
  const auto expected_digest = opened.engine->engine().state_digest();
  opened.engine.reset();

  auto options = replay_options();
  options.codec_limits.max_snapshot_bytes = 0U;
  ASSERT_TRUE(options.valid());

  const auto replayed = replay_log(file.path(), options);

  ASSERT_TRUE(replayed);
  EXPECT_EQ(replayed.report.records_replayed, 1U);
  EXPECT_EQ(replayed.report.final_state_digest, expected_digest);
}

TEST(CommandLogReplay, StrictRejectsAndValidPrefixAcceptsOnlyATornTail) {
  TemporaryLog file;
  const auto instruments = catalog();
  auto opened = LoggedEngine::create_new(file.path(), instruments, fixed_log_id());
  ASSERT_TRUE(opened);
  ASSERT_TRUE(opened.engine->submit(order(1U, 7U, 100)));
  const auto expected_digest = opened.engine->engine().state_digest();
  opened.engine.reset();

  {
    std::ofstream stream{file.path(), std::ios::binary | std::ios::app};
    const std::array torn{static_cast<char>(0), static_cast<char>(0)};
    stream.write(torn.data(), static_cast<std::streamsize>(torn.size()));
  }

  const auto strict = replay_log(file.path());
  EXPECT_FALSE(strict);
  EXPECT_EQ(strict.report.error.category, LogErrorCategory::truncated_final_record);
  EXPECT_EQ(strict.report.tail, ReplayTail::torn);
  EXPECT_FALSE(strict.report.used_valid_prefix);
  EXPECT_FALSE(strict.report.warning);

  const auto prefix =
      replay_log(file.path(), replay_options(ReplayMode::verify, TailPolicy::valid_prefix));
  ASSERT_TRUE(prefix);
  EXPECT_TRUE(prefix.report.used_valid_prefix);
  EXPECT_EQ(prefix.report.tail, ReplayTail::torn);
  EXPECT_EQ(prefix.report.final_state_digest, expected_digest);
}

TEST(CommandLogReplay, VerifyAndDiagnosticDetectRecomputedEvidenceTampering) {
  TemporaryLog file;
  const auto instruments = catalog();
  auto opened = LoggedEngine::create_new(file.path(), instruments, fixed_log_id());
  ASSERT_TRUE(opened);
  ASSERT_TRUE(opened.engine->submit(order(1U, 7U, 100)));
  opened.engine.reset();

  auto bytes = read_bytes(file.path());
  ASSERT_GE(bytes.size(), 20U);
  const auto header_length = decode_u32(std::span<const std::uint8_t, 4U>{bytes.data() + 16U, 4U});
  ASSERT_GE(bytes.size(), static_cast<std::size_t>(header_length) + 66U);
  const auto record_begin = static_cast<std::size_t>(header_length);
  const auto record_length =
      decode_u32(std::span<const std::uint8_t, 4U>{bytes.data() + record_begin, 4U});
  ASSERT_EQ(record_begin + record_length, bytes.size());

  bytes[record_begin + 30U] ^= 0x01U;
  const auto checksum = detail::crc32c(std::span<const std::uint8_t>{
      bytes.data() + record_begin, static_cast<std::size_t>(record_length) - 4U});
  encode_u32(std::span<std::uint8_t, 4U>{bytes.data() + record_begin + record_length - 4U, 4U},
             checksum);
  write_bytes(file.path(), bytes);

  const auto fast = replay_log(file.path(), replay_options(ReplayMode::fast));
  ASSERT_TRUE(fast);

  for (const auto mode : {ReplayMode::verify, ReplayMode::diagnostic}) {
    const auto checked = replay_log(file.path(), replay_options(mode));
    EXPECT_FALSE(checked);
    ASSERT_TRUE(checked.report.divergence.has_value());
    EXPECT_EQ(checked.report.divergence->category, ReplayDivergenceCategory::event_digest);
    EXPECT_EQ(checked.report.divergence->sequence, domain::Sequence{1U});
    EXPECT_FALSE(checked.report.divergence->actual_events.empty());
  }
}

TEST(CommandLogReplay, VerificationClassifiesOutcomeReasonAndCountInPrecedenceOrder) {
  TemporaryLog file;
  const auto instruments = catalog();
  auto opened = LoggedEngine::create_new(file.path(), instruments, fixed_log_id());
  ASSERT_TRUE(opened);
  ASSERT_TRUE(opened.engine->submit(order(1U, 7U, 100)));
  ASSERT_TRUE(opened.engine->submit(order(2U, 999U, 100)));
  opened.engine.reset();

  const auto canonical = read_bytes(file.path());
  ASSERT_GE(canonical.size(), 20U);
  const auto header_length =
      decode_u32(std::span<const std::uint8_t, 4U>{canonical.data() + 16U, 4U});
  const auto first_begin = static_cast<std::size_t>(header_length);
  const auto first_length =
      decode_u32(std::span<const std::uint8_t, 4U>{canonical.data() + first_begin, 4U});
  const auto second_begin = first_begin + first_length;
  ASSERT_LT(second_begin, canonical.size());
  const auto second_length =
      decode_u32(std::span<const std::uint8_t, 4U>{canonical.data() + second_begin, 4U});
  ASSERT_EQ(second_begin + second_length, canonical.size());

  {
    auto bytes = canonical;
    bytes[first_begin + 11U] = static_cast<std::uint8_t>(RecordOutcome::rejected);
    encode_u16(std::span<std::uint8_t, 2U>{bytes.data() + first_begin + 20U, 2U},
               static_cast<std::uint16_t>(domain::RejectReason::unknown_instrument));
    refresh_record_checksum(bytes, first_begin, first_length);
    write_bytes(file.path(), bytes);

    const auto replayed = replay_log(file.path(), replay_options(ReplayMode::diagnostic));
    ASSERT_TRUE(replayed.report.divergence.has_value());
    EXPECT_EQ(replayed.report.divergence->category, ReplayDivergenceCategory::outcome);
  }
  {
    auto bytes = canonical;
    encode_u16(std::span<std::uint8_t, 2U>{bytes.data() + second_begin + 20U, 2U},
               static_cast<std::uint16_t>(domain::RejectReason::duplicate_order_id));
    refresh_record_checksum(bytes, second_begin, second_length);
    write_bytes(file.path(), bytes);

    const auto replayed = replay_log(file.path(), replay_options(ReplayMode::diagnostic));
    ASSERT_TRUE(replayed.report.divergence.has_value());
    EXPECT_EQ(replayed.report.divergence->category, ReplayDivergenceCategory::rejection_reason);
  }
  {
    auto bytes = canonical;
    encode_u64(std::span<std::uint8_t, 8U>{bytes.data() + first_begin + 22U, 8U}, 99U);
    refresh_record_checksum(bytes, first_begin, first_length);
    write_bytes(file.path(), bytes);

    const auto replayed = replay_log(file.path(), replay_options(ReplayMode::diagnostic));
    ASSERT_TRUE(replayed.report.divergence.has_value());
    EXPECT_EQ(replayed.report.divergence->category, ReplayDivergenceCategory::event_count);
  }
}

}  // namespace
}  // namespace atlaslob::persistence::tests
