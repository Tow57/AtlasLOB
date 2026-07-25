#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace atlaslob::persistence::detail {

[[nodiscard]] bool checked_add(std::size_t left, std::size_t right, std::size_t& result) noexcept;
[[nodiscard]] bool checked_multiply(std::size_t left, std::size_t right,
                                    std::size_t& result) noexcept;

[[nodiscard]] std::uint64_t canonical_capacity(std::size_t value) noexcept;
[[nodiscard]] std::optional<std::size_t> host_capacity(std::uint64_t value) noexcept;

class BinaryEncoder final {
 public:
  explicit BinaryEncoder(std::size_t reserve_bytes = 0U);

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void bytes(std::span<const std::uint8_t> value);

  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
  [[nodiscard]] std::span<const std::uint8_t> view() const noexcept { return bytes_; }
  [[nodiscard]] std::vector<std::uint8_t> take() && noexcept { return std::move(bytes_); }

 private:
  std::vector<std::uint8_t> bytes_;
};

class BinaryDecoder final {
 public:
  explicit BinaryDecoder(std::span<const std::uint8_t> bytes) noexcept : bytes_{bytes} {}

  [[nodiscard]] bool u8(std::uint8_t& value) noexcept;
  [[nodiscard]] bool u16(std::uint16_t& value) noexcept;
  [[nodiscard]] bool u32(std::uint32_t& value) noexcept;
  [[nodiscard]] bool u64(std::uint64_t& value) noexcept;
  [[nodiscard]] bool i64(std::int64_t& value) noexcept;
  [[nodiscard]] bool bytes(std::size_t count, std::span<const std::uint8_t>& value) noexcept;

  [[nodiscard]] std::size_t position() const noexcept { return position_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }

 private:
  std::span<const std::uint8_t> bytes_;
  std::size_t position_{};
};

}  // namespace atlaslob::persistence::detail
