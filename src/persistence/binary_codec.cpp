#include "binary_codec.hpp"

#include <array>
#include <bit>
#include <limits>

namespace atlaslob::persistence::detail {

bool checked_add(std::size_t left, std::size_t right, std::size_t& result) noexcept {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool checked_multiply(std::size_t left, std::size_t right, std::size_t& result) noexcept {
  if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

std::uint64_t canonical_capacity(std::size_t value) noexcept {
  if (value == std::numeric_limits<std::size_t>::max()) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return static_cast<std::uint64_t>(value);
}

std::optional<std::size_t> host_capacity(std::uint64_t value) noexcept {
  if (value == std::numeric_limits<std::uint64_t>::max()) {
    return std::numeric_limits<std::size_t>::max();
  }
  if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      return std::nullopt;
    }
  }
  return static_cast<std::size_t>(value);
}

BinaryEncoder::BinaryEncoder(std::size_t reserve_bytes) { bytes_.reserve(reserve_bytes); }

void BinaryEncoder::u8(std::uint8_t value) { bytes_.push_back(value); }

void BinaryEncoder::u16(std::uint16_t value) {
  const std::array encoded{
      static_cast<std::uint8_t>((value >> 8U) & 0xffU),
      static_cast<std::uint8_t>(value & 0xffU),
  };
  bytes(encoded);
}

void BinaryEncoder::u32(std::uint32_t value) {
  const std::array encoded{
      static_cast<std::uint8_t>((value >> 24U) & 0xffU),
      static_cast<std::uint8_t>((value >> 16U) & 0xffU),
      static_cast<std::uint8_t>((value >> 8U) & 0xffU),
      static_cast<std::uint8_t>(value & 0xffU),
  };
  bytes(encoded);
}

void BinaryEncoder::u64(std::uint64_t value) {
  std::array<std::uint8_t, 8U> encoded{};
  for (std::size_t index = 0U; index < encoded.size(); ++index) {
    const auto shift = static_cast<unsigned>((7U - index) * 8U);
    encoded[index] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
  }
  bytes(encoded);
}

void BinaryEncoder::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void BinaryEncoder::bytes(std::span<const std::uint8_t> value) {
  bytes_.insert(bytes_.end(), value.begin(), value.end());
}

bool BinaryDecoder::u8(std::uint8_t& value) noexcept {
  if (remaining() < 1U) {
    return false;
  }
  value = bytes_[position_];
  ++position_;
  return true;
}

bool BinaryDecoder::u16(std::uint16_t& value) noexcept {
  if (remaining() < 2U) {
    return false;
  }
  value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes_[position_]) << 8U) |
                                     static_cast<std::uint16_t>(bytes_[position_ + 1U]));
  position_ += 2U;
  return true;
}

bool BinaryDecoder::u32(std::uint32_t& value) noexcept {
  if (remaining() < 4U) {
    return false;
  }
  value = (static_cast<std::uint32_t>(bytes_[position_]) << 24U) |
          (static_cast<std::uint32_t>(bytes_[position_ + 1U]) << 16U) |
          (static_cast<std::uint32_t>(bytes_[position_ + 2U]) << 8U) |
          static_cast<std::uint32_t>(bytes_[position_ + 3U]);
  position_ += 4U;
  return true;
}

bool BinaryDecoder::u64(std::uint64_t& value) noexcept {
  if (remaining() < 8U) {
    return false;
  }
  value = 0U;
  for (std::size_t index = 0U; index < 8U; ++index) {
    value = (value << 8U) | static_cast<std::uint64_t>(bytes_[position_ + index]);
  }
  position_ += 8U;
  return true;
}

bool BinaryDecoder::i64(std::int64_t& value) noexcept {
  std::uint64_t encoded = 0U;
  if (!u64(encoded)) {
    return false;
  }
  value = std::bit_cast<std::int64_t>(encoded);
  return true;
}

bool BinaryDecoder::bytes(std::size_t count, std::span<const std::uint8_t>& value) noexcept {
  if (remaining() < count) {
    return false;
  }
  value = bytes_.subspan(position_, count);
  position_ += count;
  return true;
}

}  // namespace atlaslob::persistence::detail
