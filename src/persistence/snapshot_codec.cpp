#include "snapshot_codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

#include "atlaslob/persistence/configuration_digest.hpp"
#include "binary_codec.hpp"
#include "command_log_codec.hpp"
#include "crc32c.hpp"

namespace atlaslob::persistence::detail {
namespace {

template <typename Value>
[[nodiscard]] SnapshotCodecResult<Value> failure(SnapshotErrorCategory category,
                                                 std::uint64_t offset = 0U) noexcept {
  return {
      .value = std::nullopt,
      .error =
          {
              .category = category,
              .byte_offset = offset,
          },
  };
}

template <typename Value>
[[nodiscard]] SnapshotCodecResult<Value> success(Value value) {
  return {
      .value = std::move(value),
      .error = {},
  };
}

[[nodiscard]] bool checked_add_u64(std::uint64_t left, std::uint64_t right,
                                   std::uint64_t& result) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

[[nodiscard]] bool size_to_u64(std::size_t value, std::uint64_t& result) noexcept {
  if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
    if (value > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
      return false;
    }
  }
  result = static_cast<std::uint64_t>(value);
  return true;
}

[[nodiscard]] std::optional<std::size_t> host_count(std::uint64_t value) noexcept {
  if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      return std::nullopt;
    }
  }
  return static_cast<std::size_t>(value);
}

inline constexpr std::uint64_t format_version_offset{8U};
inline constexpr std::uint64_t semantics_version_offset{10U};
inline constexpr std::uint64_t byte_order_marker_offset{12U};
inline constexpr std::uint64_t snapshot_length_offset{16U};
inline constexpr std::uint64_t header_length_offset{24U};
inline constexpr std::uint64_t covered_log_offset{56U};
inline constexpr std::uint64_t sequence_exhausted_offset{64U};
inline constexpr std::uint64_t active_order_count_offset{65U};
inline constexpr std::uint64_t engine_capacity_offset{73U};
inline constexpr std::uint64_t catalog_count_offset{81U};
inline constexpr std::uint64_t catalog_length_offset{85U};
inline constexpr std::uint64_t instrument_count_offset{93U};
inline constexpr std::uint64_t instruments_length_offset{97U};
inline constexpr std::uint64_t configuration_digest_offset{105U};
inline constexpr std::uint64_t state_digest_offset{137U};
inline constexpr std::uint64_t catalog_offset{snapshot_fixed_bytes};

inline constexpr std::uint64_t catalog_maximum_quantity_offset{4U};
inline constexpr std::uint64_t catalog_tick_increment_offset{12U};
inline constexpr std::uint64_t catalog_capacity_offset{20U};

inline constexpr std::uint64_t instrument_id_offset{8U};
inline constexpr std::uint64_t instrument_active_order_count_offset{12U};
inline constexpr std::uint64_t instrument_levels_offset{snapshot_instrument_fixed_bytes};

inline constexpr std::uint64_t level_price_offset{8U};
inline constexpr std::uint64_t level_aggregate_offset{16U};
inline constexpr std::uint64_t level_order_count_offset{24U};
inline constexpr std::uint64_t level_orders_offset{snapshot_level_fixed_bytes};

inline constexpr std::uint64_t order_client_id_offset{8U};
inline constexpr std::uint64_t order_instrument_id_offset{12U};
inline constexpr std::uint64_t order_side_offset{16U};
inline constexpr std::uint64_t order_price_offset{17U};
inline constexpr std::uint64_t order_remaining_quantity_offset{25U};
inline constexpr std::uint64_t order_priority_sequence_offset{33U};

[[nodiscard]] SnapshotError snapshot_error(SnapshotErrorCategory category,
                                           std::uint64_t offset) noexcept {
  return {
      .category = category,
      .byte_offset = offset,
  };
}

[[nodiscard]] SnapshotError validate_persisted_catalog(
    std::span<const PersistedInstrumentConfig> catalog) noexcept {
  if (catalog.empty()) {
    return snapshot_error(SnapshotErrorCategory::invalid_catalog, catalog_count_offset);
  }

  domain::InstrumentId previous{};
  std::uint64_t entry_offset = catalog_offset;
  for (const auto& instrument : catalog) {
    if (instrument.instrument_id.value() == 0U || instrument.instrument_id <= previous) {
      return snapshot_error(SnapshotErrorCategory::invalid_catalog, entry_offset);
    }
    if (instrument.max_order_quantity == 0U) {
      return snapshot_error(SnapshotErrorCategory::invalid_catalog,
                            entry_offset + catalog_maximum_quantity_offset);
    }
    if (instrument.tick_increment.value() <= 0) {
      return snapshot_error(SnapshotErrorCategory::invalid_catalog,
                            entry_offset + catalog_tick_increment_offset);
    }
    previous = instrument.instrument_id;
    entry_offset += snapshot_catalog_entry_bytes;
  }
  return {};
}

[[nodiscard]] std::optional<std::vector<InstrumentConfig>> host_catalog(
    std::span<const PersistedInstrumentConfig> catalog) {
  std::vector<InstrumentConfig> result;
  result.reserve(catalog.size());
  for (const auto& value : catalog) {
    const auto maximum_active = host_capacity(value.max_active_orders);
    if (!maximum_active.has_value()) {
      return std::nullopt;
    }
    result.push_back({
        .instrument_id = value.instrument_id,
        .matching =
            {
                .max_order_quantity = domain::Quantity{value.max_order_quantity},
                .tick_increment = value.tick_increment,
                .max_active_orders = *maximum_active,
            },
    });
  }
  return result;
}

