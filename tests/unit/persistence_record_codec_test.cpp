#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "command_log_codec.hpp"
#include "crc32c.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::persistence;
using namespace atlaslob::persistence::detail;

[[nodiscard]] std::string hex(std::span<const std::uint8_t> bytes) {
  static constexpr std::string_view digits{"0123456789abcdef"};
  std::string result(bytes.size() * 2U, '0');
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    result[index * 2U] = digits[(bytes[index] >> 4U) & 0x0fU];
    result[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
  }
  return result;
}

void write_u32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
  bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
  bytes[offset + 2U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
  bytes[offset + 3U] = static_cast<std::uint8_t>(value & 0xffU);
}

void refresh_crc(std::vector<std::uint8_t>& bytes) {
  write_u32(bytes, bytes.size() - 4U, crc32c(std::span{bytes}.first(bytes.size() - 4U)));
}

[[nodiscard]] Digest256 digest_from(std::uint8_t first) {
  Digest256 result;
  for (std::size_t index = 0U; index < result.bytes.size(); ++index) {
    result.bytes[index] = static_cast<std::uint8_t>(first + index);
  }
  return result;
}

[[nodiscard]] CommandRecord raw_invalid_new_record() {
  return {
      .record_version = 1U,
      .command =
          domain::NewOrder{
              .client_id = domain::ClientId{std::numeric_limits<std::uint32_t>::max()},
              .order_id = domain::OrderId{std::numeric_limits<std::uint64_t>::max()},
              .instrument_id = domain::InstrumentId{7U},
              .side = static_cast<domain::Side>(0xffU),
              .order_type = static_cast<domain::OrderType>(0xfeU),
              .time_in_force = static_cast<domain::TimeInForce>(0xfdU),
              .limit_price = domain::PriceTicks{std::numeric_limits<std::int64_t>::min()},
              .quantity = domain::Quantity{std::numeric_limits<std::uint64_t>::max()},
          },
      .sequence = domain::Sequence{1U},
      .outcome = RecordOutcome::rejected,
      .rejection_reason = domain::RejectReason::invalid_side,
      .event_count = 1U,
      .event_digest = digest_from(0U),
  };
}

TEST(PersistenceRecordCodec, MatchesGoldenBytesForEveryCommandVariantAndPricePresence) {
  const std::array records{
      raw_invalid_new_record(),
      CommandRecord{
          .record_version = 1U,
          .command =
              domain::NewOrder{
                  .client_id = domain::ClientId{1U},
                  .order_id = domain::OrderId{2U},
                  .instrument_id = domain::InstrumentId{7U},
                  .side = domain::Side::buy,
                  .order_type = domain::OrderType::market,
                  .time_in_force = domain::TimeInForce::ioc,
                  .limit_price = std::nullopt,
                  .quantity = domain::Quantity{3U},
              },
          .sequence = domain::Sequence{2U},
          .outcome = RecordOutcome::committed,
          .rejection_reason = domain::RejectReason::none,
          .event_count = 2U,
          .event_digest = digest_from(32U),
      },
      CommandRecord{
          .record_version = 1U,
          .command =
              domain::CancelOrder{
                  .client_id = domain::ClientId{11U},
                  .order_id = domain::OrderId{2U},
                  .instrument_id = domain::InstrumentId{7U},
              },
          .sequence = domain::Sequence{3U},
          .outcome = RecordOutcome::committed,
          .rejection_reason = domain::RejectReason::none,
          .event_count = 4U,
          .event_digest = digest_from(64U),
      },
      CommandRecord{
          .record_version = 1U,
          .command =
              domain::ReplaceOrder{
                  .client_id = domain::ClientId{11U},
                  .old_order_id = domain::OrderId{2U},
                  .new_order_id = domain::OrderId{std::numeric_limits<std::uint64_t>::max()},
                  .instrument_id = domain::InstrumentId{9U},
                  .new_limit_price = domain::PriceTicks{std::numeric_limits<std::int64_t>::max()},
                  .new_quantity = domain::Quantity{std::numeric_limits<std::uint64_t>::max()},
              },
          .sequence = domain::Sequence{4U},
          .outcome = RecordOutcome::rejected,
          .rejection_reason = domain::RejectReason::invalid_tick,
          .event_count = 1U,
          .event_digest = digest_from(96U),
      },
  };
  constexpr std::array<std::string_view, 4U> expected{
      "000000660000002400010102000000000000000100040000000000000001"
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
      "ffffffffffffffffffffffff00000007fffefd018000000000000000ffffffffffffffffababe47a",
      "000000660000002400010101000000000000000200000000000000000002"
      "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
      "000000010000000000000002000000070102020000000000000000000000000000000003df3fcfd8",
      "000000520000001000010201000000000000000300000000000000000004"
      "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"
      "0000000b00000000000000020000000712401f73",
      "0000006a00000028000103020000000000000004000f0000000000000001"
      "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
      "0000000b0000000000000002ffffffffffffffff000000097fffffffffffffffffffffffffffffff"
      "67e69f89",
  };

  for (std::size_t index = 0U; index < records.size(); ++index) {
    const auto encoded = encode_command_record(records[index]);
    ASSERT_TRUE(encoded) << "index=" << index << " error=" << to_string(encoded.error.category);
    EXPECT_EQ(hex(*encoded.value), expected[index]) << "index=" << index;
  }
}

