#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "command_log_codec.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::persistence;
using namespace atlaslob::persistence::detail;

void write_u32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
  bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
  bytes[offset + 2U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
  bytes[offset + 3U] = static_cast<std::uint8_t>(value & 0xffU);
}

template <typename Value>
[[nodiscard]] Value require_value(CodecResult<Value> result) {
  if (!result) {
    throw std::runtime_error{"decoder fuzz-smoke fixture failed to encode"};
  }
  return std::move(*result.value);
}

[[nodiscard]] LogHeader canonical_header_seed() {
  return {
      .format_version = command_log_format_version,
      .semantics_version = atlaslob_semantics_version,
      .log_id =
          {
              .bytes =
                  {
                      0x00U,
                      0x11U,
                      0x22U,
                      0x33U,
                      0x44U,
                      0x55U,
                      0x66U,
                      0x77U,
                      0x88U,
                      0x99U,
                      0xaaU,
                      0xbbU,
                      0xccU,
                      0xddU,
                      0xeeU,
                      0xffU,
                  },
          },
      .first_sequence = domain::Sequence{command_log_first_sequence},
      .engine_config =
          {
              .max_total_active_orders = 100U,
          },
      .catalog =
          {
              {
                  .instrument_id = domain::InstrumentId{7U},
                  .max_order_quantity = 1'000U,
                  .tick_increment = domain::PriceTicks{5},
                  .max_active_orders = 16U,
              },
              {
                  .instrument_id = domain::InstrumentId{9U},
                  .max_order_quantity = 2'000U,
                  .tick_increment = domain::PriceTicks{10},
                  .max_active_orders = std::numeric_limits<std::uint64_t>::max(),
              },
          },
  };
}

[[nodiscard]] Digest256 digest_seed(std::uint8_t first) {
  Digest256 result;
  for (std::size_t index = 0U; index < result.bytes.size(); ++index) {
    result.bytes[index] = static_cast<std::uint8_t>(first + index);
  }
  return result;
}

[[nodiscard]] std::array<CommandRecord, 4U> canonical_record_seeds() {
  return {
      CommandRecord{
          .record_version = command_log_record_version,
          .command =
              domain::NewOrder{
                  .client_id = {},
                  .order_id = {},
                  .instrument_id = {},
                  .side = static_cast<domain::Side>(0xffU),
                  .order_type = static_cast<domain::OrderType>(0xfeU),
                  .time_in_force = static_cast<domain::TimeInForce>(0xfdU),
                  .limit_price = domain::PriceTicks{std::numeric_limits<std::int64_t>::min()},
                  .quantity = {},
              },
          .sequence = domain::Sequence{1U},
          .outcome = RecordOutcome::rejected,
          .rejection_reason = domain::RejectReason::invalid_side,
          .event_count = 1U,
          .event_digest = digest_seed(0U),
      },
      CommandRecord{
          .record_version = command_log_record_version,
          .command =
              domain::NewOrder{
                  .client_id = domain::ClientId{1U},
                  .order_id = domain::OrderId{2U},
                  .instrument_id = domain::InstrumentId{7U},
                  .side = domain::Side::buy,
                  .order_type = domain::OrderType::market,
                  .time_in_force = domain::TimeInForce::ioc,
                  .limit_price = std::nullopt,
                  .quantity = domain::Quantity{std::numeric_limits<std::uint64_t>::max()},
              },
          .sequence = domain::Sequence{2U},
          .outcome = RecordOutcome::committed,
          .rejection_reason = domain::RejectReason::none,
          .event_count = 2U,
          .event_digest = digest_seed(32U),
      },
      CommandRecord{
          .record_version = command_log_record_version,
          .command =
              domain::CancelOrder{
                  .client_id = domain::ClientId{std::numeric_limits<std::uint32_t>::max()},
                  .order_id = domain::OrderId{std::numeric_limits<std::uint64_t>::max()},
                  .instrument_id = domain::InstrumentId{9U},
              },
          .sequence = domain::Sequence{3U},
          .outcome = RecordOutcome::rejected,
          .rejection_reason = domain::RejectReason::unknown_order_id,
          .event_count = std::numeric_limits<std::uint64_t>::max(),
          .event_digest = digest_seed(64U),
      },
      CommandRecord{
          .record_version = command_log_record_version,
          .command =
              domain::ReplaceOrder{
                  .client_id = domain::ClientId{11U},
                  .old_order_id = domain::OrderId{12U},
                  .new_order_id = domain::OrderId{std::numeric_limits<std::uint64_t>::max()},
                  .instrument_id = domain::InstrumentId{9U},
                  .new_limit_price = domain::PriceTicks{std::numeric_limits<std::int64_t>::max()},
                  .new_quantity = domain::Quantity{std::numeric_limits<std::uint64_t>::max()},
              },
          .sequence = domain::Sequence{4U},
          .outcome = RecordOutcome::committed,
          .rejection_reason = domain::RejectReason::none,
          .event_count = 4U,
          .event_digest = digest_seed(96U),
      },
  };
}

