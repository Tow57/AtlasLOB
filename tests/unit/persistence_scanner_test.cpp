#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "atlaslob/persistence/command_log.hpp"
#include "command_log_codec.hpp"
#include "crc32c.hpp"
#include "log_io.hpp"
#include "log_scanner.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::persistence;
using namespace atlaslob::persistence::detail;

static_assert(default_max_log_header_bytes == 1024U * 1024U);
static_assert(default_max_log_record_bytes == 64U * 1024U);
static_assert(default_log_io_chunk_bytes == 64U * 1024U);

class ScriptedSource final : public LogSource {
 public:
  explicit ScriptedSource(std::vector<std::uint8_t> bytes) : bytes_{std::move(bytes)} {}

  [[nodiscard]] LogExtentResult extent() noexcept override {
    if (fail_extent_) {
      return {
          .extent = 0U,
          .failure =
              {
                  .operation = LogIoOperation::inspect_extent,
                  .offset = 0U,
                  .system_error = std::make_error_code(std::errc::io_error),
              },
      };
    }
    return {
        .extent = static_cast<std::uint64_t>(bytes_.size()),
    };
  }

  [[nodiscard]] LogReadResult read_at(std::uint64_t offset,
                                      std::span<std::byte> destination) noexcept override {
    if (fail_at_.has_value() && offset >= *fail_at_) {
      return {
          .bytes_read = 0U,
          .eof = false,
          .failure =
              {
                  .operation = LogIoOperation::read,
                  .offset = offset,
                  .system_error = std::make_error_code(std::errc::io_error),
              },
      };
    }
    if (zero_progress_at_.has_value() && offset >= *zero_progress_at_) {
      return {};
    }
    if (offset >= bytes_.size()) {
      return {
          .bytes_read = 0U,
          .eof = true,
      };
    }

    const auto available = bytes_.size() - static_cast<std::size_t>(offset);
    const auto count = std::min({destination.size(), available, max_read_});
    std::memcpy(destination.data(), bytes_.data() + static_cast<std::size_t>(offset), count);
    return {
        .bytes_read = count,
        .eof = count == available,
    };
  }

  void set_max_read(std::size_t max_read) noexcept { max_read_ = max_read; }
  void fail_extent() noexcept { fail_extent_ = true; }
  void fail_at(std::uint64_t offset) noexcept { fail_at_ = offset; }
  void zero_progress_at(std::uint64_t offset) noexcept { zero_progress_at_ = offset; }
  void replace_bytes(std::vector<std::uint8_t> bytes) { bytes_ = std::move(bytes); }

 private:
  std::vector<std::uint8_t> bytes_;
  std::size_t max_read_{std::numeric_limits<std::size_t>::max()};
  std::optional<std::uint64_t> fail_at_;
  std::optional<std::uint64_t> zero_progress_at_;
  bool fail_extent_{};
};

class ScriptedSink final : public LogSink {
 public:
  [[nodiscard]] LogWriteResult write(std::span<const std::byte> bytes) noexcept override {
    if (zero_progress_at_.has_value() && position() >= *zero_progress_at_) {
      return {};
    }
    if (fail_at_.has_value() && position() >= *fail_at_) {
      return {
          .bytes_written = 0U,
          .failure =
              {
                  .operation = LogIoOperation::write,
                  .offset = position(),
                  .system_error = std::make_error_code(std::errc::io_error),
              },
      };
    }

    auto count = std::min(bytes.size(), max_write_);
    if (fail_at_.has_value() && *fail_at_ > position()) {
      count = std::min<std::size_t>(count, static_cast<std::size_t>(*fail_at_ - position()));
    }
    if (count == 0U) {
      return {};
    }
    const auto original_size = bytes_.size();
    bytes_.resize(original_size + count);
    std::memcpy(bytes_.data() + original_size, bytes.data(), count);
    return {
        .bytes_written = count,
    };
  }

  [[nodiscard]] LogIoFailure flush() noexcept override {
    if (!fail_flush_) {
      return {};
    }
    return {
        .operation = LogIoOperation::flush,
        .offset = position(),
        .system_error = std::make_error_code(std::errc::io_error),
    };
  }

