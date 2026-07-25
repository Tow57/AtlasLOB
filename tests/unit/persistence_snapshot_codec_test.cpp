#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atlaslob/persistence/configuration_digest.hpp"
#include "crc32c.hpp"
#include "snapshot_codec.hpp"

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

void write_u64(std::span<std::uint8_t> bytes, std::size_t offset, std::uint64_t value) {
  for (std::size_t index = 0U; index < 8U; ++index) {
    const auto shift = static_cast<unsigned>((7U - index) * 8U);
    bytes[offset + index] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
  }
}

void refresh_crc(std::vector<std::uint8_t>& bytes) {
  write_u32(bytes, bytes.size() - 4U, crc32c(std::span{bytes}.first(bytes.size() - 4U)));
}

template <typename Value>
void expect_error(const SnapshotCodecResult<Value>& result, SnapshotErrorCategory category,
                  std::uint64_t offset) {
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error.category, category);
  EXPECT_EQ(result.error.byte_offset, offset);
}

[[nodiscard]] OrderSnapshot order(std::uint64_t order_id, std::uint32_t client_id,
                                  domain::Side side, std::int64_t price, std::uint64_t remaining,
                                  std::uint64_t priority) {
  return {
      .order_id = domain::OrderId{order_id},
      .client_id = domain::ClientId{client_id},
      .instrument_id = domain::InstrumentId{7U},
      .side = side,
      .price = domain::PriceTicks{price},
      .remaining_quantity = domain::Quantity{remaining},
      .priority_sequence = domain::Sequence{priority},
  };
}

[[nodiscard]] EngineSnapshot representative_engine_snapshot() {
  return {
      .semantics_version = atlaslob_semantics_version,
      .engine_config =
          {
              .max_total_active_orders = 30U,
          },
      .catalog =
          {
              {
                  .instrument_id = domain::InstrumentId{7U},
                  .matching =
                      {
                          .max_order_quantity = domain::Quantity{100U},
                          .tick_increment = domain::PriceTicks{5},
                          .max_active_orders = 10U,
                      },
              },
              {
                  .instrument_id = domain::InstrumentId{9U},
                  .matching =
                      {
                          .max_order_quantity = domain::Quantity{200U},
                          .tick_increment = domain::PriceTicks{10},
                          .max_active_orders = 20U,
                      },
              },
          },
      .last_sequence = domain::Sequence{7U},
      .sequence_exhausted = false,
      .active_order_count = 4U,
      .instruments =
          {
              {
                  .instrument_id = domain::InstrumentId{7U},
                  .active_order_count = 4U,
                  .bids =
                      {
                          {
                              .price = domain::PriceTicks{100},
                              .aggregate_quantity = domain::Quantity{12U},
                              .orders =
                                  {
                                      order(1U, 11U, domain::Side::buy, 100, 5U, 1U),
                                      order(2U, 12U, domain::Side::buy, 100, 7U, 2U),
                                  },
                          },
                          {
                              .price = domain::PriceTicks{90},
                              .aggregate_quantity = domain::Quantity{3U},
                              .orders =
                                  {
                                      order(4U, 14U, domain::Side::buy, 90, 3U, 3U),
                                  },
                          },
                      },
                  .asks =
                      {
                          {
                              .price = domain::PriceTicks{110},
                              .aggregate_quantity = domain::Quantity{9U},
                              .orders =
                                  {
                                      order(3U, 13U, domain::Side::sell, 110, 9U, 4U),
                                  },
                          },
                      },
              },
              {
                  .instrument_id = domain::InstrumentId{9U},
                  .active_order_count = 0U,
                  .bids = {},
                  .asks = {},
              },
          },
  };
}

[[nodiscard]] LogId representative_log_id() {
  return {
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
  };
}

[[nodiscard]] SnapshotFile representative_file() {
  auto result = make_snapshot_file(representative_engine_snapshot(), representative_log_id(), 456U);
  EXPECT_TRUE(result) << to_string(result.error.category);
  return std::move(*result.value);
}

[[nodiscard]] std::vector<std::uint8_t> representative_bytes() {
  auto encoded = encode_snapshot(representative_file());
  EXPECT_TRUE(encoded) << to_string(encoded.error.category);
  return std::move(*encoded.value);
}

