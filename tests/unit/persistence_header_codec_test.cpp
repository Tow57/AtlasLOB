#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atlaslob/persistence/configuration_digest.hpp"
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

[[nodiscard]] LogHeader representative_header() {
  LogHeader header{
      .format_version = 1U,
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
      .first_sequence = domain::Sequence{1U},
      .engine_config =
          {
              .max_total_active_orders = 100U,
          },
      // Deliberately reversed: the encoder owns canonical sorting.
      .catalog =
          {
              {
                  .instrument_id = domain::InstrumentId{9U},
                  .max_order_quantity = 2'000U,
                  .tick_increment = domain::PriceTicks{10},
                  .max_active_orders = 32U,
              },
              {
                  .instrument_id = domain::InstrumentId{7U},
                  .max_order_quantity = 1'000U,
                  .tick_increment = domain::PriceTicks{5},
                  .max_active_orders = 16U,
              },
          },
  };
  return header;
}

TEST(PersistenceHeaderCodec, MatchesTheReviewedAtlslg01GoldenBytes) {
  constexpr std::string_view expected{
      "41544c534c4730310001000601020304000000980000003800112233445566778899aabbccddeeff"
      "00000000000000010000000000000064000000020000000700000000000003e80000000000000005"
      "00000000000000100000000900000000000007d0000000000000000a0000000000000020"
      "f93e001822f68d814b1cde645bb290e0efec18c6be81a4f38d6255d4e6052d7e811c888f"};

  const auto encoded = encode_log_header(representative_header());

  ASSERT_TRUE(encoded) << to_string(encoded.error.category);
  EXPECT_EQ(encoded.value->size(), 152U);
  EXPECT_EQ(hex(*encoded.value), expected);
}

TEST(PersistenceHeaderCodec, DecodesCanonicalSortedCatalogAndConfigurationDigest) {
  auto encoded = encode_log_header(representative_header());
  ASSERT_TRUE(encoded);

  auto decoded = decode_log_header(*encoded.value);

  ASSERT_TRUE(decoded) << to_string(decoded.error.category);
  ASSERT_EQ(decoded.value->catalog.size(), 2U);
  EXPECT_EQ(decoded.value->catalog[0].instrument_id, domain::InstrumentId{7U});
  EXPECT_EQ(decoded.value->catalog[1].instrument_id, domain::InstrumentId{9U});
  EXPECT_EQ(decoded.value->engine_config.max_total_active_orders, 100U);
  EXPECT_EQ(decoded.value->log_id.hex(), "00112233445566778899aabbccddeeff");
  EXPECT_EQ(configuration_digest(decoded.value->catalog, decoded.value->engine_config).hex(),
            "f93e001822f68d814b1cde645bb290e0efec18c6be81a4f38d6255d4e6052d7e");
}

TEST(PersistenceHeaderCodec, RejectsAnExcessiveDeclaredLengthBeforeVariableAllocation) {
  auto encoded = encode_log_header(representative_header());
  ASSERT_TRUE(encoded);
  auto prefix = *encoded.value;
  write_u32(prefix, 16U, default_max_log_header_bytes + 1U);

  const auto inspected = inspect_log_header_length(
      std::span<const std::uint8_t>{prefix}.first(command_log_header_fixed_prefix_bytes));

  EXPECT_FALSE(inspected);
  EXPECT_EQ(inspected.error.category, LogErrorCategory::excessive_length);
  EXPECT_EQ(inspected.error.byte_offset, 16U);
}