  [[nodiscard]] LogIoFailure sync() noexcept override {
    if (!fail_sync_) {
      return {};
    }
    return {
        .operation = LogIoOperation::sync,
        .offset = position(),
        .system_error = std::make_error_code(std::errc::io_error),
    };
  }

  [[nodiscard]] std::uint64_t position() const noexcept override {
    return static_cast<std::uint64_t>(bytes_.size());
  }

  void set_max_write(std::size_t max_write) noexcept { max_write_ = max_write; }
  void fail_at(std::uint64_t offset) noexcept { fail_at_ = offset; }
  void zero_progress_at(std::uint64_t offset) noexcept { zero_progress_at_ = offset; }
  void fail_flush() noexcept { fail_flush_ = true; }
  void fail_sync() noexcept { fail_sync_ = true; }
  void seed(std::uint8_t byte) { bytes_.push_back(byte); }

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

 private:
  std::vector<std::uint8_t> bytes_;
  std::size_t max_write_{std::numeric_limits<std::size_t>::max()};
  std::optional<std::uint64_t> fail_at_;
  std::optional<std::uint64_t> zero_progress_at_;
  bool fail_flush_{};
  bool fail_sync_{};
};

class CapturingVisitor final : public LogScanVisitor {
 public:
  void on_header(const LogHeader& header, std::span<const std::uint8_t> encoded) override {
    header_id_ = header.log_id;
    header_bytes_ = encoded.size();
  }

  void on_record(const CommandRecord& record, std::span<const std::uint8_t> encoded,
                 std::uint64_t frame_begin, std::uint64_t frame_end) override {
    sequences_.push_back(record.sequence);
    ranges_.push_back({frame_begin, frame_end});
    encoded_sizes_.push_back(encoded.size());
  }

  LogId header_id_{};
  std::size_t header_bytes_{};
  std::vector<domain::Sequence> sequences_;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges_;
  std::vector<std::size_t> encoded_sizes_;
};

[[nodiscard]] LogHeader header_fixture() {
  const std::array catalog{
      InstrumentConfig{
          .instrument_id = domain::InstrumentId{7U},
          .matching =
              {
                  .max_order_quantity = domain::Quantity{1'000U},
                  .tick_increment = domain::PriceTicks{5},
                  .max_active_orders = 16U,
              },
      },
  };
  LogId log_id;
  for (std::size_t index = 0U; index < log_id.bytes.size(); ++index) {
    log_id.bytes[index] = static_cast<std::uint8_t>(index + 1U);
  }
  auto header =
      make_log_header(catalog, MultiInstrumentEngineConfig{.max_total_active_orders = 32U}, log_id);
  if (!header) {
    throw std::runtime_error{"test fixture header construction failed"};
  }
  return std::move(*header.value);
}

[[nodiscard]] CommandRecord new_record(std::uint64_t sequence,
                                       RecordOutcome outcome = RecordOutcome::committed,
                                       domain::RejectReason reason = domain::RejectReason::none) {
  return {
      .record_version = command_log_record_version,
      .command =
          domain::NewOrder{
              .client_id = domain::ClientId{11U},
              .order_id = domain::OrderId{100U + sequence},
              .instrument_id = domain::InstrumentId{7U},
              .side = domain::Side::buy,
              .order_type = domain::OrderType::limit,
              .time_in_force = domain::TimeInForce::gtc,
              .limit_price = domain::PriceTicks{100},
              .quantity = domain::Quantity{5U},
          },
      .sequence = domain::Sequence{sequence},
      .outcome = outcome,
      .rejection_reason = reason,
      .event_count = 3U,
      .event_digest = {},
  };
}

template <typename Value>
[[nodiscard]] Value require_codec_value(CodecResult<Value> result) {
  if (!result) {
    throw std::runtime_error{"test fixture codec failure"};
  }
  return std::move(*result.value);
}

[[nodiscard]] std::vector<std::uint8_t> encoded_header() {
  return require_codec_value(encode_log_header(header_fixture()));
}

[[nodiscard]] std::vector<std::uint8_t> encoded_record(const CommandRecord& record) {
  return require_codec_value(encode_command_record(record));
}

void append(std::vector<std::uint8_t>& destination, std::span<const std::uint8_t> source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

[[nodiscard]] std::vector<std::uint8_t> complete_log(std::span<const CommandRecord> records) {
  auto bytes = encoded_header();
  for (const auto& record : records) {
    const auto encoded = encoded_record(record);
    append(bytes, encoded);
  }
  return bytes;
}

void encode_u32(std::span<std::uint8_t, 4U> destination, std::uint32_t value) {
  destination[0] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
  destination[1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
  destination[2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
  destination[3] = static_cast<std::uint8_t>(value & 0xffU);
}

void rewrite_record_crc(std::span<std::uint8_t> record) {
  const auto checksum = crc32c(std::span<const std::uint8_t>{record}.first(record.size() - 4U));
  encode_u32(std::span<std::uint8_t, 4U>{record.data() + record.size() - 4U, 4U}, checksum);
}

[[nodiscard]] std::filesystem::path temporary_path(std::string_view suffix) {
  static std::uint64_t counter{};
  const auto name =
      "atlaslob_scanner_test_" + std::to_string(++counter) + "_" + std::string{suffix};
  return std::filesystem::temp_directory_path() / name;
}

void write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  if (!output) {
    throw std::runtime_error{"failed to create scanner test input"};
  }
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw std::runtime_error{"failed to write scanner test input"};
  }
}

[[nodiscard]] std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    throw std::runtime_error{"failed to open scanner test output"};
  }
  return {
      std::istreambuf_iterator<char>{input},
      std::istreambuf_iterator<char>{},
  };
}

