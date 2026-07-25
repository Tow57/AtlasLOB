#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "atlaslob/digest.hpp"
#include "atlaslob/multi_instrument_engine.hpp"
#include "sha256.hpp"

#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
#include "canonical_digest.hpp"
#endif

namespace atlaslob {
namespace {

constexpr std::array<std::uint8_t, 8U> state_prefix{'A', 'T', 'L', 'S', 'S', 'T', '0', '1'};
constexpr std::array<std::uint8_t, 8U> multi_engine_state_prefix{'A', 'T', 'L', 'S',
                                                                 'M', 'E', '0', '1'};
constexpr std::array<std::uint8_t, 8U> event_prefix{'A', 'T', 'L', 'S', 'E', 'V', '0', '1'};

[[nodiscard]] constexpr std::uint64_t canonical_capacity(std::size_t value) noexcept {
  if (value == std::numeric_limits<std::size_t>::max()) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return static_cast<std::uint64_t>(value);
}

using Sha256 = utility::Sha256;

class CanonicalEncoder final {
 public:
  void bytes(std::span<const std::uint8_t> value) noexcept { hash_.update(value); }

  void u8(std::uint8_t value) noexcept {
    const std::array bytes{value};
    hash_.update(bytes);
  }

  void u16(std::uint16_t value) noexcept {
    const std::array bytes{
        static_cast<std::uint8_t>((value >> 8U) & 0xffU),
        static_cast<std::uint8_t>(value & 0xffU),
    };
    hash_.update(bytes);
  }

  void u32(std::uint32_t value) noexcept {
    const std::array bytes{
        static_cast<std::uint8_t>((value >> 24U) & 0xffU),
        static_cast<std::uint8_t>((value >> 16U) & 0xffU),
        static_cast<std::uint8_t>((value >> 8U) & 0xffU),
        static_cast<std::uint8_t>(value & 0xffU),
    };
    hash_.update(bytes);
  }

  void u64(std::uint64_t value) noexcept {
    std::array<std::uint8_t, 8U> bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
      const auto shift = static_cast<unsigned>((7U - index) * 8U);
      bytes[index] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
    }
    hash_.update(bytes);
  }

  void i64(std::int64_t value) noexcept { u64(static_cast<std::uint64_t>(value)); }

  void boolean(bool value) noexcept { u8(value ? 1U : 0U); }

  [[nodiscard]] Digest256 finish() noexcept { return hash_.finish(); }