TEST(PersistenceDecoderFuzzSmoke, CanonicalSeedsRoundTripByteExactly) {
  const auto encoded_header = require_value(encode_log_header(canonical_header_seed()));
  const auto decoded_header = decode_log_header(encoded_header);
  ASSERT_TRUE(decoded_header) << to_string(decoded_header.error.category);
  const auto reencoded_header = require_value(encode_log_header(*decoded_header.value));
  EXPECT_EQ(reencoded_header, encoded_header);

  for (const auto& seed : canonical_record_seeds()) {
    const auto encoded = require_value(encode_command_record(seed));
    const auto decoded = decode_command_record(encoded);
    ASSERT_TRUE(decoded) << to_string(decoded.error.category);
    const auto reencoded = require_value(encode_command_record(*decoded.value));
    EXPECT_EQ(reencoded, encoded);
  }
}

TEST(PersistenceDecoderFuzzSmoke, HeaderDecoderCoversEveryTruncationBoundary) {
  const auto encoded = require_value(encode_log_header(canonical_header_seed()));
  for (std::size_t size = 0U; size < encoded.size(); ++size) {
    const auto prefix = std::span<const std::uint8_t>{encoded}.first(size);
    const auto decoded = decode_log_header(prefix);
    EXPECT_FALSE(decoded) << "size=" << size;
    EXPECT_EQ(decoded.error.category, LogErrorCategory::invalid_length) << "size=" << size;

    const auto inspected = inspect_log_header_length(
        prefix.first(std::min(prefix.size(), command_log_header_fixed_prefix_bytes)));
    if (size < command_log_header_fixed_prefix_bytes) {
      EXPECT_FALSE(inspected) << "size=" << size;
      EXPECT_EQ(inspected.error.category, LogErrorCategory::invalid_length) << "size=" << size;
    }
  }
}

TEST(PersistenceDecoderFuzzSmoke, RecordDecoderCoversEveryTruncationBoundary) {
  for (const auto& seed : canonical_record_seeds()) {
    const auto encoded = require_value(encode_command_record(seed));
    for (std::size_t size = 0U; size < encoded.size(); ++size) {
      const auto prefix = std::span<const std::uint8_t>{encoded}.first(size);
      const auto decoded = decode_command_record(prefix);
      EXPECT_FALSE(decoded) << "record_sequence=" << seed.sequence.value() << " size=" << size;
      EXPECT_EQ(decoded.error.category, LogErrorCategory::truncated_final_record)
          << "record_sequence=" << seed.sequence.value() << " size=" << size;

      const auto inspected = inspect_log_record_length(
          prefix.first(std::min(prefix.size(), command_log_record_length_prefix_bytes)));
      if (size < command_log_record_length_prefix_bytes) {
        EXPECT_FALSE(inspected) << "record_sequence=" << seed.sequence.value() << " size=" << size;
        EXPECT_EQ(inspected.error.category, LogErrorCategory::truncated_final_record)
            << "record_sequence=" << seed.sequence.value() << " size=" << size;
      }
    }
  }
}

TEST(PersistenceDecoderFuzzSmoke, EverySingleHeaderByteCorruptionIsRejected) {
  const auto encoded = require_value(encode_log_header(canonical_header_seed()));
  for (std::size_t index = 0U; index < encoded.size(); ++index) {
    auto mutated = encoded;
    mutated[index] ^= 0x01U;
    const auto decoded = decode_log_header(mutated);
    EXPECT_FALSE(decoded) << "byte_index=" << index;
    EXPECT_NE(decoded.error.category, LogErrorCategory::none) << "byte_index=" << index;
  }
}

TEST(PersistenceDecoderFuzzSmoke, EverySingleRecordByteCorruptionIsRejected) {
  for (const auto& seed : canonical_record_seeds()) {
    const auto encoded = require_value(encode_command_record(seed));
    for (std::size_t index = 0U; index < encoded.size(); ++index) {
      auto mutated = encoded;
      mutated[index] ^= 0x01U;
      const auto decoded = decode_command_record(mutated);
      EXPECT_FALSE(decoded) << "record_sequence=" << seed.sequence.value()
                            << " byte_index=" << index;
      EXPECT_NE(decoded.error.category, LogErrorCategory::none)
          << "record_sequence=" << seed.sequence.value() << " byte_index=" << index;
    }
  }
}