TEST(CommandLogScanner, RecognizesCleanHeaderOnlyAndMultiRecordLogs) {
  const auto header = encoded_header();
  ScriptedSource header_only{header};
  auto empty = scan_command_log(header_only);

  ASSERT_TRUE(empty.clean());
  ASSERT_TRUE(empty.header.has_value());
  EXPECT_EQ(empty.source_extent, header.size());
  EXPECT_EQ(empty.header_end_offset, header.size());
  EXPECT_EQ(empty.valid_end_offset, header.size());
  EXPECT_EQ(empty.record_count, 0U);
  EXPECT_EQ(empty.last_sequence, std::nullopt);
  EXPECT_EQ(empty.next_sequence, domain::Sequence{1U});

  const std::array records{
      new_record(1U),
      new_record(2U, RecordOutcome::rejected, domain::RejectReason::duplicate_order_id),
  };
  const auto bytes = complete_log(records);
  ScriptedSource source{bytes};
  CapturingVisitor visitor;
  const auto result = scan_command_log(source, {}, &visitor);

  ASSERT_TRUE(result.clean());
  EXPECT_EQ(result.valid_end_offset, bytes.size());
  EXPECT_EQ(result.record_count, 2U);
  EXPECT_EQ(result.last_sequence, domain::Sequence{2U});
  EXPECT_EQ(result.next_sequence, domain::Sequence{3U});
  EXPECT_EQ(visitor.header_bytes_, header.size());
  EXPECT_EQ(visitor.header_id_, header_fixture().log_id);
  EXPECT_EQ(visitor.sequences_,
            (std::vector<domain::Sequence>{domain::Sequence{1U}, domain::Sequence{2U}}));
  ASSERT_EQ(visitor.ranges_.size(), 2U);
  EXPECT_EQ(visitor.ranges_[0].first, header.size());
  EXPECT_EQ(visitor.ranges_[1].second, bytes.size());
}