[[nodiscard]] bool capacity_allows(std::uint64_t count, std::uint64_t capacity) noexcept {
  return capacity == std::numeric_limits<std::uint64_t>::max() || count <= capacity;
}

[[nodiscard]] SnapshotError validate_level(
    const PriceLevelSnapshot& level, domain::InstrumentId instrument_id, domain::Side side,
    const PersistedInstrumentConfig& config, domain::Sequence covered_sequence,
    std::unordered_set<domain::OrderId, domain::StrongValueHash<domain::OrderId>>& order_ids,
    std::unordered_set<domain::Sequence, domain::StrongValueHash<domain::Sequence>>& priorities,
    std::uint64_t& order_count, std::uint64_t level_offset) {
  if (level.price.value() <= 0 || level.price.value() % config.tick_increment.value() != 0) {
    return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema,
                          level_offset + level_price_offset);
  }
  if (level.aggregate_quantity.value() == 0U) {
    return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema,
                          level_offset + level_aggregate_offset);
  }
  if (level.orders.empty()) {
    return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema,
                          level_offset + level_order_count_offset);
  }

  std::uint64_t aggregate = 0U;
  domain::Sequence previous_priority{};
  std::uint64_t order_offset = level_offset + level_orders_offset;
  for (const auto& order : level.orders) {
    if (order.order_id.value() == 0U) {
      return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema, order_offset);
    }
    if (order.client_id.value() == 0U) {
      return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema,
                            order_offset + order_client_id_offset);
    }
    if (order.instrument_id != instrument_id) {
      return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema,
                            order_offset + order_instrument_id_offset);
    }
    if (order.side != side) {
      return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema,
                            order_offset + order_side_offset);
    }
    if (order.price != level.price) {
      return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema,
                            order_offset + order_price_offset);
    }
    if (order.remaining_quantity.value() == 0U ||
        order.remaining_quantity.value() > config.max_order_quantity) {
      return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema,
                            order_offset + order_remaining_quantity_offset);
    }
    if (order.priority_sequence.value() == 0U || order.priority_sequence > covered_sequence ||
        order.priority_sequence <= previous_priority) {
      return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema,
                            order_offset + order_priority_sequence_offset);
    }
    if (!order_ids.insert(order.order_id).second) {
      return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema, order_offset);
    }
    if (!priorities.insert(order.priority_sequence).second) {
      return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema,
                            order_offset + order_priority_sequence_offset);
    }

    if (!checked_add_u64(aggregate, order.remaining_quantity.value(), aggregate)) {
      return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema,
                            order_offset + order_remaining_quantity_offset);
    }
    if (!checked_add_u64(order_count, 1U, order_count)) {
      return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema,
                            level_offset + level_order_count_offset);
    }
    previous_priority = order.priority_sequence;
    order_offset += snapshot_order_bytes;
  }

  if (aggregate != level.aggregate_quantity.value()) {
    return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema,
                          level_offset + level_aggregate_offset);
  }
  return {};
}

[[nodiscard]] SnapshotError validate_side(
    std::span<const PriceLevelSnapshot> levels, domain::InstrumentId instrument_id,
    domain::Side side, const PersistedInstrumentConfig& config, domain::Sequence covered_sequence,
    std::unordered_set<domain::OrderId, domain::StrongValueHash<domain::OrderId>>& order_ids,
    std::unordered_set<domain::Sequence, domain::StrongValueHash<domain::Sequence>>& priorities,
    std::uint64_t& order_count, std::uint64_t& level_offset) {
  domain::PriceTicks previous{};
  bool first = true;
  for (const auto& level : levels) {
    if (!first) {
      const bool ordered =
          side == domain::Side::buy ? level.price < previous : level.price > previous;
      if (!ordered) {
        return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema,
                              level_offset + level_price_offset);
      }
    }

    if (const auto error = validate_level(level, instrument_id, side, config, covered_sequence,
                                          order_ids, priorities, order_count, level_offset);
        error) {
      return error;
    }
    std::uint64_t order_bytes = 0U;
    std::uint64_t level_bytes = 0U;
    std::uint64_t order_size = 0U;
    if (!size_to_u64(level.orders.size(), order_size) ||
        order_size > std::numeric_limits<std::uint64_t>::max() / snapshot_order_bytes) {
      return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema, level_offset);
    }
    order_bytes = order_size * snapshot_order_bytes;
    if (!checked_add_u64(snapshot_level_fixed_bytes, order_bytes, level_bytes) ||
        !checked_add_u64(level_offset, level_bytes, level_offset)) {
      return snapshot_error(SnapshotErrorCategory::invalid_snapshot_schema, level_offset);
    }
    previous = level.price;
    first = false;
  }
  return {};
}