TEST(PersistenceRecordCodec, PreservesRawInvalidEnumsForDomainReplay) {
  auto encoded = encode_command_record(raw_invalid_new_record());
  ASSERT_TRUE(encoded);

  auto decoded = decode_command_record(*encoded.value);

  ASSERT_TRUE(decoded) << to_string(decoded.error.category);
  const auto& order = std::get<domain::NewOrder>(decoded.value->command);
  EXPECT_EQ(static_cast<std::uint8_t>(order.side), 0xffU);
  EXPECT_EQ(static_cast<std::uint8_t>(order.order_type), 0xfeU);
  EXPECT_EQ(static_cast<std::uint8_t>(order.time_in_force), 0xfdU);
  ASSERT_TRUE(order.limit_price.has_value());
  EXPECT_EQ(order.limit_price->value(), std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(order.quantity.value(), std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(decoded.value->outcome, RecordOutcome::rejected);
  EXPECT_EQ(decoded.value->rejection_reason, domain::RejectReason::invalid_side);
}

TEST(PersistenceRecordCodec, RejectsLengthBombBeforeReadingOrAllocatingTheRecord) {
  std::array<std::uint8_t, 8U> prefix{};
  write_u32(prefix, 0U, default_max_log_record_bytes + 1U);

  const auto inspected = inspect_log_record_length(prefix);

  EXPECT_FALSE(inspected);
  EXPECT_EQ(inspected.error.category, LogErrorCategory::excessive_length);
  EXPECT_EQ(inspected.error.byte_offset, 0U);
}

TEST(PersistenceRecordCodec, RejectsImpossibleTotalAndPayloadLengthRelationship) {
  std::array<std::uint8_t, 8U> prefix{};
  write_u32(prefix, 0U,
            static_cast<std::uint32_t>(command_log_record_fixed_bytes + new_order_payload_bytes));
  write_u32(prefix, 4U, static_cast<std::uint32_t>(new_order_payload_bytes - 1U));

  const auto inspected = inspect_log_record_length(prefix);

  EXPECT_FALSE(inspected);
  EXPECT_EQ(inspected.error.category, LogErrorCategory::invalid_length);
  EXPECT_EQ(inspected.error.byte_offset, 4U);
}

TEST(PersistenceRecordCodec, CompleteBadChecksumIsCorruptionRatherThanTornTail) {
  auto encoded = encode_command_record(raw_invalid_new_record());
  ASSERT_TRUE(encoded);
  encoded.value->back() ^= 0x01U;

  const auto decoded = decode_command_record(*encoded.value);

  EXPECT_FALSE(decoded);
  EXPECT_EQ(decoded.error.category, LogErrorCategory::bad_record_checksum);
}

TEST(PersistenceRecordCodec, RecomputedChecksumCannotHideAnUnknownRecordType) {
  auto encoded = encode_command_record(raw_invalid_new_record());
  ASSERT_TRUE(encoded);
  (*encoded.value)[10U] = 0xffU;
  refresh_crc(*encoded.value);

  const auto decoded = decode_command_record(*encoded.value);

  EXPECT_FALSE(decoded);
  EXPECT_EQ(decoded.error.category, LogErrorCategory::unknown_record_type);
  EXPECT_EQ(decoded.error.byte_offset, 10U);
}

TEST(PersistenceRecordCodec, RecomputedChecksumCannotHideAnUnsupportedRecordVersion) {
  auto encoded = encode_command_record(raw_invalid_new_record());
  ASSERT_TRUE(encoded);
  (*encoded.value)[8U] = 0U;
  (*encoded.value)[9U] = static_cast<std::uint8_t>(command_log_record_version + 1U);

  const auto checksum_result = decode_command_record(*encoded.value);
  EXPECT_FALSE(checksum_result);
  EXPECT_EQ(checksum_result.error.category, LogErrorCategory::bad_record_checksum);

  refresh_crc(*encoded.value);
  const auto version_result = decode_command_record(*encoded.value);
  EXPECT_FALSE(version_result);
  EXPECT_EQ(version_result.error.category, LogErrorCategory::unsupported_record_version);
  EXPECT_EQ(version_result.error.byte_offset, 8U);
}

TEST(PersistenceRecordCodec, UnknownOutcomeIsACommandSchemaErrorAfterChecksumValidation) {
  auto encoded = encode_command_record(raw_invalid_new_record());
  ASSERT_TRUE(encoded);
  (*encoded.value)[11U] = 0xffU;

  const auto checksum_result = decode_command_record(*encoded.value);
  EXPECT_FALSE(checksum_result);
  EXPECT_EQ(checksum_result.error.category, LogErrorCategory::bad_record_checksum);

  refresh_crc(*encoded.value);
  const auto type_result = decode_command_record(*encoded.value);
  EXPECT_FALSE(type_result);
  EXPECT_EQ(type_result.error.category, LogErrorCategory::invalid_command_schema);
  EXPECT_EQ(type_result.error.byte_offset, 11U);
}

TEST(PersistenceRecordCodec, AbsentPriceRequiresTheCanonicalZeroPlaceholder) {
  CommandRecord record{
      .record_version = 1U,
      .command =
          domain::NewOrder{
              .client_id = domain::ClientId{1U},
              .order_id = domain::OrderId{2U},
              .instrument_id = domain::InstrumentId{7U},
              .side = domain::Side::buy,
              .order_type = domain::OrderType::market,
              .time_in_force = domain::TimeInForce::ioc,
              .limit_price = std::nullopt,
              .quantity = domain::Quantity{3U},
          },
      .sequence = domain::Sequence{1U},
      .outcome = RecordOutcome::committed,
      .rejection_reason = domain::RejectReason::none,
      .event_count = 1U,
      .event_digest = digest_from(0U),
  };
  auto encoded = encode_command_record(record);
  ASSERT_TRUE(encoded);
  // Payload starts at 62. Presence is byte 19 and the price slot follows.
  (*encoded.value)[62U + 20U + 7U] = 1U;
  refresh_crc(*encoded.value);

  const auto decoded = decode_command_record(*encoded.value);

  EXPECT_FALSE(decoded);
  EXPECT_EQ(decoded.error.category, LogErrorCategory::invalid_command_schema);
  EXPECT_EQ(decoded.error.byte_offset, 62U + 19U);
}

TEST(PersistenceRecordCodec, RejectsInvalidOutcomeReasonCombinationsBeforeEncoding) {
  auto invalid = raw_invalid_new_record();
  invalid.outcome = RecordOutcome::committed;

  const auto result = encode_command_record(invalid);

  EXPECT_FALSE(result);
  EXPECT_EQ(result.error.category, LogErrorCategory::invalid_command_schema);
}

TEST(PersistenceRecordCodec, EveryStrictPrefixIsClassifiedAsATornFinalRecord) {
  auto encoded = encode_command_record(raw_invalid_new_record());
  ASSERT_TRUE(encoded);

  for (std::size_t size = 0U; size < encoded.value->size(); ++size) {
    const auto decoded =
        decode_command_record(std::span<const std::uint8_t>{*encoded.value}.first(size));
    EXPECT_FALSE(decoded) << "size=" << size;
    EXPECT_EQ(decoded.error.category, LogErrorCategory::truncated_final_record) << "size=" << size;
  }
}

}  // namespace
