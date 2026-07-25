#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include "atlaslob/domain/commands.hpp"
#include "atlaslob/multi_instrument_engine.hpp"
#include "atlaslob/persistence/command_log.hpp"
#include "atlaslob/persistence/logged_engine.hpp"
#include "command_log_codec.hpp"
#include "logged_engine_internal.hpp"
#include "multi_instrument_engine_access.hpp"

namespace atlaslob::persistence::tests {
namespace {

class FakeLogSink final : public detail::LogSink {
 public:
  detail::LogWriteResult write(std::span<const std::byte> bytes) noexcept override {
    if (fail_write_) {
      return {.failure = failure(detail::LogIoOperation::write)};
    }
    if (partial_before_failure_.has_value()) {
      const auto count = std::min(*partial_before_failure_, bytes.size());
      data_.insert(data_.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(count));
      position_ += count;
      partial_before_failure_.reset();
      fail_write_ = true;
      return {.bytes_written = count};
    }
    data_.insert(data_.end(), bytes.begin(), bytes.end());
    position_ += bytes.size();
    return {.bytes_written = bytes.size()};
  }

  detail::LogIoFailure flush() noexcept override {
    ++flush_calls_;
    return fail_flush_ ? failure(detail::LogIoOperation::flush) : detail::LogIoFailure{};
  }

  detail::LogIoFailure sync() noexcept override {
    ++sync_calls_;
    return fail_sync_ ? failure(detail::LogIoOperation::sync) : detail::LogIoFailure{};
  }

  std::uint64_t position() const noexcept override { return position_; }

  void fail_after(std::size_t bytes) noexcept { partial_before_failure_ = bytes; }
  void fail_flush(bool value = true) noexcept { fail_flush_ = value; }
  void fail_sync(bool value = true) noexcept { fail_sync_ = value; }

  [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
    return {reinterpret_cast<const std::uint8_t*>(data_.data()), data_.size()};
  }
  [[nodiscard]] std::size_t flush_calls() const noexcept { return flush_calls_; }
  [[nodiscard]] std::size_t sync_calls() const noexcept { return sync_calls_; }

 private:
  [[nodiscard]] detail::LogIoFailure failure(detail::LogIoOperation operation) const noexcept {
    return {
        .operation = operation,
        .offset = position_,
        .system_error = std::make_error_code(std::errc::io_error),
    };
  }

  std::vector<std::byte> data_;
  std::optional<std::size_t> partial_before_failure_;
  std::uint64_t position_{};
  std::size_t flush_calls_{};
  std::size_t sync_calls_{};
  bool fail_write_{};
  bool fail_flush_{};
  bool fail_sync_{};
};

InstrumentConfig instrument() {
  return {
      .instrument_id = domain::InstrumentId{7U},
      .matching =
          {
              .max_order_quantity = domain::Quantity{std::numeric_limits<std::uint64_t>::max()},
              .tick_increment = domain::PriceTicks{1},
              .max_active_orders = std::numeric_limits<std::size_t>::max(),
          },
  };
}

domain::NewOrder resting_order(std::uint64_t order_id) {
  return {
      .client_id = domain::ClientId{11U},
      .order_id = domain::OrderId{order_id},
      .instrument_id = domain::InstrumentId{7U},
      .side = domain::Side::buy,
      .order_type = domain::OrderType::limit,
      .time_in_force = domain::TimeInForce::gtc,
      .limit_price = domain::PriceTicks{100},
      .quantity = domain::Quantity{5U},
  };
}

TEST(LoggedEngineWriteAhead, WritesACompleteRecordBeforePublishingState) {
  const auto config = instrument();
  MultiInstrumentEngine engine{{&config, 1U}};
  FakeLogSink sink;
  bool poisoned = false;

  const auto result = detail::submit_logged(engine, sink, poisoned, Durability::sync_each_record,
                                            {}, domain::Command{resting_order(1U)});

  ASSERT_TRUE(result);
  ASSERT_TRUE(result.engine_result->committed());
  EXPECT_FALSE(poisoned);
  EXPECT_EQ(engine.active_order_count(), 1U);
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{2U});
  EXPECT_EQ(sink.flush_calls(), 1U);
  EXPECT_EQ(sink.sync_calls(), 1U);

  const auto decoded = detail::decode_command_record(sink.bytes());
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value->sequence, domain::Sequence{1U});
  EXPECT_EQ(decoded.value->outcome, RecordOutcome::committed);
  EXPECT_EQ(decoded.value->event_count, result.engine_result->batch()->size());
}