[[nodiscard]] SnapshotCodecResult<EngineSnapshot> validate_and_convert(
    const SnapshotFile& snapshot) {
  if (snapshot.format_version != snapshot_format_version) {
    return failure<EngineSnapshot>(SnapshotErrorCategory::unsupported_format_version,
                                   format_version_offset);
  }
  if (snapshot.semantics_version != atlaslob_semantics_version) {
    return failure<EngineSnapshot>(SnapshotErrorCategory::semantic_version_mismatch,
                                   semantics_version_offset);
  }
  if (const auto error = validate_persisted_catalog(snapshot.catalog); error) {
    return failure<EngineSnapshot>(error.category, error.byte_offset);
  }
  if (snapshot.configuration_digest !=
      configuration_digest(snapshot.catalog, snapshot.engine_config, snapshot.semantics_version)) {
    return failure<EngineSnapshot>(SnapshotErrorCategory::configuration_digest_mismatch,
                                   configuration_digest_offset);
  }
  if ((snapshot.sequence_exhausted &&
       snapshot.covered_sequence.value() != std::numeric_limits<std::uint64_t>::max()) ||
      (!snapshot.sequence_exhausted &&
       snapshot.covered_sequence.value() == std::numeric_limits<std::uint64_t>::max())) {
    return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_snapshot_schema,
                                   sequence_exhausted_offset);
  }
  std::size_t catalog_bytes = 0U;
  std::size_t initial_log_boundary = 0U;
  std::uint64_t canonical_boundary = 0U;
  if (!checked_multiply(snapshot.catalog.size(), command_log_catalog_entry_bytes, catalog_bytes) ||
      !checked_add(command_log_header_fixed_bytes, catalog_bytes, initial_log_boundary) ||
      !size_to_u64(initial_log_boundary, canonical_boundary)) {
    return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_catalog, catalog_count_offset);
  }
  if (snapshot.covered_sequence.value() == 0U && snapshot.active_order_count != 0U) {
    return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_snapshot_schema,
                                   active_order_count_offset);
  }
  if ((snapshot.covered_sequence.value() == 0U &&
       snapshot.covered_log_byte_offset != canonical_boundary) ||
      (snapshot.covered_sequence.value() != 0U &&
       snapshot.covered_log_byte_offset <= canonical_boundary)) {
    return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_snapshot_schema,
                                   covered_log_offset);
  }

  const auto engine_capacity = host_capacity(snapshot.engine_config.max_total_active_orders);
  if (!engine_capacity.has_value()) {
    return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_catalog, engine_capacity_offset);
  }
  std::uint64_t catalog_entry_offset = catalog_offset;
  for (const auto& config : snapshot.catalog) {
    if (!host_capacity(config.max_active_orders).has_value()) {
      return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_catalog,
                                     catalog_entry_offset + catalog_capacity_offset);
    }
    catalog_entry_offset += snapshot_catalog_entry_bytes;
  }
  auto catalog = host_catalog(snapshot.catalog);
  if (!catalog.has_value()) {
    return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_catalog, catalog_offset);
  }
  if (snapshot.instruments.size() != snapshot.catalog.size()) {
    return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_snapshot_schema,
                                   instrument_count_offset);
  }

  std::unordered_set<domain::OrderId, domain::StrongValueHash<domain::OrderId>> order_ids;
  std::unordered_set<domain::Sequence, domain::StrongValueHash<domain::Sequence>> priorities;

  std::uint64_t catalog_size = 0U;
  std::uint64_t encoded_catalog_bytes = 0U;
  std::uint64_t instrument_offset = 0U;
  if (!size_to_u64(snapshot.catalog.size(), catalog_size) ||
      catalog_size > std::numeric_limits<std::uint64_t>::max() / snapshot_catalog_entry_bytes) {
    return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_catalog, catalog_count_offset);
  }
  encoded_catalog_bytes = catalog_size * snapshot_catalog_entry_bytes;
  if (!checked_add_u64(snapshot_fixed_bytes, encoded_catalog_bytes, instrument_offset)) {
    return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_catalog, catalog_count_offset);
  }

  std::uint64_t total_orders = 0U;
  for (std::size_t index = 0U; index < snapshot.instruments.size(); ++index) {
    const auto& instrument = snapshot.instruments[index];
    const auto& config = snapshot.catalog[index];
    if (instrument.instrument_id != config.instrument_id) {
      return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_snapshot_schema,
                                     instrument_offset + instrument_id_offset);
    }
    if (!capacity_allows(instrument.active_order_count, config.max_active_orders)) {
      return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_snapshot_schema,
                                     instrument_offset + instrument_active_order_count_offset);
    }

    std::uint64_t instrument_orders = 0U;
    std::uint64_t level_offset = 0U;
    if (!checked_add_u64(instrument_offset, instrument_levels_offset, level_offset)) {
      return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_snapshot_schema,
                                     instrument_offset);
    }
    if (const auto error = validate_side(instrument.bids, instrument.instrument_id,
                                         domain::Side::buy, config, snapshot.covered_sequence,
                                         order_ids, priorities, instrument_orders, level_offset);
        error) {
      return failure<EngineSnapshot>(error.category, error.byte_offset);
    }
    const auto ask_levels_offset = level_offset;
    if (const auto error = validate_side(instrument.asks, instrument.instrument_id,
                                         domain::Side::sell, config, snapshot.covered_sequence,
                                         order_ids, priorities, instrument_orders, level_offset);
        error) {
      return failure<EngineSnapshot>(error.category, error.byte_offset);
    }
    if (!instrument.bids.empty() && !instrument.asks.empty() &&
        instrument.bids.front().price >= instrument.asks.front().price) {
      return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_snapshot_schema,
                                     ask_levels_offset + level_price_offset);
    }
    if (instrument_orders != instrument.active_order_count) {
      return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_snapshot_schema,
                                     instrument_offset + instrument_active_order_count_offset);
    }
    if (!checked_add_u64(total_orders, instrument_orders, total_orders)) {
      return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_snapshot_schema,
                                     instrument_offset + instrument_active_order_count_offset);
    }
    instrument_offset = level_offset;
  }

  if (total_orders != snapshot.active_order_count ||
      !capacity_allows(total_orders, snapshot.engine_config.max_total_active_orders)) {
    return failure<EngineSnapshot>(SnapshotErrorCategory::invalid_snapshot_schema,
                                   active_order_count_offset);
  }

  EngineSnapshot engine_snapshot{
      .semantics_version = snapshot.semantics_version,
      .engine_config =
          {
              .max_total_active_orders = *engine_capacity,
          },
      .catalog = std::move(*catalog),
      .last_sequence = snapshot.covered_sequence,
      .sequence_exhausted = snapshot.sequence_exhausted,
      .active_order_count = snapshot.active_order_count,
      .instruments = snapshot.instruments,
  };
  if (atlaslob::state_digest(engine_snapshot) != snapshot.state_digest) {
    return failure<EngineSnapshot>(SnapshotErrorCategory::state_digest_mismatch,
                                   state_digest_offset);
  }
  return success<EngineSnapshot>(std::move(engine_snapshot));
}

