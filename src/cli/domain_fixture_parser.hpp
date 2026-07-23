#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "atlaslob/domain/types.hpp"

namespace atlaslob::cli::detail {

struct ParsedNewOrder final {
  std::uint32_t client_id{};
  std::uint64_t order_id{};
  std::uint32_t instrument_id{};
  domain::Side side{};
  domain::OrderType order_type{};
  domain::TimeInForce time_in_force{};
  std::optional<std::int64_t> limit_price{};
  std::uint64_t quantity{};
};

struct ParsedCancelOrder final {
  std::uint32_t client_id{};
  std::uint64_t order_id{};
  std::uint32_t instrument_id{};
};

struct ParsedReplaceOrder final {
  std::uint32_t client_id{};
  std::uint64_t old_order_id{};
  std::uint64_t new_order_id{};
  std::uint32_t instrument_id{};
  std::int64_t new_limit_price{};
  std::uint64_t new_quantity{};
};

using ParsedCommand = std::variant<ParsedNewOrder, ParsedCancelOrder, ParsedReplaceOrder>;

struct IgnoredLine final {};

struct ParseError final {
  std::string reason{};
};

using ParsedLine = std::variant<IgnoredLine, ParsedCommand, ParseError>;

[[nodiscard]] ParsedLine parse_fixture_line(std::string_view line);

}  // namespace atlaslob::cli::detail