TEST(PersistenceSnapshotCodec, MatchesTheReviewedAtlssn01GoldenBytes) {
  constexpr std::string_view expected{
      "41544c53534e30310001000601020304000000000000023100000000000000e10011223344556677"
      "8899aabbccddeeff000000000000000700000000000001c800000000000000000400000000000000"
      "1e00000002000000000000003800000002000000000000014ca370b7f2421f4f54ebfdb409c0e957"
      "70c09f9d7bcd34e979459faac90fb1f396957270029cbc49b98c90553190ea1e918b67fb080eab4cf"
      "ddc5762c2f6f1f2a40000000700000000000000640000000000000005000000000000000a00000009"
      "00000000000000c8000000000000000a000000000000001400000000000001280000000700000000"
      "00000004000000000000000200000000000000010000000000000072000000000000006400000000"
      "0000000c000000000000000200000000000000010000000b00000007010000000000000064000000"
      "0000000005000000000000000100000000000000020000000c000000070100000000000000640000"
      "00000000000700000000000000020000000000000049000000000000005a00000000000000030000"
      "00000000000100000000000000040000000e0000000701000000000000005a000000000000000300"
      "000000000000030000000000000049000000000000006e0000000000000009000000000000000100"
      "000000000000030000000d0000000702000000000000006e00000000000000090000000000000004"
      "000000000000002400000009000000000000000000000000000000000000000000000000f3a24bf4"};

  const auto encoded = representative_bytes();

  EXPECT_EQ(encoded.size(), 561U);
  EXPECT_EQ(hex(encoded), expected);
}

TEST(PersistenceSnapshotCodec, RoundTripsCanonicalStateAndHostRepresentation) {
  const auto expected_file = representative_file();
  const auto encoded = encode_snapshot(expected_file);
  ASSERT_TRUE(encoded);

  const auto decoded = decode_snapshot(*encoded.value);
  ASSERT_TRUE(decoded) << to_string(decoded.error.category);
  EXPECT_EQ(*decoded.value, expected_file);
  EXPECT_EQ(decoded.value->log_id.hex(), "00112233445566778899aabbccddeeff");
  EXPECT_EQ(decoded.value->covered_sequence, domain::Sequence{7U});
  EXPECT_EQ(decoded.value->covered_log_byte_offset, 456U);

  const auto host = host_engine_snapshot(*decoded.value);
  ASSERT_TRUE(host) << to_string(host.error.category);
  EXPECT_EQ(*host.value, representative_engine_snapshot());
  EXPECT_EQ(atlaslob::state_digest(*host.value), decoded.value->state_digest);
}

TEST(PersistenceSnapshotCodec, IncludesEveryConfiguredInstrumentEvenWhenEmpty) {
  const auto decoded = decode_snapshot(representative_bytes());

  ASSERT_TRUE(decoded);
  ASSERT_EQ(decoded.value->instruments.size(), 2U);
  EXPECT_EQ(decoded.value->instruments[1].instrument_id, domain::InstrumentId{9U});
  EXPECT_EQ(decoded.value->instruments[1].active_order_count, 0U);
  EXPECT_TRUE(decoded.value->instruments[1].bids.empty());
  EXPECT_TRUE(decoded.value->instruments[1].asks.empty());
}

TEST(PersistenceSnapshotCodec, RejectsEveryByteBoundaryTruncationWithoutAllocationFromCounts) {
  const auto encoded = representative_bytes();

  for (std::size_t size = 0U; size < encoded.size(); ++size) {
    const auto decoded = decode_snapshot(std::span{encoded}.first(size));
    EXPECT_FALSE(decoded) << "size=" << size;
    EXPECT_EQ(decoded.error.category, SnapshotErrorCategory::invalid_length) << "size=" << size;
  }
}

TEST(PersistenceSnapshotCodec, EnforcesTheCallerSnapshotBoundBeforeDecodingOrEncoding) {
  const auto encoded = representative_bytes();
  const CodecLimits too_small{
      .max_snapshot_bytes = static_cast<std::uint64_t>(encoded.size() - 1U),
  };

  const auto inspected =
      inspect_snapshot_length(std::span{encoded}.first(snapshot_fixed_prefix_bytes), too_small);
  const auto decoded = decode_snapshot(encoded, too_small);
  const auto reencoded = encode_snapshot(representative_file(), too_small);

  EXPECT_FALSE(inspected);
  EXPECT_EQ(inspected.error.category, SnapshotErrorCategory::excessive_length);
  EXPECT_FALSE(decoded);
  EXPECT_EQ(decoded.error.category, SnapshotErrorCategory::excessive_length);
  EXPECT_FALSE(reencoded);
  EXPECT_EQ(reencoded.error.category, SnapshotErrorCategory::excessive_length);
}