TEST(PersistenceHeaderCodec, DistinguishesChecksumAndConfigurationDigestFailures) {
  auto encoded = encode_log_header(representative_header());
  ASSERT_TRUE(encoded);

  auto bad_crc = *encoded.value;
  bad_crc[70U] ^= 0x01U;
  const auto checksum_result = decode_log_header(bad_crc);
  EXPECT_FALSE(checksum_result);
  EXPECT_EQ(checksum_result.error.category, LogErrorCategory::bad_header_checksum);

  auto bad_digest = *encoded.value;
  bad_digest[bad_digest.size() - 36U] ^= 0x01U;
  refresh_crc(bad_digest);
  const auto digest_result = decode_log_header(bad_digest);
  EXPECT_FALSE(digest_result);
  EXPECT_EQ(digest_result.error.category, LogErrorCategory::catalog_configuration_mismatch);
}

TEST(PersistenceHeaderCodec, ChecksumPrecedesFormatInterpretationForACompleteHeader) {
  auto encoded = encode_log_header(representative_header());
  ASSERT_TRUE(encoded);
  (*encoded.value)[8U] = 0xffU;

  const auto checksum_result = decode_log_header(*encoded.value);
  EXPECT_FALSE(checksum_result);
  EXPECT_EQ(checksum_result.error.category, LogErrorCategory::bad_header_checksum);

  refresh_crc(*encoded.value);
  const auto version_result = decode_log_header(*encoded.value);
  EXPECT_FALSE(version_result);
  EXPECT_EQ(version_result.error.category, LogErrorCategory::unsupported_format_version);
  EXPECT_EQ(version_result.error.byte_offset, 8U);
}

TEST(PersistenceHeaderCodec, ClassifiesSemanticMarkerAndFirstSequenceAfterChecksumValidation) {
  const auto canonical = encode_log_header(representative_header());
  ASSERT_TRUE(canonical);

  auto semantic = *canonical.value;
  semantic[10U] = 0U;
  semantic[11U] = static_cast<std::uint8_t>(atlaslob_semantics_version + 1U);
  refresh_crc(semantic);
  const auto semantic_result = decode_log_header(semantic);
  EXPECT_FALSE(semantic_result);
  EXPECT_EQ(semantic_result.error.category, LogErrorCategory::semantic_version_mismatch);
  EXPECT_EQ(semantic_result.error.byte_offset, 10U);

  auto marker = *canonical.value;
  marker[12U] ^= 0x01U;
  refresh_crc(marker);
  const auto marker_result = decode_log_header(marker);
  EXPECT_FALSE(marker_result);
  EXPECT_EQ(marker_result.error.category, LogErrorCategory::unsupported_format_version);
  EXPECT_EQ(marker_result.error.byte_offset, 12U);

  auto first_sequence = *canonical.value;
  first_sequence[47U] = 2U;
  refresh_crc(first_sequence);
  const auto first_sequence_result = decode_log_header(first_sequence);
  EXPECT_FALSE(first_sequence_result);
  EXPECT_EQ(first_sequence_result.error.category, LogErrorCategory::unsupported_format_version);
  EXPECT_EQ(first_sequence_result.error.byte_offset, 40U);
}

TEST(PersistenceHeaderCodec, HeaderTruncationIsInvalidAndNeverARepairableTornRecord) {
  auto encoded = encode_log_header(representative_header());
  ASSERT_TRUE(encoded);

  for (std::size_t size = 0U; size < encoded.value->size(); ++size) {
    const auto decoded =
        decode_log_header(std::span<const std::uint8_t>{*encoded.value}.first(size));
    EXPECT_FALSE(decoded) << "size=" << size;
    EXPECT_EQ(decoded.error.category, LogErrorCategory::invalid_length) << "size=" << size;
  }
}

TEST(PersistenceHeaderCodec, RejectsDuplicateAndInvalidCatalogEntriesBeforeEncoding) {
  auto duplicate = representative_header();
  duplicate.catalog[1].instrument_id = duplicate.catalog[0].instrument_id;
  const auto duplicate_result = encode_log_header(duplicate);
  EXPECT_FALSE(duplicate_result);
  EXPECT_EQ(duplicate_result.error.category, LogErrorCategory::catalog_configuration_mismatch);

  auto invalid = representative_header();
  invalid.catalog[0].tick_increment = domain::PriceTicks{};
  const auto invalid_result = encode_log_header(invalid);
  EXPECT_FALSE(invalid_result);
  EXPECT_EQ(invalid_result.error.category, LogErrorCategory::catalog_configuration_mismatch);
}