TEST(CommandLogScanner, SnapshotBoundsDoNotAffectLogScanning) {
  const std::array records{new_record(1U)};
  const auto bytes = complete_log(records);
  ScriptedSource source{bytes};
  LogScanOptions options;
  options.codec_limits.max_snapshot_bytes = 0U;

  ASSERT_TRUE(options.valid());
  const auto result = scan_command_log(source, options);

  ASSERT_TRUE(result.clean());
  EXPECT_EQ(result.valid_end_offset, bytes.size());
  EXPECT_EQ(result.record_count, 1U);
}

TEST(CommandLogScanner, ValidPrefixDigestDistinguishesSameShapeValidLogs) {
  auto first_record = new_record(1U);
  auto second_record = first_record;
  std::get<domain::NewOrder>(second_record.command).order_id = domain::OrderId{999U};
  const std::array first_records{first_record};
  const std::array second_records{second_record};
  const auto first_bytes = complete_log(first_records);
  const auto second_bytes = complete_log(second_records);
  ASSERT_EQ(first_bytes.size(), second_bytes.size());

  ScriptedSource source{first_bytes};
  const auto first = scan_command_log(source);
  source.replace_bytes(second_bytes);
  const auto second = scan_command_log(source);

  ASSERT_TRUE(first.clean());
  ASSERT_TRUE(second.clean());
  EXPECT_EQ(first.source_extent, second.source_extent);
  EXPECT_EQ(first.valid_end_offset, second.valid_end_offset);
  EXPECT_EQ(first.record_count, second.record_count);
  ASSERT_TRUE(first.valid_prefix_digest.has_value());
  ASSERT_TRUE(second.valid_prefix_digest.has_value());
  EXPECT_NE(first.valid_prefix_digest, second.valid_prefix_digest);
}

TEST(CommandLogScanner, EveryIncompleteHeaderIsInvalidAndNeverRepairable) {
  const auto header = encoded_header();
  for (std::size_t cut = 0U; cut < header.size(); ++cut) {
    using Difference = std::vector<std::uint8_t>::difference_type;
    ScriptedSource source{
        std::vector<std::uint8_t>{header.begin(), header.begin() + static_cast<Difference>(cut)}};
    const auto result = scan_command_log(source);
    EXPECT_EQ(result.termination, LogScanTermination::corruption) << "cut=" << cut;
    EXPECT_EQ(result.error.category, LogErrorCategory::invalid_length) << "cut=" << cut;
    EXPECT_EQ(result.error.byte_offset, cut) << "cut=" << cut;
    EXPECT_EQ(result.valid_end_offset, 0U) << "cut=" << cut;
    EXPECT_FALSE(result.repairable()) << "cut=" << cut;
  }
}

TEST(CommandLogScanner, EveryIncompleteRecordIsARepairableTornTail) {
  const auto header = encoded_header();
  const auto record = encoded_record(new_record(1U));
  for (std::size_t cut = 1U; cut < record.size(); ++cut) {
    auto bytes = header;
    append(bytes, std::span<const std::uint8_t>{record}.first(cut));
    ScriptedSource source{bytes};
    const auto result = scan_command_log(source);
    EXPECT_EQ(result.termination, LogScanTermination::truncated_tail) << "cut=" << cut;
    EXPECT_EQ(result.error.category, LogErrorCategory::truncated_final_record) << "cut=" << cut;
    EXPECT_EQ(result.error.byte_offset, bytes.size()) << "cut=" << cut;
    EXPECT_EQ(result.valid_end_offset, header.size()) << "cut=" << cut;
    EXPECT_TRUE(result.repairable()) << "cut=" << cut;
  }
}