TEST(PersistenceSnapshotCodec, DetectsSingleBitCorruptionAtEveryByte) {
  const auto canonical = representative_bytes();

  for (std::size_t index = 0U; index < canonical.size(); ++index) {
    auto corrupted = canonical;
    corrupted[index] ^= 0x01U;
    EXPECT_FALSE(decode_snapshot(corrupted)) << "index=" << index;
  }
}

TEST(PersistenceSnapshotCodec, ValidatesChecksumBeforeInterpretingCompletePayload) {
  auto corrupted = representative_bytes();
  corrupted[8U] = 0xffU;

  const auto checksum = decode_snapshot(corrupted);
  EXPECT_FALSE(checksum);
  EXPECT_EQ(checksum.error.category, SnapshotErrorCategory::bad_checksum);

  refresh_crc(corrupted);
  const auto format = decode_snapshot(corrupted);
  EXPECT_FALSE(format);
  EXPECT_EQ(format.error.category, SnapshotErrorCategory::unsupported_format_version);
}

TEST(PersistenceSnapshotCodec, DistinguishesConfigurationAndStateDigestFailures) {
  const auto canonical = representative_bytes();

  auto bad_configuration = canonical;
  bad_configuration[105U] ^= 0x01U;
  refresh_crc(bad_configuration);
  const auto configuration = decode_snapshot(bad_configuration);
  expect_error(configuration, SnapshotErrorCategory::configuration_digest_mismatch, 105U);

  auto bad_state = canonical;
  bad_state[137U] ^= 0x01U;
  refresh_crc(bad_state);
  const auto state = decode_snapshot(bad_state);
  expect_error(state, SnapshotErrorCategory::state_digest_mismatch, 137U);
}

TEST(PersistenceSnapshotCodec, ReportsCanonicalHeaderCatalogAndHierarchyOffsets) {
  const auto canonical = representative_bytes();

  auto marker = canonical;
  marker[12U] ^= 0x01U;
  refresh_crc(marker);
  expect_error(decode_snapshot(marker), SnapshotErrorCategory::unsupported_format_version, 12U);

  auto duplicate_catalog = canonical;
  write_u32(duplicate_catalog, 197U, 7U);
  refresh_crc(duplicate_catalog);
  expect_error(decode_snapshot(duplicate_catalog), SnapshotErrorCategory::invalid_catalog, 197U);

  auto duplicate_instrument = canonical;
  write_u32(duplicate_instrument, 529U, 7U);
  refresh_crc(duplicate_instrument);
  expect_error(decode_snapshot(duplicate_instrument),
               SnapshotErrorCategory::invalid_snapshot_schema, 529U);

  auto duplicate_order = canonical;
  write_u64(duplicate_order, 334U, 1U);
  refresh_crc(duplicate_order);
  expect_error(decode_snapshot(duplicate_order), SnapshotErrorCategory::invalid_snapshot_schema,
               334U);

  auto duplicate_priority = canonical;
  write_u64(duplicate_priority, 367U, 1U);
  refresh_crc(duplicate_priority);
  expect_error(decode_snapshot(duplicate_priority), SnapshotErrorCategory::invalid_snapshot_schema,
               367U);

  auto invalid_side = canonical;
  invalid_side[309U] = static_cast<std::uint8_t>(domain::Side::sell);
  refresh_crc(invalid_side);
  expect_error(decode_snapshot(invalid_side), SnapshotErrorCategory::invalid_snapshot_schema, 309U);

  auto wrong_aggregate = canonical;
  write_u64(wrong_aggregate, 277U, 13U);
  refresh_crc(wrong_aggregate);
  expect_error(decode_snapshot(wrong_aggregate), SnapshotErrorCategory::invalid_snapshot_schema,
               277U);

  auto crossed = canonical;
  write_u64(crossed, 456U, 100U);
  write_u64(crossed, 497U, 100U);
  refresh_crc(crossed);
  expect_error(decode_snapshot(crossed), SnapshotErrorCategory::invalid_snapshot_schema, 456U);
}