TEST(PersistenceDecoderFuzzSmoke, HeaderLengthBombsFailBeforeVariableDecode) {
  const auto encoded = require_value(encode_log_header(canonical_header_seed()));
  constexpr std::array invalid_lengths{
      std::uint32_t{0U},
      static_cast<std::uint32_t>(command_log_header_fixed_bytes - 1U),
  };
  for (const auto length : invalid_lengths) {
    auto bomb = encoded;
    write_u32(bomb, 16U, length);
    const auto inspected = inspect_log_header_length(
        std::span<const std::uint8_t>{bomb}.first(command_log_header_fixed_prefix_bytes));
    EXPECT_FALSE(inspected) << "length=" << length;
    EXPECT_EQ(inspected.error.category, LogErrorCategory::invalid_length) << "length=" << length;
  }

  constexpr std::array excessive_lengths{
      static_cast<std::uint32_t>(default_max_log_header_bytes + 1U),
      std::numeric_limits<std::uint32_t>::max(),
  };
  for (const auto length : excessive_lengths) {
    auto bomb = encoded;
    write_u32(bomb, 16U, length);
    const auto inspected = inspect_log_header_length(
        std::span<const std::uint8_t>{bomb}.first(command_log_header_fixed_prefix_bytes));
    EXPECT_FALSE(inspected) << "length=" << length;
    EXPECT_EQ(inspected.error.category, LogErrorCategory::excessive_length) << "length=" << length;
  }

  auto count_bomb = encoded;
  write_u32(count_bomb, 56U, std::numeric_limits<std::uint32_t>::max());
  const auto count_result = inspect_log_header_length(
      std::span<const std::uint8_t>{count_bomb}.first(command_log_header_fixed_prefix_bytes));
  EXPECT_FALSE(count_result);
  EXPECT_NE(count_result.error.category, LogErrorCategory::none);
}

TEST(PersistenceDecoderFuzzSmoke, RecordLengthBombsFailBeforePayloadDecode) {
  const auto encoded = require_value(encode_command_record(canonical_record_seeds()[0]));
  constexpr std::array invalid_lengths{
      std::uint32_t{0U},
      static_cast<std::uint32_t>(command_log_record_fixed_bytes - 1U),
  };
  for (const auto length : invalid_lengths) {
    auto prefix = encoded;
    write_u32(prefix, 0U, length);
    const auto inspected = inspect_log_record_length(
        std::span<const std::uint8_t>{prefix}.first(command_log_record_length_prefix_bytes));
    EXPECT_FALSE(inspected) << "length=" << length;
    EXPECT_EQ(inspected.error.category, LogErrorCategory::invalid_length) << "length=" << length;
  }

  constexpr std::array excessive_lengths{
      static_cast<std::uint32_t>(default_max_log_record_bytes + 1U),
      std::numeric_limits<std::uint32_t>::max(),
  };
  for (const auto length : excessive_lengths) {
    auto prefix = encoded;
    write_u32(prefix, 0U, length);
    const auto inspected = inspect_log_record_length(
        std::span<const std::uint8_t>{prefix}.first(command_log_record_length_prefix_bytes));
    EXPECT_FALSE(inspected) << "length=" << length;
    EXPECT_EQ(inspected.error.category, LogErrorCategory::excessive_length) << "length=" << length;
  }

  auto maximum_bounded = encoded;
  write_u32(maximum_bounded, 0U, default_max_log_record_bytes);
  write_u32(
      maximum_bounded, 4U,
      default_max_log_record_bytes - static_cast<std::uint32_t>(command_log_record_fixed_bytes));
  const auto maximum_inspected = inspect_log_record_length(
      std::span<const std::uint8_t>{maximum_bounded}.first(command_log_record_length_prefix_bytes));
  ASSERT_TRUE(maximum_inspected);
  EXPECT_EQ(*maximum_inspected.value, default_max_log_record_bytes);
  const auto incomplete_maximum = decode_command_record(maximum_bounded);
  EXPECT_FALSE(incomplete_maximum);
  EXPECT_EQ(incomplete_maximum.error.category, LogErrorCategory::truncated_final_record);

  auto payload_bomb = encoded;
  write_u32(payload_bomb, 4U, std::numeric_limits<std::uint32_t>::max());
  const auto payload_result = decode_command_record(payload_bomb);
  EXPECT_FALSE(payload_result);
  EXPECT_EQ(payload_result.error.category, LogErrorCategory::invalid_length);
}

}  // namespace