TEST(CommandLogScanner, CompleteInvalidLengthWinsOverAnIncompleteTail) {
  const auto header = encoded_header();
  {
    auto bytes = header;
    const std::array<std::uint8_t, 4U> invalid{};
    append(bytes, invalid);
    ScriptedSource source{bytes};
    const auto result = scan_command_log(source);
    EXPECT_EQ(result.termination, LogScanTermination::corruption);
    EXPECT_EQ(result.error.category, LogErrorCategory::invalid_length);
    EXPECT_EQ(result.error.byte_offset, header.size());
    EXPECT_EQ(result.valid_end_offset, header.size());
  }
  {
    auto bytes = header;
    std::array<std::uint8_t, 4U> excessive{};
    encode_u32(excessive, default_max_log_record_bytes + 1U);
    append(bytes, excessive);
    ScriptedSource source{bytes};
    const auto result = scan_command_log(source);
    EXPECT_EQ(result.termination, LogScanTermination::corruption);
    EXPECT_EQ(result.error.category, LogErrorCategory::excessive_length);
    EXPECT_EQ(result.error.byte_offset, header.size());
  }
  {
    auto bytes = header;
    std::array<std::uint8_t, 4U> valid_total{};
    encode_u32(valid_total, command_log_record_fixed_bytes);
    append(bytes, valid_total);
    ScriptedSource source{bytes};
    const auto result = scan_command_log(source);
    EXPECT_EQ(result.termination, LogScanTermination::truncated_tail);
    EXPECT_EQ(result.error.category, LogErrorCategory::truncated_final_record);
  }
  {
    auto bytes = header;
    std::array<std::uint8_t, 8U> impossible_lengths{};
    encode_u32(
        std::span<std::uint8_t, 4U>{impossible_lengths.data(), 4U},
        static_cast<std::uint32_t>(command_log_record_fixed_bytes + new_order_payload_bytes));
    encode_u32(std::span<std::uint8_t, 4U>{impossible_lengths.data() + 4U, 4U},
               static_cast<std::uint32_t>(new_order_payload_bytes - 1U));
    append(bytes, impossible_lengths);
    ScriptedSource source{bytes};
    const auto result = scan_command_log(source);
    EXPECT_EQ(result.termination, LogScanTermination::corruption);
    EXPECT_EQ(result.error.category, LogErrorCategory::invalid_length);
    EXPECT_EQ(result.error.byte_offset, header.size() + 4U);
    EXPECT_EQ(result.valid_end_offset, header.size());
    EXPECT_FALSE(result.repairable());
  }
}

TEST(CommandLogScanner, ReportsChecksumAndSchemaCorruptionAtAbsoluteOffsets) {
  const auto header = encoded_header();
  {
    auto damaged = header;
    damaged.back() ^= 0x01U;
    ScriptedSource source{damaged};
    const auto result = scan_command_log(source);
    EXPECT_EQ(result.error.category, LogErrorCategory::bad_header_checksum);
    EXPECT_EQ(result.error.byte_offset, header.size() - 4U);
    EXPECT_EQ(result.valid_end_offset, 0U);
  }
  {
    auto record = encoded_record(new_record(1U));
    record[30U] ^= 0x01U;
    auto bytes = header;
    append(bytes, record);
    ScriptedSource source{bytes};
    const auto result = scan_command_log(source);
    EXPECT_EQ(result.error.category, LogErrorCategory::bad_record_checksum);
    EXPECT_EQ(result.error.byte_offset, header.size() + record.size() - 4U);
    EXPECT_EQ(result.valid_end_offset, header.size());
  }
  {
    auto record = encoded_record(new_record(1U));
    record[11U] = 0xffU;
    rewrite_record_crc(record);
    auto bytes = header;
    append(bytes, record);
    ScriptedSource source{bytes};
    const auto result = scan_command_log(source);
    EXPECT_EQ(result.error.category, LogErrorCategory::invalid_command_schema);
    EXPECT_EQ(result.error.byte_offset, header.size() + 11U);
  }
}

TEST(CommandLogScanner, PreservesDomainInvalidCommandFieldsInRejectedRecords) {
  auto record = new_record(1U, RecordOutcome::rejected, domain::RejectReason::invalid_side);
  record.command = domain::NewOrder{
      .client_id = {},
      .order_id = {},
      .instrument_id = {},
      .side = static_cast<domain::Side>(0xffU),
      .order_type = static_cast<domain::OrderType>(0xfeU),
      .time_in_force = static_cast<domain::TimeInForce>(0xfdU),
      .limit_price = domain::PriceTicks{-1},
      .quantity = {},
  };
  const std::array records{record};
  const auto bytes = complete_log(records);
  ScriptedSource source{bytes};
  CapturingVisitor visitor;

  const auto result = scan_command_log(source, {}, &visitor);

  EXPECT_TRUE(result.clean());
  EXPECT_EQ(visitor.sequences_, std::vector<domain::Sequence>{domain::Sequence{1U}});
}

