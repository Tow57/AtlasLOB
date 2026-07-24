#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "atlaslob/digest.hpp"

#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
#include "canonical_digest.hpp"
#endif

namespace atlaslob {
namespace {

constexpr std::array<std::uint8_t, 8U> state_prefix{'A', 'T', 'L', 'S', 'S', 'T', '0', '1'};
constexpr std::array<std::uint8_t, 8U> event_prefix{'A', 'T', 'L', 'S', 'E', 'V', '0', '1'};

class Sha256 final {
 public:
  void update(std::span<const std::uint8_t> input) noexcept {
    total_bytes_ += static_cast<std::uint64_t>(input.size());
    for (const auto byte : input) {
      buffer_[buffer_size_] = byte;
      ++buffer_size_;
      if (buffer_size_ == buffer_.size()) {
        transform(buffer_);
        buffer_size_ = 0U;
      }
    }
  }

  [[nodiscard]] Digest256 finish() noexcept {
    const auto message_bits = total_bytes_ * 8U;
    buffer_[buffer_size_] = 0x80U;
    ++buffer_size_;

    if (buffer_size_ > 56U) {
      while (buffer_size_ < buffer_.size()) {
        buffer_[buffer_size_] = 0U;
        ++buffer_size_;
      }
      transform(buffer_);
      buffer_size_ = 0U;
    }
    while (buffer_size_ < 56U) {
      buffer_[buffer_size_] = 0U;
      ++buffer_size_;
    }
    for (std::size_t index = 0U; index < 8U; ++index) {
      const auto shift = static_cast<unsigned>((7U - index) * 8U);
      buffer_[56U + index] = static_cast<std::uint8_t>((message_bits >> shift) & 0xffU);
    }
    transform(buffer_);

    Digest256 result;
    for (std::size_t word = 0U; word < state_.size(); ++word) {
      for (std::size_t byte = 0U; byte < 4U; ++byte) {
        const auto shift = static_cast<unsigned>((3U - byte) * 8U);
        result.bytes[word * 4U + byte] = static_cast<std::uint8_t>((state_[word] >> shift) & 0xffU);
      }
    }
    return result;
  }

 private:
  [[nodiscard]] static constexpr std::uint32_t choose(std::uint32_t x, std::uint32_t y,
                                                      std::uint32_t z) noexcept {
    return (x & y) ^ (~x & z);
  }

  [[nodiscard]] static constexpr std::uint32_t majority(std::uint32_t x, std::uint32_t y,
                                                        std::uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
  }

  [[nodiscard]] static constexpr std::uint32_t large_sigma_zero(std::uint32_t value) noexcept {
    return std::rotr(value, 2) ^ std::rotr(value, 13) ^ std::rotr(value, 22);
  }

  [[nodiscard]] static constexpr std::uint32_t large_sigma_one(std::uint32_t value) noexcept {
    return std::rotr(value, 6) ^ std::rotr(value, 11) ^ std::rotr(value, 25);
  }

  [[nodiscard]] static constexpr std::uint32_t small_sigma_zero(std::uint32_t value) noexcept {
    return std::rotr(value, 7) ^ std::rotr(value, 18) ^ (value >> 3U);
  }

  [[nodiscard]] static constexpr std::uint32_t small_sigma_one(std::uint32_t value) noexcept {
    return std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10U);
  }

  void transform(const std::array<std::uint8_t, 64U>& block) noexcept {
    std::array<std::uint32_t, 64U> words{};
    for (std::size_t index = 0U; index < 16U; ++index) {
      const auto offset = index * 4U;
      words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                     (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                     (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                     static_cast<std::uint32_t>(block[offset + 3U]);
    }
    for (std::size_t index = 16U; index < words.size(); ++index) {
      words[index] = small_sigma_one(words[index - 2U]) + words[index - 7U] +
                     small_sigma_zero(words[index - 15U]) + words[index - 16U];
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (std::size_t index = 0U; index < words.size(); ++index) {
      const auto first =
          h + large_sigma_one(e) + choose(e, f, g) + round_constants[index] + words[index];
      const auto second = large_sigma_zero(a) + majority(a, b, c);
      h = g;
      g = f;
      f = e;
      e = d + first;
      d = c;
      c = b;
      b = a;
      a = first + second;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  static constexpr std::array<std::uint32_t, 64U> round_constants{
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
      0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
      0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
      0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
      0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
      0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
      0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
      0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
      0xc67178f2U,
  };

  std::array<std::uint32_t, 8U> state_{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  std::array<std::uint8_t, 64U> buffer_{};
  std::size_t buffer_size_{};
  std::uint64_t total_bytes_{};
};

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
