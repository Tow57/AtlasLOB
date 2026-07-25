#include "command_log_codec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "atlaslob/persistence/configuration_digest.hpp"
#include "binary_codec.hpp"
#include "crc32c.hpp"

namespace atlaslob::persistence::detail {
namespace {

template <typename Value>
[[nodiscard]] CodecResult<Value> failure(LogErrorCategory category, std::size_t offset) noexcept {
  return {
      .value = std::nullopt,
      .error =
          {
              .category = category,
              .byte_offset = static_cast<std::uint64_t>(offset),
          },
  };
}

template <typename Value>
[[nodiscard]] CodecResult<Value> success(Value value) {
  return {
      .value = std::move(value),
      .error = {},
  };
}

[[nodiscard]] bool capacity_fits_u64(std::size_t value) noexcept {
  if constexpr (sizeof(std::size_t) <= sizeof(std::uint64_t)) {
    return true;
  } else {
    return value <= static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max());
  }
}

[[nodiscard]] bool persisted_catalog_valid(
    std::span<const PersistedInstrumentConfig> catalog) noexcept {
  if (catalog.empty()) {
    return false;
  }

  domain::InstrumentId previous{};
  for (const auto& instrument : catalog) {
    if (instrument.instrument_id.value() == 0U || instrument.instrument_id <= previous ||
        instrument.max_order_quantity == 0U || instrument.tick_increment.value() <= 0) {
      return false;
    }
    previous = instrument.instrument_id;
  }
  return true;
}

[[nodiscard]] bool header_lengths(std::uint32_t header_length, std::uint32_t catalog_length,
                                  std::uint32_t catalog_count, CodecLimits limits,
                                  LogErrorCategory& category) noexcept {
  if (header_length < command_log_header_fixed_bytes ||
      catalog_length % command_log_catalog_entry_bytes != 0U) {
    category = LogErrorCategory::invalid_length;
    return false;
  }
  if (header_length > limits.max_header_bytes) {
    category = LogErrorCategory::excessive_length;
    return false;
  }

  std::size_t expected_catalog_length = 0U;
  std::size_t expected_header_length = 0U;
  if (!checked_multiply(static_cast<std::size_t>(catalog_count), command_log_catalog_entry_bytes,
                        expected_catalog_length) ||
      !checked_add(command_log_header_fixed_bytes, expected_catalog_length,
                   expected_header_length) ||
      expected_catalog_length != catalog_length || expected_header_length != header_length) {
    category = LogErrorCategory::invalid_length;
    return false;
  }
  return true;
}

void encode_catalog_entry(BinaryEncoder& encoder, const PersistedInstrumentConfig& instrument) {
  encoder.u32(instrument.instrument_id.value());
  encoder.u64(instrument.max_order_quantity);
  encoder.i64(instrument.tick_increment.value());
  encoder.u64(instrument.max_active_orders);
}

[[nodiscard]] bool decode_catalog_entry(BinaryDecoder& decoder,
                                        PersistedInstrumentConfig& instrument) {
  std::uint32_t instrument_id = 0U;
  std::uint64_t maximum_quantity = 0U;
  std::int64_t tick_increment = 0;
  std::uint64_t maximum_active = 0U;
  if (!decoder.u32(instrument_id) || !decoder.u64(maximum_quantity) ||
      !decoder.i64(tick_increment) || !decoder.u64(maximum_active)) {
    return false;
  }
  instrument = {
      .instrument_id = domain::InstrumentId{instrument_id},
      .max_order_quantity = maximum_quantity,
      .tick_increment = domain::PriceTicks{tick_increment},
      .max_active_orders = maximum_active,
  };
  return true;
}

[[nodiscard]] std::size_t payload_size(domain::CommandType type) noexcept {
  switch (type) {
    case domain::CommandType::new_order:
      return new_order_payload_bytes;
    case domain::CommandType::cancel:
      return cancel_order_payload_bytes;
    case domain::CommandType::replace:
      return replace_order_payload_bytes;
  }
  return 0U;
}

void encode_command_payload(BinaryEncoder& encoder, const domain::Command& command) {
  std::visit(
      [&encoder](const auto& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        encoder.u32(value.client_id.value());
        if constexpr (std::is_same_v<Value, domain::NewOrder>) {
          encoder.u64(value.order_id.value());
          encoder.u32(value.instrument_id.value());
          encoder.u8(static_cast<std::uint8_t>(value.side));
          encoder.u8(static_cast<std::uint8_t>(value.order_type));
          encoder.u8(static_cast<std::uint8_t>(value.time_in_force));
          encoder.u8(value.limit_price.has_value() ? 1U : 0U);
          encoder.i64(value.limit_price.has_value() ? value.limit_price->value() : 0);
          encoder.u64(value.quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::CancelOrder>) {
          encoder.u64(value.order_id.value());
          encoder.u32(value.instrument_id.value());
        } else {
          static_assert(std::is_same_v<Value, domain::ReplaceOrder>);
          encoder.u64(value.old_order_id.value());
          encoder.u64(value.new_order_id.value());
          encoder.u32(value.instrument_id.value());
          encoder.i64(value.new_limit_price.value());
          encoder.u64(value.new_quantity.value());
        }
      },
      command);
}

[[nodiscard]] CodecResult<domain::Command> decode_command_payload(
    domain::CommandType type, std::span<const std::uint8_t> payload, std::size_t payload_offset) {
  BinaryDecoder decoder{payload};
  std::uint32_t client_id = 0U;
  if (!decoder.u32(client_id)) {
    return failure<domain::Command>(LogErrorCategory::invalid_command_schema, payload_offset);
  }

  switch (type) {
    case domain::CommandType::new_order: {
      std::uint64_t order_id = 0U;
      std::uint32_t instrument_id = 0U;
      std::uint8_t side = 0U;
      std::uint8_t order_type = 0U;
      std::uint8_t time_in_force = 0U;
      std::uint8_t price_present = 0U;
      std::int64_t price = 0;
      std::uint64_t quantity = 0U;
      if (!decoder.u64(order_id) || !decoder.u32(instrument_id) || !decoder.u8(side) ||
          !decoder.u8(order_type) || !decoder.u8(time_in_force) || !decoder.u8(price_present) ||
          !decoder.i64(price) || !decoder.u64(quantity) || decoder.remaining() != 0U) {
        return failure<domain::Command>(LogErrorCategory::invalid_command_schema,
                                        payload_offset + decoder.position());
      }
      if (price_present > 1U || (price_present == 0U && price != 0)) {
        return failure<domain::Command>(LogErrorCategory::invalid_command_schema,
                                        payload_offset + 19U);
      }
      return success<domain::Command>(domain::NewOrder{
          .client_id = domain::ClientId{client_id},
          .order_id = domain::OrderId{order_id},
          .instrument_id = domain::InstrumentId{instrument_id},
          .side = static_cast<domain::Side>(side),
          .order_type = static_cast<domain::OrderType>(order_type),
          .time_in_force = static_cast<domain::TimeInForce>(time_in_force),
          .limit_price = price_present == 0U
                             ? std::optional<domain::PriceTicks>{}
                             : std::optional<domain::PriceTicks>{domain::PriceTicks{price}},
          .quantity = domain::Quantity{quantity},
      });
    }
    case domain::CommandType::cancel: {
      std::uint64_t order_id = 0U;
      std::uint32_t instrument_id = 0U;
      if (!decoder.u64(order_id) || !decoder.u32(instrument_id) || decoder.remaining() != 0U) {
        return failure<domain::Command>(LogErrorCategory::invalid_command_schema,
                                        payload_offset + decoder.position());
      }
      return success<domain::Command>(domain::CancelOrder{
          .client_id = domain::ClientId{client_id},
          .order_id = domain::OrderId{order_id},
          .instrument_id = domain::InstrumentId{instrument_id},
      });
    }
    case domain::CommandType::replace: {
      std::uint64_t old_order_id = 0U;
      std::uint64_t new_order_id = 0U;
      std::uint32_t instrument_id = 0U;
      std::int64_t price = 0;
      std::uint64_t quantity = 0U;
      if (!decoder.u64(old_order_id) || !decoder.u64(new_order_id) || !decoder.u32(instrument_id) ||
          !decoder.i64(price) || !decoder.u64(quantity) || decoder.remaining() != 0U) {
        return failure<domain::Command>(LogErrorCategory::invalid_command_schema,
                                        payload_offset + decoder.position());
      }
      return success<domain::Command>(domain::ReplaceOrder{
          .client_id = domain::ClientId{client_id},
          .old_order_id = domain::OrderId{old_order_id},
          .new_order_id = domain::OrderId{new_order_id},
          .instrument_id = domain::InstrumentId{instrument_id},
          .new_limit_price = domain::PriceTicks{price},
          .new_quantity = domain::Quantity{quantity},
      });
    }
  }
  return failure<domain::Command>(LogErrorCategory::unknown_record_type, 10U);
}

}  // namespace

CodecResult<LogHeader> make_log_header(std::span<const InstrumentConfig> canonical_catalog,
                                       MultiInstrumentEngineConfig engine_config, LogId log_id) {
  if (canonical_catalog.empty() || !capacity_fits_u64(engine_config.max_total_active_orders)) {
    return failure<LogHeader>(LogErrorCategory::catalog_configuration_mismatch, 0U);
  }

  std::vector<PersistedInstrumentConfig> persisted_catalog;
  persisted_catalog.reserve(canonical_catalog.size());
  domain::InstrumentId previous{};
  for (const auto& instrument : canonical_catalog) {
    if (instrument.instrument_id.value() == 0U || instrument.instrument_id <= previous ||
        !instrument.matching.valid() || !capacity_fits_u64(instrument.matching.max_active_orders)) {
      return failure<LogHeader>(LogErrorCategory::catalog_configuration_mismatch, 0U);
    }
    persisted_catalog.push_back({
        .instrument_id = instrument.instrument_id,
        .max_order_quantity = instrument.matching.max_order_quantity.value(),
        .tick_increment = instrument.matching.tick_increment,
        .max_active_orders = canonical_capacity(instrument.matching.max_active_orders),
    });
    previous = instrument.instrument_id;
  }

  return success<LogHeader>({
      .format_version = command_log_format_version,
      .semantics_version = atlaslob_semantics_version,
      .log_id = log_id,
      .first_sequence = domain::Sequence{command_log_first_sequence},
      .engine_config =
          {
              .max_total_active_orders = canonical_capacity(engine_config.max_total_active_orders),
          },
      .catalog = std::move(persisted_catalog),
  });
}

CodecResult<HostConfiguration> host_configuration(const LogHeader& header) {
  if (!persisted_catalog_valid(header.catalog)) {
    return failure<HostConfiguration>(LogErrorCategory::catalog_configuration_mismatch, 60U);
  }
  const auto engine_capacity = host_capacity(header.engine_config.max_total_active_orders);
  if (!engine_capacity.has_value()) {
    return failure<HostConfiguration>(LogErrorCategory::catalog_configuration_mismatch, 48U);
  }

  HostConfiguration result{
      .engine_config =
          {
              .max_total_active_orders = *engine_capacity,
          },
      .catalog = {},
  };
  result.catalog.reserve(header.catalog.size());
  for (const auto& instrument : header.catalog) {
    const auto active_capacity = host_capacity(instrument.max_active_orders);
    if (!active_capacity.has_value()) {
      return failure<HostConfiguration>(LogErrorCategory::catalog_configuration_mismatch, 60U);
    }
    result.catalog.push_back({
        .instrument_id = instrument.instrument_id,
        .matching =
            {
                .max_order_quantity = domain::Quantity{instrument.max_order_quantity},
                .tick_increment = instrument.tick_increment,
                .max_active_orders = *active_capacity,
            },
    });
  }
  return success<HostConfiguration>(std::move(result));
}

CodecResult<std::size_t> inspect_log_header_length(std::span<const std::uint8_t> fixed_prefix,
                                                   CodecLimits limits) noexcept {
  if (fixed_prefix.size() < command_log_header_fixed_prefix_bytes) {
    return failure<std::size_t>(LogErrorCategory::invalid_length, fixed_prefix.size());
  }

  BinaryDecoder decoder{fixed_prefix.first(command_log_header_fixed_prefix_bytes)};
  std::span<const std::uint8_t> magic;
  std::uint16_t format_version = 0U;
  std::uint16_t semantics_version = 0U;
  std::uint32_t marker = 0U;
  std::uint32_t header_length = 0U;
  std::uint32_t catalog_length = 0U;
  std::span<const std::uint8_t> log_id;
  std::uint64_t first_sequence = 0U;
  std::uint64_t engine_capacity = 0U;
  std::uint32_t catalog_count = 0U;
  if (!decoder.bytes(command_log_magic.size(), magic) || !decoder.u16(format_version) ||
      !decoder.u16(semantics_version) || !decoder.u32(marker) || !decoder.u32(header_length) ||
      !decoder.u32(catalog_length) || !decoder.bytes(16U, log_id) || !decoder.u64(first_sequence) ||
      !decoder.u64(engine_capacity) || !decoder.u32(catalog_count)) {
    return failure<std::size_t>(LogErrorCategory::invalid_length, decoder.position());
  }
  static_cast<void>(semantics_version);
  static_cast<void>(log_id);
  static_cast<void>(engine_capacity);
  static_cast<void>(magic);
  static_cast<void>(format_version);
  static_cast<void>(marker);
  static_cast<void>(first_sequence);

  LogErrorCategory category = LogErrorCategory::none;
  if (!header_lengths(header_length, catalog_length, catalog_count, limits, category)) {
    return failure<std::size_t>(category, 16U);
  }
  return success<std::size_t>(header_length);
}

CodecResult<std::size_t> inspect_log_record_length(std::span<const std::uint8_t> length_prefix,
                                                   CodecLimits limits) noexcept {
  if (length_prefix.size() < command_log_record_length_prefix_bytes) {
    return failure<std::size_t>(LogErrorCategory::truncated_final_record, length_prefix.size());
  }
  BinaryDecoder decoder{length_prefix.first(command_log_record_length_prefix_bytes)};
  std::uint32_t total_length = 0U;
  std::uint32_t payload_length = 0U;
  if (!decoder.u32(total_length) || !decoder.u32(payload_length)) {
    return failure<std::size_t>(LogErrorCategory::truncated_final_record, decoder.position());
  }
  if (total_length < command_log_record_fixed_bytes) {
    return failure<std::size_t>(LogErrorCategory::invalid_length, 0U);
  }
  if (total_length > limits.max_record_bytes) {
    return failure<std::size_t>(LogErrorCategory::excessive_length, 0U);
  }
  if (total_length - command_log_record_fixed_bytes != payload_length) {
    return failure<std::size_t>(LogErrorCategory::invalid_length, 4U);
  }
  return success<std::size_t>(total_length);
}

CodecResult<std::vector<std::uint8_t>> encode_log_header(const LogHeader& header,
                                                         CodecLimits limits) {
  if (header.format_version != command_log_format_version) {
    return failure<std::vector<std::uint8_t>>(LogErrorCategory::unsupported_format_version, 8U);
  }
  if (header.semantics_version != atlaslob_semantics_version) {
    return failure<std::vector<std::uint8_t>>(LogErrorCategory::semantic_version_mismatch, 10U);
  }
  if (header.first_sequence.value() != command_log_first_sequence) {
    return failure<std::vector<std::uint8_t>>(LogErrorCategory::unsupported_format_version, 40U);
  }
  if (header.catalog.size() > std::numeric_limits<std::uint32_t>::max()) {
    return failure<std::vector<std::uint8_t>>(LogErrorCategory::excessive_length, 56U);
  }

  auto canonical_catalog = header.catalog;
  std::sort(canonical_catalog.begin(), canonical_catalog.end(),
            [](const PersistedInstrumentConfig& left, const PersistedInstrumentConfig& right) {
              return left.instrument_id < right.instrument_id;
            });
  if (!persisted_catalog_valid(canonical_catalog)) {
    return failure<std::vector<std::uint8_t>>(LogErrorCategory::catalog_configuration_mismatch,
                                              48U);
  }

  std::size_t catalog_length = 0U;
  std::size_t header_length = 0U;
  if (!checked_multiply(canonical_catalog.size(), command_log_catalog_entry_bytes,
                        catalog_length) ||
      !checked_add(command_log_header_fixed_bytes, catalog_length, header_length) ||
      header_length > std::numeric_limits<std::uint32_t>::max()) {
    return failure<std::vector<std::uint8_t>>(LogErrorCategory::excessive_length, 16U);
  }
  if (header_length > limits.max_header_bytes) {
    return failure<std::vector<std::uint8_t>>(LogErrorCategory::excessive_length, 16U);
  }

  BinaryEncoder encoder{header_length};
  encoder.bytes(command_log_magic);
  encoder.u16(header.format_version);
  encoder.u16(header.semantics_version);
  encoder.u32(command_log_byte_order_marker);
  encoder.u32(static_cast<std::uint32_t>(header_length));
  encoder.u32(static_cast<std::uint32_t>(catalog_length));
  encoder.bytes(header.log_id.bytes);
  encoder.u64(header.first_sequence.value());
  encoder.u64(header.engine_config.max_total_active_orders);
  encoder.u32(static_cast<std::uint32_t>(canonical_catalog.size()));
  for (const auto& instrument : canonical_catalog) {
    encode_catalog_entry(encoder, instrument);
  }
  const auto digest =
      configuration_digest(canonical_catalog, header.engine_config, header.semantics_version);
  encoder.bytes(digest.bytes);
  encoder.u32(crc32c(encoder.view()));
  if (encoder.size() != header_length) {
    return failure<std::vector<std::uint8_t>>(LogErrorCategory::invalid_length, encoder.size());
  }
  return success<std::vector<std::uint8_t>>(std::move(encoder).take());
}

CodecResult<LogHeader> decode_log_header(std::span<const std::uint8_t> bytes, CodecLimits limits) {
  const auto length = inspect_log_header_length(bytes, limits);
  if (!length) {
    return {
        .value = std::nullopt,
        .error = length.error,
    };
  }
  if (bytes.size() < *length.value) {
    return failure<LogHeader>(LogErrorCategory::invalid_length, bytes.size());
  }
  const auto encoded = bytes.first(*length.value);
  BinaryDecoder checksum_decoder{encoded.last(4U)};
  std::uint32_t stored_checksum = 0U;
  if (!checksum_decoder.u32(stored_checksum) ||
      stored_checksum != crc32c(encoded.first(encoded.size() - 4U))) {
    return failure<LogHeader>(LogErrorCategory::bad_header_checksum, encoded.size() - 4U);
  }

  BinaryDecoder decoder{encoded};
  std::span<const std::uint8_t> magic;
  std::uint16_t format_version = 0U;
  std::uint16_t semantics_version = 0U;
  std::uint32_t marker = 0U;
  std::uint32_t header_length = 0U;
  std::uint32_t catalog_length = 0U;
  std::span<const std::uint8_t> log_id;
  std::uint64_t first_sequence = 0U;
  std::uint64_t engine_capacity = 0U;
  std::uint32_t catalog_count = 0U;
  if (!decoder.bytes(command_log_magic.size(), magic) || !decoder.u16(format_version) ||
      !decoder.u16(semantics_version) || !decoder.u32(marker) || !decoder.u32(header_length) ||
      !decoder.u32(catalog_length) || !decoder.bytes(16U, log_id) || !decoder.u64(first_sequence) ||
      !decoder.u64(engine_capacity) || !decoder.u32(catalog_count)) {
    return failure<LogHeader>(LogErrorCategory::invalid_length, decoder.position());
  }
  static_cast<void>(header_length);
  static_cast<void>(catalog_length);
  if (!std::equal(command_log_magic.begin(), command_log_magic.end(), magic.begin())) {
    return failure<LogHeader>(LogErrorCategory::unsupported_format_version, 0U);
  }
  if (format_version != command_log_format_version) {
    return failure<LogHeader>(LogErrorCategory::unsupported_format_version, 8U);
  }
  if (marker != command_log_byte_order_marker) {
    return failure<LogHeader>(LogErrorCategory::unsupported_format_version, 12U);
  }
  if (first_sequence != command_log_first_sequence) {
    return failure<LogHeader>(LogErrorCategory::unsupported_format_version, 40U);
  }
  if (semantics_version != atlaslob_semantics_version) {
    return failure<LogHeader>(LogErrorCategory::semantic_version_mismatch, 10U);
  }
  std::vector<PersistedInstrumentConfig> catalog;
  catalog.reserve(catalog_count);
  for (std::uint32_t index = 0U; index < catalog_count; ++index) {
    PersistedInstrumentConfig instrument;
    if (!decode_catalog_entry(decoder, instrument)) {
      return failure<LogHeader>(LogErrorCategory::catalog_configuration_mismatch,
                                decoder.position());
    }
    catalog.push_back(instrument);
  }
  std::span<const std::uint8_t> stored_digest_bytes;
  if (!decoder.bytes(32U, stored_digest_bytes)) {
    return failure<LogHeader>(LogErrorCategory::invalid_length, decoder.position());
  }

  PersistedEngineConfig engine_config{
      .max_total_active_orders = engine_capacity,
  };
  if (!persisted_catalog_valid(catalog)) {
    return failure<LogHeader>(LogErrorCategory::catalog_configuration_mismatch, 60U);
  }
  const auto expected_digest = configuration_digest(catalog, engine_config, semantics_version);
  if (!std::equal(expected_digest.bytes.begin(), expected_digest.bytes.end(),
                  stored_digest_bytes.begin())) {
    return failure<LogHeader>(LogErrorCategory::catalog_configuration_mismatch,
                              encoded.size() - 36U);
  }

  LogHeader result{
      .format_version = format_version,
      .semantics_version = semantics_version,
      .log_id = {},
      .first_sequence = domain::Sequence{first_sequence},
      .engine_config = engine_config,
      .catalog = std::move(catalog),
  };
  std::copy(log_id.begin(), log_id.end(), result.log_id.bytes.begin());
  return success<LogHeader>(std::move(result));
}

CodecResult<std::vector<std::uint8_t>> encode_command_record(const CommandRecord& record,
                                                             CodecLimits limits) {
  if (record.record_version != command_log_record_version) {
    return failure<std::vector<std::uint8_t>>(LogErrorCategory::unsupported_record_version, 8U);
  }
  if (record.sequence.value() == 0U || record.event_count == 0U || !is_valid(record.outcome)) {
    return failure<std::vector<std::uint8_t>>(LogErrorCategory::invalid_command_schema, 11U);
  }
  if ((record.outcome == RecordOutcome::committed &&
       record.rejection_reason != domain::RejectReason::none) ||
      (record.outcome == RecordOutcome::rejected && !domain::is_valid(record.rejection_reason))) {
    return failure<std::vector<std::uint8_t>>(LogErrorCategory::invalid_command_schema, 20U);
  }

  const auto type = domain::command_type(record.command);
  const auto command_payload_size = payload_size(type);
  std::size_t total_size = 0U;
  if (command_payload_size == 0U ||
      !checked_add(command_log_record_fixed_bytes, command_payload_size, total_size) ||
      total_size > std::numeric_limits<std::uint32_t>::max()) {
    return failure<std::vector<std::uint8_t>>(LogErrorCategory::invalid_length, 4U);
  }
  if (total_size > limits.max_record_bytes) {
    return failure<std::vector<std::uint8_t>>(LogErrorCategory::excessive_length, 0U);
  }

  BinaryEncoder encoder{total_size};
  encoder.u32(static_cast<std::uint32_t>(total_size));
  encoder.u32(static_cast<std::uint32_t>(command_payload_size));
  encoder.u16(record.record_version);
  encoder.u8(static_cast<std::uint8_t>(type));
  encoder.u8(static_cast<std::uint8_t>(record.outcome));
  encoder.u64(record.sequence.value());
  encoder.u16(static_cast<std::uint16_t>(record.rejection_reason));
  encoder.u64(record.event_count);
  encoder.bytes(record.event_digest.bytes);
  encode_command_payload(encoder, record.command);
  encoder.u32(crc32c(encoder.view()));
  if (encoder.size() != total_size) {
    return failure<std::vector<std::uint8_t>>(LogErrorCategory::invalid_length, encoder.size());
  }
  return success<std::vector<std::uint8_t>>(std::move(encoder).take());
}

CodecResult<CommandRecord> decode_command_record(std::span<const std::uint8_t> bytes,
                                                 CodecLimits limits) {
  const auto length = inspect_log_record_length(bytes, limits);
  if (!length) {
    return {
        .value = std::nullopt,
        .error = length.error,
    };
  }
  const auto total_length = *length.value;
  if (bytes.size() < total_length) {
    return failure<CommandRecord>(LogErrorCategory::truncated_final_record, bytes.size());
  }
  const auto encoded = bytes.first(total_length);

  BinaryDecoder length_decoder{encoded.first(command_log_record_length_prefix_bytes)};
  std::uint32_t decoded_total_length = 0U;
  std::uint32_t decoded_payload_length = 0U;
  if (!length_decoder.u32(decoded_total_length) || !length_decoder.u32(decoded_payload_length)) {
    return failure<CommandRecord>(LogErrorCategory::invalid_length, length_decoder.position());
  }
  std::size_t expected_total = 0U;
  if (!checked_add(command_log_record_fixed_bytes, static_cast<std::size_t>(decoded_payload_length),
                   expected_total) ||
      expected_total != decoded_total_length) {
    return failure<CommandRecord>(LogErrorCategory::invalid_length, 4U);
  }

  BinaryDecoder checksum_decoder{encoded.last(4U)};
  std::uint32_t stored_checksum = 0U;
  if (!checksum_decoder.u32(stored_checksum) ||
      stored_checksum != crc32c(encoded.first(encoded.size() - 4U))) {
    return failure<CommandRecord>(LogErrorCategory::bad_record_checksum, encoded.size() - 4U);
  }

  BinaryDecoder decoder{encoded};
  std::uint32_t ignored_total = 0U;
  std::uint32_t payload_length = 0U;
  std::uint16_t record_version = 0U;
  std::uint8_t raw_type = 0U;
  std::uint8_t raw_outcome = 0U;
  std::uint64_t sequence = 0U;
  std::uint16_t raw_rejection_reason = 0U;
  std::uint64_t event_count = 0U;
  std::span<const std::uint8_t> digest_bytes;
  if (!decoder.u32(ignored_total) || !decoder.u32(payload_length) || !decoder.u16(record_version) ||
      !decoder.u8(raw_type) || !decoder.u8(raw_outcome) || !decoder.u64(sequence) ||
      !decoder.u16(raw_rejection_reason) || !decoder.u64(event_count) ||
      !decoder.bytes(32U, digest_bytes)) {
    return failure<CommandRecord>(LogErrorCategory::invalid_length, decoder.position());
  }
  if (record_version != command_log_record_version) {
    return failure<CommandRecord>(LogErrorCategory::unsupported_record_version, 8U);
  }

  const auto type = static_cast<domain::CommandType>(raw_type);
  if (!domain::is_valid(type)) {
    return failure<CommandRecord>(LogErrorCategory::unknown_record_type, 10U);
  }
  if (payload_size(type) != payload_length) {
    return failure<CommandRecord>(LogErrorCategory::invalid_length, 4U);
  }
  const auto outcome = static_cast<RecordOutcome>(raw_outcome);
  const auto rejection_reason = static_cast<domain::RejectReason>(raw_rejection_reason);
  if (!is_valid(outcome)) {
    return failure<CommandRecord>(LogErrorCategory::invalid_command_schema, 11U);
  }
  if (sequence == 0U || event_count == 0U ||
      (outcome == RecordOutcome::committed && rejection_reason != domain::RejectReason::none) ||
      (outcome == RecordOutcome::rejected && !domain::is_valid(rejection_reason))) {
    return failure<CommandRecord>(LogErrorCategory::invalid_command_schema, 11U);
  }

  std::span<const std::uint8_t> payload;
  if (!decoder.bytes(payload_length, payload)) {
    return failure<CommandRecord>(LogErrorCategory::invalid_length, decoder.position());
  }
  auto command = decode_command_payload(type, payload, 62U);
  if (!command) {
    return {
        .value = std::nullopt,
        .error = command.error,
    };
  }

  CommandRecord result{
      .record_version = record_version,
      .command = std::move(*command.value),
      .sequence = domain::Sequence{sequence},
      .outcome = outcome,
      .rejection_reason = rejection_reason,
      .event_count = event_count,
      .event_digest = {},
  };
  std::copy(digest_bytes.begin(), digest_bytes.end(), result.event_digest.bytes.begin());
  return success<CommandRecord>(std::move(result));
}

}  // namespace atlaslob::persistence::detail