TEST(CommandLogScanner, DistinguishesDuplicateAndMissingSequencesAfterSchemaValidation) {
  {
    const std::array records{new_record(1U), new_record(1U)};
    ScriptedSource source{complete_log(records)};
    const auto result = scan_command_log(source);
    EXPECT_EQ(result.error.category, LogErrorCategory::duplicate_sequence);
    EXPECT_EQ(result.record_count, 1U);
    EXPECT_EQ(result.last_sequence, domain::Sequence{1U});
    EXPECT_EQ(result.next_sequence, domain::Sequence{2U});
  }
  {
    const std::array records{new_record(1U), new_record(3U)};
    ScriptedSource source{complete_log(records)};
    const auto result = scan_command_log(source);
    EXPECT_EQ(result.error.category, LogErrorCategory::missing_sequence);
    EXPECT_EQ(result.record_count, 1U);
    EXPECT_EQ(result.last_sequence, domain::Sequence{1U});
    EXPECT_EQ(result.next_sequence, domain::Sequence{2U});
  }
}

TEST(CommandLogScanner, ShortReadsAreEquivalentAndOperationalFailuresStayDistinct) {
  const std::array records{new_record(1U), new_record(2U)};
  const auto bytes = complete_log(records);
  ScriptedSource one_byte{bytes};
  one_byte.set_max_read(1U);
  const auto short_result = scan_command_log(one_byte);
  ASSERT_TRUE(short_result.clean());
  EXPECT_EQ(short_result.valid_end_offset, bytes.size());

  ScriptedSource extent_failure{bytes};
  extent_failure.fail_extent();
  const auto failed_extent = scan_command_log(extent_failure);
  EXPECT_EQ(failed_extent.termination, LogScanTermination::io_failure);
  EXPECT_EQ(failed_extent.error.category, LogErrorCategory::io_failure);

  ScriptedSource read_failure{bytes};
  read_failure.set_max_read(7U);
  read_failure.fail_at(7U);
  const auto failed_read = scan_command_log(read_failure);
  EXPECT_EQ(failed_read.termination, LogScanTermination::io_failure);
  EXPECT_EQ(failed_read.error.category, LogErrorCategory::io_failure);
  EXPECT_EQ(failed_read.error.byte_offset, 7U);

  ScriptedSource zero_progress{bytes};
  zero_progress.zero_progress_at(0U);
  const auto zero = scan_command_log(zero_progress);
  EXPECT_EQ(zero.termination, LogScanTermination::io_failure);
  EXPECT_EQ(zero.error.category, LogErrorCategory::io_failure);
}

TEST(CommandLogScanner, CopiesOnlyValidatedFramesAndSurfacesSinkFailures) {
  const auto header = encoded_header();
  const auto first = encoded_record(new_record(1U));
  const auto second = encoded_record(new_record(2U));
  auto torn = header;
  append(torn, first);
  append(torn, std::span<const std::uint8_t>{second}.first(17U));

  ScriptedSource source{torn};
  ScriptedSink sink;
  sink.set_max_write(1U);
  const auto copied = scan_command_log_to_sink(source, sink);
  ASSERT_EQ(copied.termination, LogScanTermination::truncated_tail);
  auto expected = header;
  append(expected, first);
  EXPECT_EQ(sink.bytes(), expected);
  EXPECT_EQ(copied.valid_end_offset, expected.size());

  ScriptedSource flush_source{torn};
  ScriptedSink flush_sink;
  flush_sink.fail_flush();
  const auto flush_failure = scan_command_log_to_sink(flush_source, flush_sink);
  EXPECT_EQ(flush_failure.termination, LogScanTermination::io_failure);
  EXPECT_EQ(flush_failure.error.category, LogErrorCategory::io_failure);
  EXPECT_EQ(flush_failure.valid_end_offset, expected.size());

  ScriptedSource nonempty_source{expected};
  ScriptedSink nonempty_sink;
  nonempty_sink.seed(0xaaU);
  const auto nonempty = scan_command_log_to_sink(nonempty_source, nonempty_sink);
  EXPECT_EQ(nonempty.termination, LogScanTermination::io_failure);
  EXPECT_EQ(nonempty.error.system_error, std::make_error_code(std::errc::invalid_argument));

  ScriptedSource zero_source{expected};
  ScriptedSink zero_sink;
  zero_sink.zero_progress_at(0U);
  const auto zero = scan_command_log_to_sink(zero_source, zero_sink);
  EXPECT_EQ(zero.termination, LogScanTermination::io_failure);
  EXPECT_EQ(zero.error.category, LogErrorCategory::io_failure);
}