TEST(PersistenceHeaderCodec, HostWriterAndReplayConversionsPreserveCapacitySentinels) {
  const std::array host_catalog{
      InstrumentConfig{
          .instrument_id = domain::InstrumentId{7U},
          .matching =
              {
                  .max_order_quantity = domain::Quantity{1'000U},
                  .tick_increment = domain::PriceTicks{5},
                  .max_active_orders = std::numeric_limits<std::size_t>::max(),
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
  const MultiInstrumentEngineConfig host_engine_config{
      .max_total_active_orders = std::numeric_limits<std::size_t>::max(),
  };

  auto persisted =
      make_log_header(host_catalog, host_engine_config, representative_header().log_id);

  ASSERT_TRUE(persisted) << to_string(persisted.error.category);
  EXPECT_EQ(persisted.value->engine_config.max_total_active_orders,
            std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(persisted.value->catalog[0].max_active_orders,
            std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(configuration_digest(persisted.value->catalog, persisted.value->engine_config),
            configuration_digest(host_catalog, host_engine_config));

  auto host = host_configuration(*persisted.value);
  ASSERT_TRUE(host) << to_string(host.error.category);
  EXPECT_EQ(host.value->engine_config.max_total_active_orders,
            std::numeric_limits<std::size_t>::max());
  EXPECT_EQ(host.value->catalog[0].matching.max_active_orders,
            std::numeric_limits<std::size_t>::max());
  EXPECT_EQ(host.value->catalog,
            std::vector<InstrumentConfig>(host_catalog.begin(), host_catalog.end()));
}

TEST(PersistenceHeaderCodec, CodecRetainsCanonicalCapacityBeforeHostConstruction) {
  auto header = representative_header();
  header.engine_config.max_total_active_orders = std::numeric_limits<std::uint64_t>::max() - 1U;
  header.catalog[0].max_active_orders = std::numeric_limits<std::uint64_t>::max() - 1U;

  auto encoded = encode_log_header(header);
  ASSERT_TRUE(encoded);
  auto decoded = decode_log_header(*encoded.value);

  ASSERT_TRUE(decoded) << to_string(decoded.error.category);
  EXPECT_EQ(decoded.value->engine_config.max_total_active_orders,
            std::numeric_limits<std::uint64_t>::max() - 1U);
  EXPECT_EQ(decoded.value->catalog[1].max_active_orders,
            std::numeric_limits<std::uint64_t>::max() - 1U);

  const auto host = host_configuration(*decoded.value);
  if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
    EXPECT_FALSE(host);
    EXPECT_EQ(host.error.category, LogErrorCategory::catalog_configuration_mismatch);
  } else {
    EXPECT_TRUE(host);
  }
}

TEST(PersistenceHeaderCodec, WriterConversionRequiresCanonicalSortedHostCatalog) {
  std::array host_catalog{
      InstrumentConfig{
          .instrument_id = domain::InstrumentId{9U},
          .matching =
              {
                  .max_order_quantity = domain::Quantity{1U},
                  .tick_increment = domain::PriceTicks{1},
                  .max_active_orders = 1U,
              },
      },
      InstrumentConfig{
          .instrument_id = domain::InstrumentId{7U},
          .matching =
              {
                  .max_order_quantity = domain::Quantity{1U},
                  .tick_increment = domain::PriceTicks{1},
                  .max_active_orders = 1U,
              },
      },
  };

  const auto converted = make_log_header(host_catalog, MultiInstrumentEngineConfig{}, LogId{});

  EXPECT_FALSE(converted);
  EXPECT_EQ(converted.error.category, LogErrorCategory::catalog_configuration_mismatch);
}

}  // namespace