TEST(PersistenceSnapshotCodec, PropagatesCanonicalValueOffsetsThroughEveryValueEntryPoint) {
  auto invalid_engine = representative_engine_snapshot();
  invalid_engine.instruments[0].bids[0].orders[0].remaining_quantity = domain::Quantity{};

  const auto made = make_snapshot_file(invalid_engine, representative_log_id(), 456U);
  expect_error(made, SnapshotErrorCategory::invalid_snapshot_schema, 318U);

  auto invalid_file = representative_file();
  invalid_file.instruments[0].bids[0].orders[0].remaining_quantity = domain::Quantity{};

  const auto hosted = host_engine_snapshot(invalid_file);
  expect_error(hosted, SnapshotErrorCategory::invalid_snapshot_schema, 318U);

  const auto encoded = encode_snapshot(invalid_file);
  expect_error(encoded, SnapshotErrorCategory::invalid_snapshot_schema, 318U);
}

TEST(PersistenceSnapshotCodec, ReportsTheFirstTrailingByteAfterCanonicalInstrumentBlocks) {
  auto trailing = representative_bytes();
  trailing.insert(trailing.end() - 4, 0xffU);
  write_u64(trailing, 16U, static_cast<std::uint64_t>(trailing.size()));
  write_u64(trailing, 97U, 333U);
  refresh_crc(trailing);

  expect_error(decode_snapshot(trailing), SnapshotErrorCategory::invalid_length, 557U);
}

TEST(PersistenceSnapshotCodec, RejectsMalformedLengthsAndCountBombsBeforeReserve) {
  const auto canonical = representative_bytes();

  auto header_length = canonical;
  write_u64(header_length, 24U, std::numeric_limits<std::uint64_t>::max());
  refresh_crc(header_length);
  EXPECT_EQ(decode_snapshot(header_length).error.category, SnapshotErrorCategory::invalid_length);

  auto catalog_count = canonical;
  write_u32(catalog_count, 81U, std::numeric_limits<std::uint32_t>::max());
  refresh_crc(catalog_count);
  EXPECT_EQ(decode_snapshot(catalog_count).error.category, SnapshotErrorCategory::invalid_length);

  auto instrument_block = canonical;
  write_u64(instrument_block, 225U, std::numeric_limits<std::uint64_t>::max());
  refresh_crc(instrument_block);
  EXPECT_EQ(decode_snapshot(instrument_block).error.category,
            SnapshotErrorCategory::invalid_length);

  auto order_count = canonical;
  write_u64(order_count, 285U, std::numeric_limits<std::uint64_t>::max());
  refresh_crc(order_count);
  EXPECT_EQ(decode_snapshot(order_count).error.category, SnapshotErrorCategory::invalid_length);

  auto claimed_active_count = canonical;
  write_u64(claimed_active_count, 65U, std::numeric_limits<std::uint64_t>::max());
  refresh_crc(claimed_active_count);
  EXPECT_EQ(decode_snapshot(claimed_active_count).error.category,
            SnapshotErrorCategory::invalid_snapshot_schema);
}

TEST(PersistenceSnapshotCodec, RejectsInvalidCatalogAndInstrumentTopology) {
  const auto canonical = representative_bytes();

  auto duplicate_catalog = canonical;
  write_u32(duplicate_catalog, 197U, 7U);
  refresh_crc(duplicate_catalog);
  EXPECT_EQ(decode_snapshot(duplicate_catalog).error.category,
            SnapshotErrorCategory::invalid_catalog);

  auto duplicate_instrument = canonical;
  write_u32(duplicate_instrument, 529U, 7U);
  refresh_crc(duplicate_instrument);
  EXPECT_EQ(decode_snapshot(duplicate_instrument).error.category,
            SnapshotErrorCategory::invalid_snapshot_schema);
}