TEST(CommandLogRepair, WritesANewValidatedPrefixAndRefusesUnsafeOutputs) {
  const auto header = encoded_header();
  const auto first = encoded_record(new_record(1U));
  const auto second = encoded_record(new_record(2U));
  auto valid_prefix = header;
  append(valid_prefix, first);
  auto torn = valid_prefix;
  append(torn, std::span<const std::uint8_t>{second}.first(9U));

  const auto input = temporary_path("input.log");
  const auto output = temporary_path("output.log");
  const auto existing = temporary_path("existing.log");
  const auto corrupt_input = temporary_path("corrupt.log");
  const auto refused_output = temporary_path("refused.log");
  const auto clean_refused_output = temporary_path("clean-refused.log");
  std::error_code ignored;
  static_cast<void>(std::filesystem::remove(input, ignored));
  static_cast<void>(std::filesystem::remove(output, ignored));
  static_cast<void>(std::filesystem::remove(existing, ignored));
  static_cast<void>(std::filesystem::remove(corrupt_input, ignored));
  static_cast<void>(std::filesystem::remove(refused_output, ignored));
  static_cast<void>(std::filesystem::remove(clean_refused_output, ignored));

  write_file(input, torn);
  const auto repaired = repair_command_log_to_new_file(input, output);
  ASSERT_TRUE(repaired.output_created);
  EXPECT_EQ(repaired.scan.termination, LogScanTermination::truncated_tail);
  EXPECT_EQ(read_file(output), valid_prefix);
  EXPECT_EQ(read_file(input), torn);

  auto repaired_source = open_native_log_source(output);
  ASSERT_TRUE(repaired_source);
  EXPECT_TRUE(scan_command_log(*repaired_source.source).clean());

  const auto clean_result = repair_command_log_to_new_file(output, clean_refused_output);
  EXPECT_FALSE(clean_result.output_created);
  EXPECT_TRUE(clean_result.scan.clean());
  EXPECT_FALSE(std::filesystem::exists(clean_refused_output));

  const std::array sentinel{std::uint8_t{0xdeU}, std::uint8_t{0xadU}};
  write_file(existing, sentinel);
  const auto existing_result = repair_command_log_to_new_file(input, existing);
  EXPECT_FALSE(existing_result.output_created);
  EXPECT_EQ(existing_result.scan.termination, LogScanTermination::io_failure);
  EXPECT_EQ(read_file(existing), (std::vector<std::uint8_t>{sentinel.begin(), sentinel.end()}));

  const auto same_result = repair_command_log_to_new_file(input, input);
  EXPECT_FALSE(same_result.output_created);
  EXPECT_EQ(same_result.scan.termination, LogScanTermination::io_failure);
  EXPECT_EQ(read_file(input), torn);

  auto corrupt = valid_prefix;
  corrupt[header.size() + 30U] ^= 0x01U;
  write_file(corrupt_input, corrupt);
  const auto corruption_result = repair_command_log_to_new_file(corrupt_input, refused_output);
  EXPECT_FALSE(corruption_result.output_created);
  EXPECT_EQ(corruption_result.scan.error.category, LogErrorCategory::bad_record_checksum);
  EXPECT_FALSE(std::filesystem::exists(refused_output));

  static_cast<void>(std::filesystem::remove(input, ignored));
  static_cast<void>(std::filesystem::remove(output, ignored));
  static_cast<void>(std::filesystem::remove(existing, ignored));
  static_cast<void>(std::filesystem::remove(corrupt_input, ignored));
  static_cast<void>(std::filesystem::remove(refused_output, ignored));
  static_cast<void>(std::filesystem::remove(clean_refused_output, ignored));
}

