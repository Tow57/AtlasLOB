#include "domain_fixture_parser.hpp"

#include <array>
#include <charconv>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace atlaslob::cli::detail {
namespace {

[[nodiscard]] constexpr bool is_ascii_space(char value) noexcept {
  switch (value) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '\v':
      return true;
    default:
      return false;
  }
}

[[nodiscard]] std::vector<std::string_view> tokenize(std::string_view line) {
  std::vector<std::string_view> tokens;
  std::size_t position = 0;

  while (position < line.size()) {
    while (position < line.size() && is_ascii_space(line[position])) {
      ++position;
    }

    if (position == line.size() || line[position] == '#') {
      break;
    }

    const auto begin = position;
    while (position < line.size() && !is_ascii_space(line[position])) {
      ++position;
    }
    tokens.emplace_back(line.substr(begin, position - begin));
  }

  return tokens;
}

template <typename Integer>
[[nodiscard]] bool parse_integer(std::string_view token, Integer& destination) noexcept {
  static_assert(std::numeric_limits<Integer>::is_integer);

  Integer value{};
  const auto* const begin = token.data();
  const auto* const end = begin + token.size();
  const auto [parsed_end, error] = std::from_chars(begin, end, value, 10);
  if (error != std::errc{} || parsed_end != end) {
    return false;
  }

  destination = value;
  return true;
}

[[nodiscard]] bool parse_side(std::string_view token, domain::Side& destination) noexcept {
  if (token == "BUY") {
    destination = domain::Side::buy;
    return true;
  }
  if (token == "SELL") {
    destination = domain::Side::sell;
    return true;
  }
  return false;
}

[[nodiscard]] bool parse_order_type(std::string_view token,
                                    domain::OrderType& destination) noexcept {
  if (token == "LIMIT") {
    destination = domain::OrderType::limit;
    return true;
  }
  if (token == "MARKET") {
    destination = domain::OrderType::market;
    return true;
  }
  return false;
}

[[nodiscard]] bool parse_time_in_force(std::string_view token,
                                       domain::TimeInForce& destination) noexcept {
  if (token == "GTC") {
    destination = domain::TimeInForce::gtc;
    return true;
  }
  if (token == "IOC") {
    destination = domain::TimeInForce::ioc;
    return true;
  }
  if (token == "FOK") {
    destination = domain::TimeInForce::fok;
    return true;
  }
  return false;
}

[[nodiscard]] ParseError invalid_field(std::string_view field) {
  return ParseError{.reason = "invalid_" + std::string{field}};
}

[[nodiscard]] ParsedLine parse_new(std::span<const std::string_view> tokens) {
  constexpr std::size_t expected_field_count = 9;
  if (tokens.size() != expected_field_count) {
    return ParseError{.reason = "NEW_expected_9_fields"};
  }

  ParsedNewOrder order{};
  if (!parse_integer(tokens[1], order.client_id)) {
    return invalid_field("client_id");
  }
  if (!parse_integer(tokens[2], order.order_id)) {
    return invalid_field("order_id");
  }
  if (!parse_integer(tokens[3], order.instrument_id)) {
    return invalid_field("instrument_id");
  }
  if (!parse_side(tokens[4], order.side)) {
    return invalid_field("side");
  }
  if (!parse_order_type(tokens[5], order.order_type)) {
    return invalid_field("order_type");
  }
  if (!parse_time_in_force(tokens[6], order.time_in_force)) {
    return invalid_field("time_in_force");
  }
  if (tokens[7] != "-") {
    std::int64_t price{};
    if (!parse_integer(tokens[7], price)) {
      return invalid_field("price");
    }
    order.limit_price = price;
  }
  if (!parse_integer(tokens[8], order.quantity)) {
    return invalid_field("quantity");
  }

  return ParsedCommand{order};
}