TEST(PersistenceSnapshotCodec, RejectsDuplicateOrderIdsAndGlobalPriorities) {
  const auto canonical = representative_bytes();

  auto duplicate_id = canonical;
  write_u64(duplicate_id, 334U, 1U);
  refresh_crc(duplicate_id);
  EXPECT_EQ(decode_snapshot(duplicate_id).error.category,
            SnapshotErrorCategory::invalid_snapshot_schema);

  auto duplicate_priority = canonical;
  write_u64(duplicate_priority, 367U, 1U);
  refresh_crc(duplicate_priority);
  EXPECT_EQ(decode_snapshot(duplicate_priority).error.category,
            SnapshotErrorCategory::invalid_snapshot_schema);
}

TEST(PersistenceSnapshotCodec, RejectsInvalidOrderHierarchyQuantityAndPriority) {
  const auto canonical = representative_bytes();

  auto wrong_side = canonical;
  wrong_side[309U] = static_cast<std::uint8_t>(domain::Side::sell);
  refresh_crc(wrong_side);
  EXPECT_EQ(decode_snapshot(wrong_side).error.category,
            SnapshotErrorCategory::invalid_snapshot_schema);

  auto wrong_price = canonical;
  write_u64(wrong_price, 310U, 105U);
  refresh_crc(wrong_price);
  EXPECT_EQ(decode_snapshot(wrong_price).error.category,
            SnapshotErrorCategory::invalid_snapshot_schema);

  auto zero_quantity = canonical;
  write_u64(zero_quantity, 318U, 0U);
  refresh_crc(zero_quantity);
  EXPECT_EQ(decode_snapshot(zero_quantity).error.category,
            SnapshotErrorCategory::invalid_snapshot_schema);

  auto future_priority = canonical;
  write_u64(future_priority, 326U, 8U);
  refresh_crc(future_priority);
  EXPECT_EQ(decode_snapshot(future_priority).error.category,
            SnapshotErrorCategory::invalid_snapshot_schema);
}

TEST(PersistenceSnapshotCodec, RejectsWrongAggregatesCountsLevelOrderAndCrossedBooks) {
  const auto canonical = representative_bytes();

  auto aggregate = canonical;
  write_u64(aggregate, 277U, 13U);
  refresh_crc(aggregate);
  EXPECT_EQ(decode_snapshot(aggregate).error.category,
            SnapshotErrorCategory::invalid_snapshot_schema);

  auto active_count = canonical;
  write_u64(active_count, 237U, 5U);
  refresh_crc(active_count);
  EXPECT_EQ(decode_snapshot(active_count).error.category,
            SnapshotErrorCategory::invalid_snapshot_schema);

  auto level_order = canonical;
  write_u64(level_order, 383U, 100U);
  write_u64(level_order, 424U, 100U);
  refresh_crc(level_order);
  EXPECT_EQ(decode_snapshot(level_order).error.category,
            SnapshotErrorCategory::invalid_snapshot_schema);

  auto crossed = canonical;
  write_u64(crossed, 456U, 100U);
  write_u64(crossed, 497U, 100U);
  refresh_crc(crossed);
  EXPECT_EQ(decode_snapshot(crossed).error.category,
            SnapshotErrorCategory::invalid_snapshot_schema);
}

TEST(PersistenceSnapshotCodec, RejectsInconsistentExhaustionState) {
  auto encoded = representative_bytes();
  encoded[64U] = 1U;
  refresh_crc(encoded);

  const auto decoded = decode_snapshot(encoded);

  EXPECT_FALSE(decoded);
  EXPECT_EQ(decoded.error.category, SnapshotErrorCategory::invalid_snapshot_schema);
}

TEST(PersistenceSnapshotCodec, AcceptsTheOnlyValidExhaustedSequenceState) {
  auto snapshot = representative_engine_snapshot();
  snapshot.last_sequence = domain::Sequence{std::numeric_limits<std::uint64_t>::max()};
  snapshot.sequence_exhausted = true;
  auto file = make_snapshot_file(snapshot, representative_log_id(), 999U);
  ASSERT_TRUE(file) << to_string(file.error.category);

  const auto encoded = encode_snapshot(*file.value);
  ASSERT_TRUE(encoded);
  const auto decoded = decode_snapshot(*encoded.value);

  ASSERT_TRUE(decoded) << to_string(decoded.error.category);
  EXPECT_TRUE(decoded.value->sequence_exhausted);
  EXPECT_EQ(decoded.value->covered_sequence, snapshot.last_sequence);
}