TEST(NativeAppendFileSink, SeparatesBufferedFlushAndSyncSessionBoundaries) {
  const auto path = temporary_path("append.log");
  const auto abandoned_path = temporary_path("abandoned.log");
  std::error_code ignored;
  static_cast<void>(std::filesystem::remove(path, ignored));
  static_cast<void>(std::filesystem::remove(abandoned_path, ignored));
  const auto header = encoded_header();

  {
    auto opened = open_native_append_log_sink(abandoned_path);
    ASSERT_TRUE(opened);
    const auto write =
        opened.sink->write(std::as_bytes(std::span<const std::uint8_t>{header}.first(8U)));
    ASSERT_TRUE(write);
  }
  EXPECT_FALSE(std::filesystem::exists(abandoned_path));

  {
    auto opened = open_native_append_log_sink(path);
    ASSERT_TRUE(opened);
    std::size_t written{};
    while (written < header.size()) {
      const auto write =
          opened.sink->write(std::as_bytes(std::span<const std::uint8_t>{header}.subspan(written)));
      ASSERT_TRUE(write);
      ASSERT_GT(write.bytes_written, 0U);
      written += write.bytes_written;
    }
    EXPECT_EQ(opened.sink->position(), header.size());
    EXPECT_FALSE(opened.sink->flush());
    EXPECT_FALSE(opened.sink->sync());
    opened.sink->mark_header_published();
    EXPECT_TRUE(opened.sink->header_published());
    EXPECT_FALSE(opened.sink->close());
  }
  EXPECT_EQ(read_file(path), header);

  const auto existing = open_native_append_log_sink(path);
  EXPECT_FALSE(existing);

  static_cast<void>(std::filesystem::remove(path, ignored));
  static_cast<void>(std::filesystem::remove(abandoned_path, ignored));
}

TEST(NativeNewFileSink, SurfacesCleanupFailureAndAllowsAnExplicitRetry) {
  const auto path = temporary_path("cleanup-failure.log");
  std::error_code ignored;
  static_cast<void>(std::filesystem::remove(path, ignored));
  auto opened = open_native_new_log_sink(path);
  ASSERT_TRUE(opened);
  const auto header = encoded_header();
  const auto write = opened.sink->write(std::as_bytes(std::span<const std::uint8_t>{header}));
  ASSERT_TRUE(write);
  ASSERT_TRUE(std::filesystem::exists(path));

  set_remove_file_hook_for_testing([](const std::filesystem::path&) noexcept {
    return std::make_error_code(std::errc::permission_denied);
  });
  const auto failed_cleanup = opened.sink->abandon();
  EXPECT_EQ(failed_cleanup.operation, LogIoOperation::remove_file);
  EXPECT_EQ(failed_cleanup.system_error, std::make_error_code(std::errc::permission_denied));
  EXPECT_TRUE(std::filesystem::exists(path));

  set_remove_file_hook_for_testing(nullptr);
  EXPECT_FALSE(opened.sink->abandon());
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(CommandLogScanner, RejectsInvalidScannerOptionsBeforeReading) {
  ScriptedSource source{encoded_header()};
  LogScanOptions options;
  options.read_chunk_bytes = 0U;
  const auto result = scan_command_log(source, options);
  EXPECT_EQ(result.termination, LogScanTermination::corruption);
  EXPECT_EQ(result.error.category, LogErrorCategory::invalid_length);
  EXPECT_EQ(result.valid_end_offset, 0U);
}

}  // namespace