TEST(LoggedEngineWriteAhead, EveryPartialWriteBoundaryLeavesCoreUnchangedAndPoisonsSession) {
  const auto config = instrument();
  MultiInstrumentEngine probe_engine{{&config, 1U}};
  FakeLogSink probe_sink;
  bool probe_poisoned = false;
  const auto probe =
      detail::submit_logged(probe_engine, probe_sink, probe_poisoned, Durability::sync_each_record,
                            {}, domain::Command{resting_order(1U)});
  ASSERT_TRUE(probe);
  const auto encoded_size = probe_sink.bytes().size();
  ASSERT_GT(encoded_size, 0U);

  for (std::size_t cutoff = 0U; cutoff < encoded_size; ++cutoff) {
    SCOPED_TRACE(cutoff);
    MultiInstrumentEngine engine{{&config, 1U}};
    FakeLogSink sink;
    sink.fail_after(cutoff);
    bool poisoned = false;

    const auto failed = detail::submit_logged(engine, sink, poisoned, Durability::sync_each_record,
                                              {}, domain::Command{resting_order(1U)});

    EXPECT_FALSE(failed);
    EXPECT_TRUE(failed.session_poisoned);
    EXPECT_TRUE(poisoned);
    EXPECT_EQ(engine.active_order_count(), 0U);
    EXPECT_EQ(engine.next_sequence(), domain::Sequence{1U});

    const auto refused = detail::submit_logged(engine, sink, poisoned, Durability::sync_each_record,
                                               {}, domain::Command{resting_order(2U)});
    EXPECT_FALSE(refused);
    EXPECT_TRUE(refused.session_poisoned);
    EXPECT_EQ(engine.next_sequence(), domain::Sequence{1U});
  }
}

TEST(LoggedEngineWriteAhead, FlushAndSyncFailuresAreAtomicAndPoisoning) {
  for (const auto fail_sync : {false, true}) {
    const auto config = instrument();
    MultiInstrumentEngine engine{{&config, 1U}};
    FakeLogSink sink;
    if (fail_sync) {
      sink.fail_sync();
    } else {
      sink.fail_flush();
    }
    bool poisoned = false;

    const auto failed = detail::submit_logged(engine, sink, poisoned, Durability::sync_each_record,
                                              {}, domain::Command{resting_order(1U)});

    EXPECT_FALSE(failed);
    EXPECT_TRUE(poisoned);
    EXPECT_EQ(engine.active_order_count(), 0U);
    EXPECT_EQ(engine.next_sequence(), domain::Sequence{1U});
  }
}

TEST(LoggedEngineWriteAhead, DurabilityModesHaveDistinctFlushBoundaries) {
  const auto config = instrument();
  for (const auto durability :
       {Durability::buffered, Durability::flush_each_record, Durability::sync_each_record}) {
    MultiInstrumentEngine engine{{&config, 1U}};
    FakeLogSink sink;
    bool poisoned = false;

    const auto result = detail::submit_logged(engine, sink, poisoned, durability, {},
                                              domain::Command{resting_order(1U)});
    ASSERT_TRUE(result);

    EXPECT_EQ(sink.flush_calls(), durability == Durability::buffered ? 0U : 1U);
    EXPECT_EQ(sink.sync_calls(), durability == Durability::sync_each_record ? 1U : 0U);
  }
}

TEST(LoggedEngineWriteAhead, SequencedRejectionsAreLogged) {
  const auto config = instrument();
  MultiInstrumentEngine engine{{&config, 1U}};
  FakeLogSink sink;
  bool poisoned = false;
  auto invalid = resting_order(0U);
  invalid.client_id = domain::ClientId{0U};

  const auto result = detail::submit_logged(engine, sink, poisoned, Durability::buffered, {},
                                            domain::Command{invalid});

  ASSERT_TRUE(result);
  ASSERT_TRUE(result.engine_result->rejected());
  const auto decoded = detail::decode_command_record(sink.bytes());
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value->outcome, RecordOutcome::rejected);
  EXPECT_EQ(decoded.value->rejection_reason, domain::RejectReason::invalid_client_id);
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{2U});
}

TEST(LoggedEngineWriteAhead, LogsMaximumSequenceOnceAndExcludesExhaustionAttempts) {
  constexpr auto maximum_sequence = std::numeric_limits<std::uint64_t>::max();
  const auto config = instrument();
  MultiInstrumentEngine engine{{&config, 1U}};
  core::MultiInstrumentEngineAccess::set_next_sequence_for_testing(
      engine, domain::Sequence{maximum_sequence});
  FakeLogSink sink;
  bool poisoned = false;

  const auto maximum = detail::submit_logged(engine, sink, poisoned, Durability::buffered, {},
                                             domain::Command{resting_order(1U)});

  ASSERT_TRUE(maximum);
  ASSERT_NE(maximum.engine_result->batch(), nullptr);
  EXPECT_EQ(maximum.engine_result->batch()->command_sequence(), domain::Sequence{maximum_sequence});
  EXPECT_TRUE(engine.sequence_exhausted());
  EXPECT_EQ(engine.next_sequence(), domain::Sequence{});
  const auto logged_bytes = std::vector<std::uint8_t>{sink.bytes().begin(), sink.bytes().end()};
  const auto decoded = detail::decode_command_record(logged_bytes);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value->sequence, domain::Sequence{maximum_sequence});

  const auto exhausted = detail::submit_logged(engine, sink, poisoned, Durability::buffered, {},
                                               domain::Command{resting_order(2U)});

  ASSERT_TRUE(exhausted.engine_result.has_value());
  EXPECT_EQ(exhausted.engine_result->error(), EngineError::sequence_exhausted);
  EXPECT_EQ(exhausted.engine_result->batch(), nullptr);
  EXPECT_FALSE(exhausted.error);
  EXPECT_FALSE(exhausted.session_poisoned);
  EXPECT_FALSE(poisoned);
  EXPECT_EQ(std::vector<std::uint8_t>(sink.bytes().begin(), sink.bytes().end()), logged_bytes);
}

}  // namespace
}  // namespace atlaslob::persistence::tests