TEST(PersistenceSnapshotCodec, InitialSnapshotRequiresTheExactEmptyLogHeaderBoundary) {
  auto snapshot = representative_engine_snapshot();
  snapshot.last_sequence = {};
  snapshot.active_order_count = 0U;
  for (auto& instrument : snapshot.instruments) {
    instrument.active_order_count = 0U;
    instrument.bids.clear();
    instrument.asks.clear();
  }

  const auto canonical = make_snapshot_file(snapshot, representative_log_id(), 152U);
  ASSERT_TRUE(canonical) << to_string(canonical.error.category);
  EXPECT_TRUE(encode_snapshot(*canonical.value));

  const auto wrong_boundary = make_snapshot_file(snapshot, representative_log_id(), 151U);
  EXPECT_FALSE(wrong_boundary);
  EXPECT_EQ(wrong_boundary.error.category, SnapshotErrorCategory::invalid_snapshot_schema);

  const auto positive_at_header =
      make_snapshot_file(representative_engine_snapshot(), representative_log_id(), 152U);
  EXPECT_FALSE(positive_at_header);
  EXPECT_EQ(positive_at_header.error.category, SnapshotErrorCategory::invalid_snapshot_schema);
}

TEST(PersistenceSnapshotCodec, EncoderRefusesNonCanonicalOrInconsistentValues) {
  auto invalid_catalog = representative_file();
  std::swap(invalid_catalog.catalog[0], invalid_catalog.catalog[1]);
  EXPECT_EQ(encode_snapshot(invalid_catalog).error.category,
            SnapshotErrorCategory::invalid_catalog);

  auto invalid_state = representative_file();
  invalid_state.instruments[0].bids[0].orders[0].remaining_quantity = domain::Quantity{};
  EXPECT_EQ(encode_snapshot(invalid_state).error.category,
            SnapshotErrorCategory::invalid_snapshot_schema);

  auto bad_configuration_digest = representative_file();
  bad_configuration_digest.configuration_digest.bytes[0] ^= 0x01U;
  EXPECT_EQ(encode_snapshot(bad_configuration_digest).error.category,
            SnapshotErrorCategory::configuration_digest_mismatch);

  auto bad_state_digest = representative_file();
  bad_state_digest.state_digest.bytes[0] ^= 0x01U;
  EXPECT_EQ(encode_snapshot(bad_state_digest).error.category,
            SnapshotErrorCategory::state_digest_mismatch);
}

TEST(PersistenceSnapshotCodec, ErrorVocabularyIsStableAndComplete) {
  EXPECT_EQ(to_string(SnapshotErrorCategory::none), "none");
  EXPECT_EQ(to_string(SnapshotErrorCategory::invalid_length), "invalid_length");
  EXPECT_EQ(to_string(SnapshotErrorCategory::excessive_length), "excessive_length");
  EXPECT_EQ(to_string(SnapshotErrorCategory::unsupported_format_version),
            "unsupported_format_version");
  EXPECT_EQ(to_string(SnapshotErrorCategory::semantic_version_mismatch),
            "semantic_version_mismatch");
  EXPECT_EQ(to_string(SnapshotErrorCategory::bad_checksum), "bad_checksum");
  EXPECT_EQ(to_string(SnapshotErrorCategory::configuration_digest_mismatch),
            "configuration_digest_mismatch");
  EXPECT_EQ(to_string(SnapshotErrorCategory::invalid_catalog), "invalid_catalog");
  EXPECT_EQ(to_string(SnapshotErrorCategory::invalid_snapshot_schema), "invalid_snapshot_schema");
  EXPECT_EQ(to_string(SnapshotErrorCategory::state_digest_mismatch), "state_digest_mismatch");
  EXPECT_EQ(to_string(SnapshotErrorCategory::log_id_mismatch), "log_id_mismatch");
  EXPECT_EQ(to_string(SnapshotErrorCategory::log_boundary_mismatch), "log_boundary_mismatch");
  EXPECT_EQ(to_string(SnapshotErrorCategory::sequence_mismatch), "sequence_mismatch");
  EXPECT_EQ(to_string(SnapshotErrorCategory::io_failure), "io_failure");
  EXPECT_EQ(to_string(static_cast<SnapshotErrorCategory>(255U)), "unknown");
}

}  // namespace