[[nodiscard]] ParsedLine parse_cancel(std::span<const std::string_view> tokens) {
  constexpr std::size_t expected_field_count = 4;
  if (tokens.size() != expected_field_count) {
    return ParseError{.reason = "CANCEL_expected_4_fields"};
  }

  ParsedCancelOrder order{};
  if (!parse_integer(tokens[1], order.client_id)) {
    return invalid_field("client_id");
  }
  if (!parse_integer(tokens[2], order.order_id)) {
    return invalid_field("order_id");
  }
  if (!parse_integer(tokens[3], order.instrument_id)) {
    return invalid_field("instrument_id");
  }

  return ParsedCommand{order};
}

[[nodiscard]] ParsedLine parse_replace(std::span<const std::string_view> tokens) {
  constexpr std::size_t expected_field_count = 7;
  if (tokens.size() != expected_field_count) {
    return ParseError{.reason = "REPLACE_expected_7_fields"};
  }

  ParsedReplaceOrder order{};
  if (!parse_integer(tokens[1], order.client_id)) {
    return invalid_field("client_id");
  }
  if (!parse_integer(tokens[2], order.old_order_id)) {
    return invalid_field("old_order_id");
  }
  if (!parse_integer(tokens[3], order.new_order_id)) {
    return invalid_field("new_order_id");
  }
  if (!parse_integer(tokens[4], order.instrument_id)) {
    return invalid_field("instrument_id");
  }
  if (!parse_integer(tokens[5], order.new_limit_price)) {
    return invalid_field("new_price");
  }
  if (!parse_integer(tokens[6], order.new_quantity)) {
    return invalid_field("new_quantity");
  }

  return ParsedCommand{order};
}

}  // namespace

ParsedLine parse_fixture_line(std::string_view line) {
  const auto tokens = tokenize(line);
  if (tokens.empty()) {
    return IgnoredLine{};
  }

  if (tokens.front() == "NEW") {
    return parse_new(tokens);
  }
  if (tokens.front() == "CANCEL") {
    return parse_cancel(tokens);
  }
  if (tokens.front() == "REPLACE") {
    return parse_replace(tokens);
  }
  return ParseError{.reason = "unknown_command"};
}

domain::Command to_domain_command(const ParsedCommand& command) {
  return std::visit(
      [](const auto& parsed) -> domain::Command {
        using Parsed = std::remove_cvref_t<decltype(parsed)>;
        if constexpr (std::is_same_v<Parsed, ParsedNewOrder>) {
          return domain::NewOrder{
              .client_id = domain::ClientId{parsed.client_id},
              .order_id = domain::OrderId{parsed.order_id},
              .instrument_id = domain::InstrumentId{parsed.instrument_id},
              .side = parsed.side,
              .order_type = parsed.order_type,
              .time_in_force = parsed.time_in_force,
              .limit_price = parsed.limit_price.has_value()
                                 ? std::optional{domain::PriceTicks{*parsed.limit_price}}
                                 : std::nullopt,
              .quantity = domain::Quantity{parsed.quantity},
          };
        } else if constexpr (std::is_same_v<Parsed, ParsedCancelOrder>) {
          return domain::CancelOrder{
              .client_id = domain::ClientId{parsed.client_id},
              .order_id = domain::OrderId{parsed.order_id},
              .instrument_id = domain::InstrumentId{parsed.instrument_id},
          };
        } else {
          static_assert(std::is_same_v<Parsed, ParsedReplaceOrder>);
          return domain::ReplaceOrder{
              .client_id = domain::ClientId{parsed.client_id},
              .old_order_id = domain::OrderId{parsed.old_order_id},
              .new_order_id = domain::OrderId{parsed.new_order_id},
              .instrument_id = domain::InstrumentId{parsed.instrument_id},
              .new_limit_price = domain::PriceTicks{parsed.new_limit_price},
              .new_quantity = domain::Quantity{parsed.new_quantity},
          };
        }
      },
      command);
}

}  // namespace atlaslob::cli::detail