[[nodiscard]] bool checked_encoded_size(const SnapshotFile& snapshot, CodecLimits limits,
                                        std::size_t& header_length, std::size_t& instruments_length,
                                        std::size_t& total_length,
                                        SnapshotErrorCategory& category) noexcept {
  if (!limits.valid()) {
    category = SnapshotErrorCategory::excessive_length;
    return false;
  }
  if (snapshot.catalog.size() > std::numeric_limits<std::uint32_t>::max() ||
      snapshot.instruments.size() > std::numeric_limits<std::uint32_t>::max()) {
    category = SnapshotErrorCategory::excessive_length;
    return false;
  }

  std::size_t catalog_bytes = 0U;
  if (!checked_multiply(snapshot.catalog.size(), snapshot_catalog_entry_bytes, catalog_bytes) ||
      !checked_add(snapshot_fixed_bytes, catalog_bytes, header_length)) {
    category = SnapshotErrorCategory::excessive_length;
    return false;
  }

  instruments_length = 0U;
  for (const auto& instrument : snapshot.instruments) {
    std::size_t instrument_length = snapshot_instrument_fixed_bytes;
    const auto accumulate_levels = [&instrument_length](const auto& levels) noexcept {
      for (const auto& level : levels) {
        std::size_t order_bytes = 0U;
        std::size_t level_length = 0U;
        if (!checked_multiply(level.orders.size(), snapshot_order_bytes, order_bytes) ||
            !checked_add(snapshot_level_fixed_bytes, order_bytes, level_length) ||
            !checked_add(instrument_length, level_length, instrument_length)) {
          return false;
        }
      }
      return true;
    };
    if (!accumulate_levels(instrument.bids) || !accumulate_levels(instrument.asks) ||
        !checked_add(instruments_length, instrument_length, instruments_length)) {
      category = SnapshotErrorCategory::excessive_length;
      return false;
    }
  }

  if (!checked_add(header_length, instruments_length, total_length) ||
      !checked_add(total_length, sizeof(std::uint32_t), total_length)) {
    category = SnapshotErrorCategory::excessive_length;
    return false;
  }

  std::uint64_t canonical_total = 0U;
  if (!size_to_u64(total_length, canonical_total) || canonical_total > limits.max_snapshot_bytes) {
    category = SnapshotErrorCategory::excessive_length;
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

void encode_order(BinaryEncoder& encoder, const OrderSnapshot& order) {
  encoder.u64(order.order_id.value());
  encoder.u32(order.client_id.value());
  encoder.u32(order.instrument_id.value());
  encoder.u8(static_cast<std::uint8_t>(order.side));
  encoder.i64(order.price.value());
  encoder.u64(order.remaining_quantity.value());
  encoder.u64(order.priority_sequence.value());
}

[[nodiscard]] std::size_t encoded_level_size(const PriceLevelSnapshot& level) noexcept {
  return snapshot_level_fixed_bytes + level.orders.size() * snapshot_order_bytes;
}

void encode_level(BinaryEncoder& encoder, const PriceLevelSnapshot& level) {
  encoder.u64(static_cast<std::uint64_t>(encoded_level_size(level)));
  encoder.i64(level.price.value());
  encoder.u64(level.aggregate_quantity.value());
  encoder.u64(static_cast<std::uint64_t>(level.orders.size()));
  for (const auto& order : level.orders) {
    encode_order(encoder, order);
  }
}

[[nodiscard]] std::size_t encoded_instrument_size(const InstrumentSnapshot& instrument) noexcept {
  std::size_t result = snapshot_instrument_fixed_bytes;
  for (const auto& level : instrument.bids) {
    result += encoded_level_size(level);
  }
  for (const auto& level : instrument.asks) {
    result += encoded_level_size(level);
  }
  return result;
}

void encode_instrument(BinaryEncoder& encoder, const InstrumentSnapshot& instrument) {
  encoder.u64(static_cast<std::uint64_t>(encoded_instrument_size(instrument)));
  encoder.u32(instrument.instrument_id.value());
  encoder.u64(instrument.active_order_count);
  encoder.u64(static_cast<std::uint64_t>(instrument.bids.size()));
  encoder.u64(static_cast<std::uint64_t>(instrument.asks.size()));
  for (const auto& level : instrument.bids) {
    encode_level(encoder, level);
  }
  for (const auto& level : instrument.asks) {
    encode_level(encoder, level);
  }
}

[[nodiscard]] bool decode_catalog_entry(BinaryDecoder& decoder,
                                        PersistedInstrumentConfig& instrument) noexcept {
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

[[nodiscard]] SnapshotCodecResult<PriceLevelSnapshot> decode_level(BinaryDecoder& parent,
                                                                   std::size_t absolute_offset) {
  std::uint64_t encoded_length = 0U;
  if (!parent.u64(encoded_length) || encoded_length < snapshot_level_fixed_bytes) {
    return failure<PriceLevelSnapshot>(SnapshotErrorCategory::invalid_length, absolute_offset);
  }
  const auto length = host_count(encoded_length);
  if (!length.has_value() || *length - sizeof(std::uint64_t) > parent.remaining()) {
    return failure<PriceLevelSnapshot>(SnapshotErrorCategory::invalid_length, absolute_offset);
  }

  std::span<const std::uint8_t> body;
  if (!parent.bytes(*length - sizeof(std::uint64_t), body)) {
    return failure<PriceLevelSnapshot>(SnapshotErrorCategory::invalid_length, absolute_offset);
  }
  BinaryDecoder decoder{body};
  std::int64_t price = 0;
  std::uint64_t aggregate = 0U;
  std::uint64_t order_count = 0U;
  if (!decoder.i64(price) || !decoder.u64(aggregate) || !decoder.u64(order_count)) {
    return failure<PriceLevelSnapshot>(SnapshotErrorCategory::invalid_length, absolute_offset);
  }

  const auto count = host_count(order_count);
  std::size_t expected_order_bytes = 0U;
  std::size_t expected_length = 0U;
  if (!count.has_value() || !checked_multiply(*count, snapshot_order_bytes, expected_order_bytes) ||
      !checked_add(snapshot_level_fixed_bytes, expected_order_bytes, expected_length) ||
      expected_length != *length) {
    return failure<PriceLevelSnapshot>(SnapshotErrorCategory::invalid_length, absolute_offset);
  }

  PriceLevelSnapshot level{
      .price = domain::PriceTicks{price},
      .aggregate_quantity = domain::Quantity{aggregate},
      .orders = {},
  };
  level.orders.reserve(*count);
  for (std::size_t index = 0U; index < *count; ++index) {
    std::uint64_t order_id = 0U;
    std::uint32_t client_id = 0U;
    std::uint32_t instrument_id = 0U;
    std::uint8_t side = 0U;
    std::int64_t order_price = 0;
    std::uint64_t remaining = 0U;
    std::uint64_t priority = 0U;
    if (!decoder.u64(order_id) || !decoder.u32(client_id) || !decoder.u32(instrument_id) ||
        !decoder.u8(side) || !decoder.i64(order_price) || !decoder.u64(remaining) ||
        !decoder.u64(priority)) {
      return failure<PriceLevelSnapshot>(SnapshotErrorCategory::invalid_length,
                                         absolute_offset + decoder.position());
    }
    level.orders.push_back({
        .order_id = domain::OrderId{order_id},
        .client_id = domain::ClientId{client_id},
        .instrument_id = domain::InstrumentId{instrument_id},
        .side = static_cast<domain::Side>(side),
        .price = domain::PriceTicks{order_price},
        .remaining_quantity = domain::Quantity{remaining},
        .priority_sequence = domain::Sequence{priority},
    });
  }
  if (decoder.remaining() != 0U) {
    return failure<PriceLevelSnapshot>(SnapshotErrorCategory::invalid_length,
                                       absolute_offset + decoder.position());
  }
  return success<PriceLevelSnapshot>(std::move(level));
}

[[nodiscard]] SnapshotCodecResult<InstrumentSnapshot> decode_instrument(
    BinaryDecoder& parent, std::size_t absolute_offset) {
  std::uint64_t encoded_length = 0U;
  if (!parent.u64(encoded_length) || encoded_length < snapshot_instrument_fixed_bytes) {
    return failure<InstrumentSnapshot>(SnapshotErrorCategory::invalid_length, absolute_offset);
  }
  const auto length = host_count(encoded_length);
  if (!length.has_value() || *length - sizeof(std::uint64_t) > parent.remaining()) {
    return failure<InstrumentSnapshot>(SnapshotErrorCategory::invalid_length, absolute_offset);
  }

  std::span<const std::uint8_t> body;
  if (!parent.bytes(*length - sizeof(std::uint64_t), body)) {
    return failure<InstrumentSnapshot>(SnapshotErrorCategory::invalid_length, absolute_offset);
  }
  BinaryDecoder decoder{body};
  std::uint32_t instrument_id = 0U;
  std::uint64_t active_order_count = 0U;
  std::uint64_t bid_count = 0U;
  std::uint64_t ask_count = 0U;
  if (!decoder.u32(instrument_id) || !decoder.u64(active_order_count) || !decoder.u64(bid_count) ||
      !decoder.u64(ask_count)) {
    return failure<InstrumentSnapshot>(SnapshotErrorCategory::invalid_length, absolute_offset);
  }
  const auto bids = host_count(bid_count);
  const auto asks = host_count(ask_count);
  std::size_t level_count = 0U;
  std::size_t minimum_level_bytes = 0U;
  if (!bids.has_value() || !asks.has_value() || !checked_add(*bids, *asks, level_count) ||
      !checked_multiply(level_count, snapshot_level_fixed_bytes, minimum_level_bytes) ||
      minimum_level_bytes > decoder.remaining()) {
    return failure<InstrumentSnapshot>(SnapshotErrorCategory::invalid_length, absolute_offset);
  }

  InstrumentSnapshot instrument{
      .instrument_id = domain::InstrumentId{instrument_id},
      .active_order_count = active_order_count,
      .bids = {},
      .asks = {},
  };
  instrument.bids.reserve(*bids);
  instrument.asks.reserve(*asks);
  for (std::size_t index = 0U; index < *bids; ++index) {
    const auto level_offset = absolute_offset + sizeof(std::uint64_t) + decoder.position();
    auto level = decode_level(decoder, level_offset);
    if (!level) {
      return failure<InstrumentSnapshot>(level.error.category,
                                         static_cast<std::size_t>(level.error.byte_offset));
    }
    instrument.bids.push_back(std::move(*level.value));
  }
  for (std::size_t index = 0U; index < *asks; ++index) {
    const auto level_offset = absolute_offset + sizeof(std::uint64_t) + decoder.position();
    auto level = decode_level(decoder, level_offset);
    if (!level) {
      return failure<InstrumentSnapshot>(level.error.category,
                                         static_cast<std::size_t>(level.error.byte_offset));
    }
    instrument.asks.push_back(std::move(*level.value));
  }
  if (decoder.remaining() != 0U) {
    return failure<InstrumentSnapshot>(SnapshotErrorCategory::invalid_length,
                                       absolute_offset + *length - decoder.remaining());
  }
  return success<InstrumentSnapshot>(std::move(instrument));
}

}  // namespace

SnapshotCodecResult<SnapshotFile> make_snapshot_file(const EngineSnapshot& snapshot, LogId log_id,
                                                     std::uint64_t covered_log_byte_offset) {
  std::vector<PersistedInstrumentConfig> catalog;
  catalog.reserve(snapshot.catalog.size());
  for (const auto& value : snapshot.catalog) {
    catalog.push_back({
        .instrument_id = value.instrument_id,
        .max_order_quantity = value.matching.max_order_quantity.value(),
        .tick_increment = value.matching.tick_increment,
        .max_active_orders = canonical_capacity(value.matching.max_active_orders),
    });
  }

  SnapshotFile file{
      .format_version = snapshot_format_version,
      .semantics_version = snapshot.semantics_version,
      .log_id = log_id,
      .covered_sequence = snapshot.last_sequence,
      .covered_log_byte_offset = covered_log_byte_offset,
      .sequence_exhausted = snapshot.sequence_exhausted,
      .engine_config =
          {
              .max_total_active_orders =
                  canonical_capacity(snapshot.engine_config.max_total_active_orders),
          },
      .catalog = std::move(catalog),
      .active_order_count = snapshot.active_order_count,
      .instruments = snapshot.instruments,
      .state_digest = atlaslob::state_digest(snapshot),
  };
  file.configuration_digest =
      configuration_digest(file.catalog, file.engine_config, file.semantics_version);

  const auto validated = validate_and_convert(file);
  if (!validated) {
    return failure<SnapshotFile>(validated.error.category, validated.error.byte_offset);
  }
  return success<SnapshotFile>(std::move(file));
}

SnapshotCodecResult<EngineSnapshot> host_engine_snapshot(const SnapshotFile& snapshot) {
  return validate_and_convert(snapshot);
}

SnapshotCodecResult<std::size_t> inspect_snapshot_length(std::span<const std::uint8_t> fixed_prefix,
                                                         CodecLimits limits) noexcept {
  if (fixed_prefix.size() < snapshot_fixed_prefix_bytes) {
    return failure<std::size_t>(SnapshotErrorCategory::invalid_length, fixed_prefix.size());
  }
  BinaryDecoder decoder{fixed_prefix};
  std::span<const std::uint8_t> ignored;
  std::uint64_t length = 0U;
  if (!decoder.bytes(16U, ignored) || !decoder.u64(length) ||
      length < snapshot_fixed_bytes + sizeof(std::uint32_t)) {
    return failure<std::size_t>(SnapshotErrorCategory::invalid_length, 16U);
  }
  if (!limits.valid() || length > limits.max_snapshot_bytes) {
    return failure<std::size_t>(SnapshotErrorCategory::excessive_length, 16U);
  }
  const auto host_length = host_count(length);
  if (!host_length.has_value()) {
    return failure<std::size_t>(SnapshotErrorCategory::excessive_length, 16U);
  }
  return success<std::size_t>(*host_length);
}

SnapshotCodecResult<std::vector<std::uint8_t>> encode_snapshot(const SnapshotFile& snapshot,
                                                               CodecLimits limits) {
  const auto validated = validate_and_convert(snapshot);
  if (!validated) {
    return failure<std::vector<std::uint8_t>>(validated.error.category,
                                              validated.error.byte_offset);
  }

  std::size_t header_length = 0U;
  std::size_t instruments_length = 0U;
  std::size_t total_length = 0U;
  SnapshotErrorCategory category = SnapshotErrorCategory::none;
  if (!checked_encoded_size(snapshot, limits, header_length, instruments_length, total_length,
                            category)) {
    return failure<std::vector<std::uint8_t>>(category);
  }

  BinaryEncoder encoder{total_length};
  encoder.bytes(snapshot_magic);
  encoder.u16(snapshot.format_version);
  encoder.u16(snapshot.semantics_version);
  encoder.u32(snapshot_byte_order_marker);
  encoder.u64(static_cast<std::uint64_t>(total_length));
  encoder.u64(static_cast<std::uint64_t>(header_length));
  encoder.bytes(snapshot.log_id.bytes);
  encoder.u64(snapshot.covered_sequence.value());
  encoder.u64(snapshot.covered_log_byte_offset);
  encoder.u8(snapshot.sequence_exhausted ? 1U : 0U);
  encoder.u64(snapshot.active_order_count);
  encoder.u64(snapshot.engine_config.max_total_active_orders);
  encoder.u32(static_cast<std::uint32_t>(snapshot.catalog.size()));
  encoder.u64(static_cast<std::uint64_t>(snapshot.catalog.size() * snapshot_catalog_entry_bytes));
  encoder.u32(static_cast<std::uint32_t>(snapshot.instruments.size()));
  encoder.u64(static_cast<std::uint64_t>(instruments_length));
  encoder.bytes(snapshot.configuration_digest.bytes);
  encoder.bytes(snapshot.state_digest.bytes);
  for (const auto& instrument : snapshot.catalog) {
    encode_catalog_entry(encoder, instrument);
  }
  for (const auto& instrument : snapshot.instruments) {
    encode_instrument(encoder, instrument);
  }
  encoder.u32(crc32c(encoder.view()));

  if (encoder.size() != total_length) {
    return failure<std::vector<std::uint8_t>>(SnapshotErrorCategory::invalid_length);
  }
  return success<std::vector<std::uint8_t>>(std::move(encoder).take());
}

SnapshotCodecResult<SnapshotFile> decode_snapshot(std::span<const std::uint8_t> bytes,
                                                  CodecLimits limits) {
  if (bytes.size() < snapshot_fixed_prefix_bytes) {
    return failure<SnapshotFile>(SnapshotErrorCategory::invalid_length, bytes.size());
  }
  const auto inspected = inspect_snapshot_length(bytes.first(snapshot_fixed_prefix_bytes), limits);
  if (!inspected) {
    return failure<SnapshotFile>(inspected.error.category, inspected.error.byte_offset);
  }
  if (*inspected.value != bytes.size()) {
    return failure<SnapshotFile>(SnapshotErrorCategory::invalid_length, 16U);
  }

  BinaryDecoder checksum_decoder{bytes.last(sizeof(std::uint32_t))};
  std::uint32_t encoded_checksum = 0U;
  if (!checksum_decoder.u32(encoded_checksum) ||
      crc32c(bytes.first(bytes.size() - sizeof(std::uint32_t))) != encoded_checksum) {
    return failure<SnapshotFile>(SnapshotErrorCategory::bad_checksum,
                                 bytes.size() - sizeof(std::uint32_t));
  }

  BinaryDecoder decoder{bytes.first(bytes.size() - sizeof(std::uint32_t))};
  std::span<const std::uint8_t> magic;
  std::uint16_t format_version = 0U;
  std::uint16_t semantics_version = 0U;
  std::uint32_t marker = 0U;
  std::uint64_t total_length = 0U;
  std::uint64_t header_length = 0U;
  std::span<const std::uint8_t> log_id_bytes;
  std::uint64_t covered_sequence = 0U;
  std::uint64_t decoded_covered_log_offset = 0U;
  std::uint8_t exhausted = 0U;
  std::uint64_t active_order_count = 0U;
  std::uint64_t maximum_total_active = 0U;
  std::uint32_t catalog_count = 0U;
  std::uint64_t catalog_length = 0U;
  std::uint32_t instrument_count = 0U;
  std::uint64_t instruments_length = 0U;
  std::span<const std::uint8_t> configuration_digest_bytes;
  std::span<const std::uint8_t> state_digest_bytes;
  if (!decoder.bytes(snapshot_magic.size(), magic) || !decoder.u16(format_version) ||
      !decoder.u16(semantics_version) || !decoder.u32(marker) || !decoder.u64(total_length) ||
      !decoder.u64(header_length) || !decoder.bytes(16U, log_id_bytes) ||
      !decoder.u64(covered_sequence) || !decoder.u64(decoded_covered_log_offset) ||
      !decoder.u8(exhausted) || !decoder.u64(active_order_count) ||
      !decoder.u64(maximum_total_active) || !decoder.u32(catalog_count) ||
      !decoder.u64(catalog_length) || !decoder.u32(instrument_count) ||
      !decoder.u64(instruments_length) || !decoder.bytes(32U, configuration_digest_bytes) ||
      !decoder.bytes(32U, state_digest_bytes)) {
    return failure<SnapshotFile>(SnapshotErrorCategory::invalid_length, decoder.position());
  }

  if (!std::ranges::equal(magic, snapshot_magic)) {
    return failure<SnapshotFile>(SnapshotErrorCategory::unsupported_format_version, 0U);
  }
  if (format_version != snapshot_format_version) {
    return failure<SnapshotFile>(SnapshotErrorCategory::unsupported_format_version,
                                 format_version_offset);
  }
  if (marker != snapshot_byte_order_marker) {
    return failure<SnapshotFile>(SnapshotErrorCategory::unsupported_format_version,
                                 byte_order_marker_offset);
  }
  if (semantics_version != atlaslob_semantics_version) {
    return failure<SnapshotFile>(SnapshotErrorCategory::semantic_version_mismatch,
                                 semantics_version_offset);
  }
  if (total_length != bytes.size()) {
    return failure<SnapshotFile>(SnapshotErrorCategory::invalid_snapshot_schema,
                                 snapshot_length_offset);
  }
  if (exhausted > 1U) {
    return failure<SnapshotFile>(SnapshotErrorCategory::invalid_snapshot_schema,
                                 sequence_exhausted_offset);
  }

  std::size_t expected_catalog_length = 0U;
  std::size_t expected_header_length = 0U;
  if (!checked_multiply(static_cast<std::size_t>(catalog_count), snapshot_catalog_entry_bytes,
                        expected_catalog_length)) {
    return failure<SnapshotFile>(SnapshotErrorCategory::invalid_length, catalog_count_offset);
  }
  if (catalog_length != expected_catalog_length) {
    return failure<SnapshotFile>(SnapshotErrorCategory::invalid_length, catalog_length_offset);
  }
  if (!checked_add(snapshot_fixed_bytes, expected_catalog_length, expected_header_length) ||
      header_length != expected_header_length ||
      header_length > bytes.size() - sizeof(std::uint32_t)) {
    return failure<SnapshotFile>(SnapshotErrorCategory::invalid_length, header_length_offset);
  }
  if (instruments_length !=
      bytes.size() - static_cast<std::size_t>(header_length) - sizeof(std::uint32_t)) {
    return failure<SnapshotFile>(SnapshotErrorCategory::invalid_length, instruments_length_offset);
  }

  SnapshotFile snapshot{
      .format_version = format_version,
      .semantics_version = semantics_version,
      .covered_sequence = domain::Sequence{covered_sequence},
      .covered_log_byte_offset = decoded_covered_log_offset,
      .sequence_exhausted = exhausted != 0U,
      .engine_config =
          {
              .max_total_active_orders = maximum_total_active,
          },
      .catalog = {},
      .active_order_count = active_order_count,
      .instruments = {},
  };
  std::ranges::copy(log_id_bytes, snapshot.log_id.bytes.begin());
  std::ranges::copy(configuration_digest_bytes, snapshot.configuration_digest.bytes.begin());
  std::ranges::copy(state_digest_bytes, snapshot.state_digest.bytes.begin());

  snapshot.catalog.reserve(catalog_count);
  for (std::uint32_t index = 0U; index < catalog_count; ++index) {
    PersistedInstrumentConfig instrument;
    if (!decode_catalog_entry(decoder, instrument)) {
      return failure<SnapshotFile>(SnapshotErrorCategory::invalid_length, decoder.position());
    }
    snapshot.catalog.push_back(instrument);
  }
  if (decoder.position() != header_length) {
    return failure<SnapshotFile>(SnapshotErrorCategory::invalid_length, decoder.position());
  }
  if (const auto error = validate_persisted_catalog(snapshot.catalog); error) {
    return failure<SnapshotFile>(error.category, error.byte_offset);
  }
  if (snapshot.configuration_digest !=
      configuration_digest(snapshot.catalog, snapshot.engine_config, snapshot.semantics_version)) {
    return failure<SnapshotFile>(SnapshotErrorCategory::configuration_digest_mismatch,
                                 configuration_digest_offset);
  }

  const std::size_t instrument_section_start = decoder.position();
  if (instrument_count > decoder.remaining() / snapshot_instrument_fixed_bytes) {
    return failure<SnapshotFile>(SnapshotErrorCategory::invalid_length, instrument_count_offset);
  }
  snapshot.instruments.reserve(instrument_count);
  for (std::uint32_t index = 0U; index < instrument_count; ++index) {
    const auto instrument_offset = decoder.position();
    auto instrument = decode_instrument(decoder, instrument_offset);
    if (!instrument) {
      return failure<SnapshotFile>(instrument.error.category, instrument.error.byte_offset);
    }
    snapshot.instruments.push_back(std::move(*instrument.value));
  }
  if (decoder.remaining() != 0U ||
      decoder.position() - instrument_section_start != instruments_length) {
    return failure<SnapshotFile>(SnapshotErrorCategory::invalid_length, decoder.position());
  }

  const auto validated = validate_and_convert(snapshot);
  if (!validated) {
    return failure<SnapshotFile>(validated.error.category, validated.error.byte_offset);
  }
  return success<SnapshotFile>(std::move(snapshot));
}

}  // namespace atlaslob::persistence::detail
