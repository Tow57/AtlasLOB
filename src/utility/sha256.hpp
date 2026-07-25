#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "atlaslob/digest.hpp"

namespace atlaslob::utility {

// Allocation-free SHA-256 shared by canonical evidence and persistence
// codecs. Callers use it for deterministic evidence, not authentication.
class Sha256 final {
 public:
  void update(std::span<const std::uint8_t> input) noexcept;
  [[nodiscard]] Digest256 finish() noexcept;

 private:
  [[nodiscard]] static constexpr std::uint32_t choose(std::uint32_t x, std::uint32_t y,
                                                      std::uint32_t z) noexcept {
    return (x & y) ^ (~x & z);
  }
  [[nodiscard]] static constexpr std::uint32_t majority(std::uint32_t x, std::uint32_t y,
                                                        std::uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
  }
  [[nodiscard]] static constexpr std::uint32_t large_sigma_zero(std::uint32_t value) noexcept;
  [[nodiscard]] static constexpr std::uint32_t large_sigma_one(std::uint32_t value) noexcept;
  [[nodiscard]] static constexpr std::uint32_t small_sigma_zero(std::uint32_t value) noexcept;
  [[nodiscard]] static constexpr std::uint32_t small_sigma_one(std::uint32_t value) noexcept;
  void transform(const std::array<std::uint8_t, 64U>& block) noexcept;

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

[[nodiscard]] Digest256 sha256(std::span<const std::uint8_t> input) noexcept;

}  // namespace atlaslob::utility