 private:
  Sha256 hash_;
};

void encode_order(CanonicalEncoder& encoder, const OrderSnapshot& order) noexcept {
  encoder.u64(order.order_id.value());
  encoder.u32(order.client_id.value());
  encoder.u32(order.instrument_id.value());
  encoder.u8(static_cast<std::uint8_t>(order.side));
  encoder.i64(order.price.value());
  encoder.u64(order.remaining_quantity.value());
  encoder.u64(order.priority_sequence.value());
}

void encode_level(CanonicalEncoder& encoder, const PriceLevelSnapshot& level) noexcept {
  encoder.i64(level.price.value());
  encoder.u64(level.aggregate_quantity.value());
  encoder.u64(static_cast<std::uint64_t>(level.orders.size()));
  for (const auto& order : level.orders) {
    encode_order(encoder, order);
  }
}

void encode_header(CanonicalEncoder& encoder, domain::EventType type,
                   const domain::EventHeader& header) noexcept {
  encoder.u8(static_cast<std::uint8_t>(type));
  encoder.u64(header.command_sequence.value());
  encoder.u32(header.event_index);
  encoder.u32(header.instrument_id.value());
}

void encode_optional_order_id(CanonicalEncoder& encoder,
                              const std::optional<domain::OrderId>& order_id) noexcept {
  encoder.boolean(order_id.has_value());
  encoder.u64(order_id.has_value() ? order_id->value() : 0U);
}

void encode_optional_level(CanonicalEncoder& encoder,
                           const std::optional<domain::TopOfBookLevel>& level) noexcept {
  encoder.boolean(level.has_value());
  encoder.i64(level.has_value() ? level->price.value() : 0);
  encoder.u64(level.has_value() ? level->aggregate_quantity.value() : 0U);
}

void encode_event(CanonicalEncoder& encoder, const domain::Event& event) noexcept {
  std::visit(
      [&encoder](const auto& value) noexcept {
        using Value = std::remove_cvref_t<decltype(value)>;
        encode_header(encoder, domain::expected_event_type<Value>(), value.header);
        if constexpr (std::is_same_v<Value, domain::AcceptedEvent>) {
          encoder.u8(static_cast<std::uint8_t>(value.command_type));
        } else if constexpr (std::is_same_v<Value, domain::RejectedEvent>) {
          encoder.u8(static_cast<std::uint8_t>(value.command_type));
          encoder.u16(static_cast<std::uint16_t>(value.reason));
          encode_optional_order_id(encoder, value.order_id);
        } else if constexpr (std::is_same_v<Value, domain::TradeEvent>) {
          encoder.u64(value.aggressor_order_id.value());
          encoder.u64(value.resting_order_id.value());
          encoder.u32(value.aggressor_client_id.value());
          encoder.u32(value.resting_client_id.value());
          encoder.u8(static_cast<std::uint8_t>(value.aggressor_side));
          encoder.i64(value.execution_price.value());
          encoder.u64(value.execution_quantity.value());
          encoder.u64(value.aggressor_remaining.value());
          encoder.u64(value.resting_remaining.value());
        } else if constexpr (std::is_same_v<Value, domain::RestedEvent>) {
          encoder.u64(value.order_id.value());
          encoder.u32(value.client_id.value());
          encoder.u8(static_cast<std::uint8_t>(value.side));
          encoder.i64(value.price.value());
          encoder.u64(value.remaining_quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::CanceledEvent>) {
          encoder.u64(value.order_id.value());
          encoder.u64(value.canceled_quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::ReplacedEvent>) {
          encoder.u64(value.old_order_id.value());
          encoder.u64(value.new_order_id.value());
        } else if constexpr (std::is_same_v<Value, domain::DoneEvent>) {
          encoder.u64(value.order_id.value());
          encoder.u8(static_cast<std::uint8_t>(value.reason));
          encoder.u64(value.remaining_quantity.value());
        } else {
          static_assert(std::is_same_v<Value, domain::BookChangedEvent>);
          encode_optional_level(encoder, value.best_bid);
          encode_optional_level(encoder, value.best_ask);
        }
      },
      event);
}

}  // namespace

std::string Digest256::hex() const {
  static constexpr std::string_view digits{"0123456789abcdef"};
  std::string result(bytes.size() * 2U, '0');
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    result[index * 2U] = digits[(bytes[index] >> 4U) & 0x0fU];
    result[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
  }
  return result;
}

Digest256 state_digest(const BookSnapshot& snapshot) noexcept {
  CanonicalEncoder encoder;
  encoder.bytes(state_prefix);
  encoder.u16(snapshot.semantics_version);
  encoder.u32(snapshot.instrument_id.value());
  encoder.u64(snapshot.last_sequence.value());
  encoder.boolean(snapshot.sequence_exhausted);
  encoder.u64(snapshot.active_order_count);
  encoder.u64(static_cast<std::uint64_t>(snapshot.bids.size()));
  for (const auto& level : snapshot.bids) {
    encode_level(encoder, level);
  }
  encoder.u64(static_cast<std::uint64_t>(snapshot.asks.size()));
  for (const auto& level : snapshot.asks) {
    encode_level(encoder, level);
  }
  return encoder.finish();
}

Digest256 state_digest(const EngineSnapshot& snapshot) noexcept {
  CanonicalEncoder encoder;
  encoder.bytes(multi_engine_state_prefix);
  encoder.u16(snapshot.semantics_version);
  encoder.u64(canonical_capacity(snapshot.engine_config.max_total_active_orders));
  encoder.u64(static_cast<std::uint64_t>(snapshot.catalog.size()));
  for (const auto& config : snapshot.catalog) {
    encoder.u32(config.instrument_id.value());
    encoder.u64(config.matching.max_order_quantity.value());
    encoder.i64(config.matching.tick_increment.value());
    encoder.u64(canonical_capacity(config.matching.max_active_orders));
  }
  encoder.u64(snapshot.last_sequence.value());
  encoder.boolean(snapshot.sequence_exhausted);
  encoder.u64(snapshot.active_order_count);
  encoder.u64(static_cast<std::uint64_t>(snapshot.instruments.size()));
  for (const auto& instrument : snapshot.instruments) {
    encoder.u32(instrument.instrument_id.value());
    encoder.u64(instrument.active_order_count);
    encoder.u64(static_cast<std::uint64_t>(instrument.bids.size()));
    for (const auto& level : instrument.bids) {
      encode_level(encoder, level);
    }
    encoder.u64(static_cast<std::uint64_t>(instrument.asks.size()));
    for (const auto& level : instrument.asks) {
      encode_level(encoder, level);
    }
  }
  return encoder.finish();
}

Digest256 event_digest(const domain::EventBatch& batch) noexcept {
  CanonicalEncoder encoder;
  encoder.bytes(event_prefix);
  encoder.u16(atlaslob_semantics_version);
  encoder.u64(batch.command_sequence().value());
  encoder.u32(batch.instrument_id().value());
  encoder.u64(static_cast<std::uint64_t>(batch.size()));
  for (const auto& event : batch.events()) {
    encode_event(encoder, event);
  }
  return encoder.finish();
}

#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
namespace core::test {

Digest256 sha256_for_testing(std::span<const std::uint8_t> bytes) noexcept {
  Sha256 hash;
  hash.update(bytes);
  return hash.finish();
}

}  // namespace core::test
#endif

}  // namespace atlaslob
