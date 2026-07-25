#include "sha256.hpp"

#include <bit>

namespace atlaslob::utility {

constexpr std::uint32_t Sha256::large_sigma_zero(std::uint32_t value) noexcept {
  return std::rotr(value, 2) ^ std::rotr(value, 13) ^ std::rotr(value, 22);
}

constexpr std::uint32_t Sha256::large_sigma_one(std::uint32_t value) noexcept {
  return std::rotr(value, 6) ^ std::rotr(value, 11) ^ std::rotr(value, 25);
}

constexpr std::uint32_t Sha256::small_sigma_zero(std::uint32_t value) noexcept {
  return std::rotr(value, 7) ^ std::rotr(value, 18) ^ (value >> 3U);
}

constexpr std::uint32_t Sha256::small_sigma_one(std::uint32_t value) noexcept {
  return std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10U);
}

void Sha256::update(std::span<const std::uint8_t> input) noexcept {
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

Digest256 Sha256::finish() noexcept {
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

void Sha256::transform(const std::array<std::uint8_t, 64U>& block) noexcept {
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

Digest256 sha256(std::span<const std::uint8_t> input) noexcept {
  Sha256 hash;
  hash.update(input);
  return hash.finish();
}

}  // namespace atlaslob::utility
