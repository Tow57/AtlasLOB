#include "benchmark_workload.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "sha256.hpp"

namespace atlaslob::benchmark {
namespace {

constexpr std::string_view header_magic{"ATLAS_DIFF_V2"};

static_assert(std::numeric_limits<std::size_t>::digits <=
              std::numeric_limits<std::uint64_t>::digits);

struct Header final {
  MultiInstrumentEngineConfig engine_config{};
  std::vector<InstrumentConfig> catalog;
  std::uint64_t catalog_count{};
  std::uint64_t command_count{};
  std::uint64_t checkpoint_interval{};
};

using ParsedCommand = std::variant<domain::Command, std::string_view>;

[[nodiscard]] bool canonical_fields(std::string_view line, std::vector<std::string_view>& output) {
  output.clear();
  if (line.empty() || line.front() == ' ' || line.back() == ' ') {
    return false;
  }
  if (!std::all_of(line.begin(), line.end(), [](char value) {
        const auto byte = static_cast<unsigned char>(value);
        return byte >= 0x20U && byte <= 0x7eU;
      })) {
    return false;
  }
  std::size_t begin = 0U;
  while (begin < line.size()) {
    const auto end = line.find(' ', begin);
    const auto count = end == std::string_view::npos ? line.size() - begin : end - begin;
    if (count == 0U) {
      return false;
    }
    const auto field = line.substr(begin, count);
    output.push_back(field);
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1U;
  }
  return !output.empty();
}

[[nodiscard]] bool canonical_unsigned_token(std::string_view token) noexcept {
  if (token.empty() || (token.size() > 1U && token.front() == '0')) {
    return false;
  }
  return std::all_of(token.begin(), token.end(),
                     [](char value) { return value >= '0' && value <= '9'; });
}

template <typename Integer>
[[nodiscard]] bool parse_unsigned(std::string_view token, Integer& destination) noexcept {
  static_assert(std::is_integral_v<Integer>);
  static_assert(std::is_unsigned_v<Integer>);
  if (!canonical_unsigned_token(token)) {
    return false;
  }
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

[[nodiscard]] bool parse_i64(std::string_view token, std::int64_t& destination) noexcept {
  if (token.empty()) {
    return false;
  }
  if (token.front() == '-') {
    const auto magnitude = token.substr(1U);
    if (!canonical_unsigned_token(magnitude) || magnitude == "0") {
      return false;
    }
  } else if (!canonical_unsigned_token(token)) {
    return false;
  }
  std::int64_t value{};
  const auto* const begin = token.data();
  const auto* const end = begin + token.size();
  const auto [parsed_end, error] = std::from_chars(begin, end, value, 10);
  if (error != std::errc{} || parsed_end != end) {
    return false;
  }
  destination = value;
  return true;
}

[[nodiscard]] bool parse_u8(std::string_view token, std::uint8_t& destination) noexcept {
  std::uint64_t value{};
  if (!parse_unsigned(token, value) || value > std::numeric_limits<std::uint8_t>::max()) {
    return false;
  }
  destination = static_cast<std::uint8_t>(value);
  return true;
}

[[nodiscard]] std::optional<WorkloadParseError> read_protocol_line(std::istream& input,
                                                                   std::string& line,
                                                                   std::uint64_t& line_number,
                                                                   std::string_view missing_code,
                                                                   utility::Sha256& stream_hash) {
  line.clear();
  const auto current_line = line_number + 1U;
  for (;;) {
    const auto value = input.get();
    if (value == std::char_traits<char>::eof()) {
      if (input.bad() || (input.fail() && !input.eof())) {
        return WorkloadParseError{current_line, "input_read_failure"};
      }
      if (line.empty()) {
        return WorkloadParseError{current_line, missing_code};
      }
      line_number = current_line;
      return WorkloadParseError{current_line, "invalid_line_ending"};
    }

    const auto character = static_cast<char>(value);
    if (character == '\n') {
      const auto* const bytes = reinterpret_cast<const std::uint8_t*>(line.data());
      stream_hash.update(std::span<const std::uint8_t>{bytes, line.size()});
      constexpr std::uint8_t line_feed{'\n'};
      stream_hash.update(std::span<const std::uint8_t>{&line_feed, 1U});
      line_number = current_line;
      return std::nullopt;
    }
    if (character == '\r') {
      line_number = current_line;
      return WorkloadParseError{current_line, "invalid_line_ending"};
    }
    if (line.size() == maximum_benchmark_line_bytes) {
      line_number = current_line;
      return WorkloadParseError{current_line, "line_exceeds_limit"};
    }
    line.push_back(character);
  }
}

[[nodiscard]] std::variant<Header, std::string_view> parse_header(std::string_view line) {
  std::vector<std::string_view> fields;
  if (!canonical_fields(line, fields)) {
    return std::string_view{"noncanonical_header"};
  }
  if (fields.size() != 5U) {
    return std::string_view{"invalid_header_field_count"};
  }
  if (fields[0] != header_magic) {
    return std::string_view{"unsupported_header"};
  }

  std::uint64_t max_total{};
  std::uint64_t catalog_count{};
  std::uint64_t command_count{};
  std::uint64_t checkpoint_interval{};
  if (!parse_unsigned(fields[1], max_total) ||
      max_total > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::string_view{"invalid_header_max_total_active_orders"};
  }
  if (!parse_unsigned(fields[2], catalog_count) || catalog_count == 0U ||
      catalog_count > std::numeric_limits<std::uint32_t>::max() ||
      catalog_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::string_view{"invalid_header_catalog_count"};
  }
  if (!parse_unsigned(fields[3], command_count) ||
      command_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::string_view{"invalid_header_command_count"};
  }
  if (!parse_unsigned(fields[4], checkpoint_interval)) {
    return std::string_view{"invalid_header_checkpoint_interval"};
  }

  Header header{
      .engine_config =
          {
              .max_total_active_orders = static_cast<std::size_t>(max_total),
          },
      .catalog = {},
      .catalog_count = catalog_count,
      .command_count = command_count,
      .checkpoint_interval = checkpoint_interval,
  };
  return header;
}

[[nodiscard]] std::variant<InstrumentConfig, std::string_view> parse_instrument(
    std::string_view line) {
  std::vector<std::string_view> fields;
  if (!canonical_fields(line, fields)) {
    return std::string_view{"noncanonical_instrument"};
  }
  if (fields.size() != 5U || fields[0] != "I") {
    return std::string_view{"invalid_instrument_record"};
  }

  std::uint32_t instrument_id{};
  std::uint64_t max_quantity{};
  std::int64_t tick_increment{};
  std::uint64_t max_active{};
  if (!parse_unsigned(fields[1], instrument_id) || instrument_id == 0U) {
    return std::string_view{"invalid_instrument_id"};
  }
  if (!parse_unsigned(fields[2], max_quantity) || max_quantity == 0U) {
    return std::string_view{"invalid_instrument_max_quantity"};
  }
  if (!parse_i64(fields[3], tick_increment) || tick_increment <= 0) {
    return std::string_view{"invalid_instrument_tick_increment"};
  }
  if (!parse_unsigned(fields[4], max_active) ||
      max_active > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::string_view{"invalid_instrument_max_active_orders"};
  }

  return InstrumentConfig{
      .instrument_id = domain::InstrumentId{instrument_id},
      .matching =
          {
              .max_order_quantity = domain::Quantity{max_quantity},
              .tick_increment = domain::PriceTicks{tick_increment},
              .max_active_orders = static_cast<std::size_t>(max_active),
          },
  };
}

[[nodiscard]] ParsedCommand parse_new(const std::vector<std::string_view>& fields) {
  if (fields.size() != 10U) {
    return std::string_view{"invalid_new_field_count"};
  }
  std::uint32_t client{};
  std::uint64_t order{};
  std::uint32_t instrument{};
  std::uint8_t side{};
  std::uint8_t order_type{};
  std::uint8_t time_in_force{};
  std::uint8_t price_present{};
  std::int64_t price{};
  std::uint64_t quantity{};
  if (!parse_unsigned(fields[1], client)) {
    return std::string_view{"invalid_new_client"};
  }
  if (!parse_unsigned(fields[2], order)) {
    return std::string_view{"invalid_new_order"};
  }
  if (!parse_unsigned(fields[3], instrument)) {
    return std::string_view{"invalid_new_instrument"};
  }
  if (!parse_u8(fields[4], side)) {
    return std::string_view{"invalid_new_side_code"};
  }
  if (!parse_u8(fields[5], order_type)) {
    return std::string_view{"invalid_new_order_type_code"};
  }
  if (!parse_u8(fields[6], time_in_force)) {
    return std::string_view{"invalid_new_time_in_force_code"};
  }
  if (!parse_u8(fields[7], price_present) || price_present > 1U) {
    return std::string_view{"invalid_new_price_presence"};
  }
  if (!parse_i64(fields[8], price)) {
    return std::string_view{"invalid_new_price"};
  }
  if (price_present == 0U && price != 0) {
    return std::string_view{"nonzero_absent_price_placeholder"};
  }
  if (!parse_unsigned(fields[9], quantity)) {
    return std::string_view{"invalid_new_quantity"};
  }
  return domain::Command{domain::NewOrder{
      .client_id = domain::ClientId{client},
      .order_id = domain::OrderId{order},
      .instrument_id = domain::InstrumentId{instrument},
      .side = static_cast<domain::Side>(side),
      .order_type = static_cast<domain::OrderType>(order_type),
      .time_in_force = static_cast<domain::TimeInForce>(time_in_force),
      .limit_price = price_present == 0U
                         ? std::nullopt
                         : std::optional<domain::PriceTicks>{domain::PriceTicks{price}},
      .quantity = domain::Quantity{quantity},
  }};
}

[[nodiscard]] ParsedCommand parse_cancel(const std::vector<std::string_view>& fields) {
  if (fields.size() != 4U) {
    return std::string_view{"invalid_cancel_field_count"};
  }
  std::uint32_t client{};
  std::uint64_t order{};
  std::uint32_t instrument{};
  if (!parse_unsigned(fields[1], client)) {
    return std::string_view{"invalid_cancel_client"};
  }
  if (!parse_unsigned(fields[2], order)) {
    return std::string_view{"invalid_cancel_order"};
  }
  if (!parse_unsigned(fields[3], instrument)) {
    return std::string_view{"invalid_cancel_instrument"};
  }
  return domain::Command{domain::CancelOrder{
      .client_id = domain::ClientId{client},
      .order_id = domain::OrderId{order},
      .instrument_id = domain::InstrumentId{instrument},
  }};
}

[[nodiscard]] ParsedCommand parse_replace(const std::vector<std::string_view>& fields) {
  if (fields.size() != 7U) {
    return std::string_view{"invalid_replace_field_count"};
  }
  std::uint32_t client{};
  std::uint64_t old_order{};
  std::uint64_t new_order{};
  std::uint32_t instrument{};
  std::int64_t price{};
  std::uint64_t quantity{};
  if (!parse_unsigned(fields[1], client)) {
    return std::string_view{"invalid_replace_client"};
  }
  if (!parse_unsigned(fields[2], old_order)) {
    return std::string_view{"invalid_replace_old_order"};
  }
  if (!parse_unsigned(fields[3], new_order)) {
    return std::string_view{"invalid_replace_new_order"};
  }
  if (!parse_unsigned(fields[4], instrument)) {
    return std::string_view{"invalid_replace_instrument"};
  }
  if (!parse_i64(fields[5], price)) {
    return std::string_view{"invalid_replace_price"};
  }
  if (!parse_unsigned(fields[6], quantity)) {
    return std::string_view{"invalid_replace_quantity"};
  }
  return domain::Command{domain::ReplaceOrder{
      .client_id = domain::ClientId{client},
      .old_order_id = domain::OrderId{old_order},
      .new_order_id = domain::OrderId{new_order},
      .instrument_id = domain::InstrumentId{instrument},
      .new_limit_price = domain::PriceTicks{price},
      .new_quantity = domain::Quantity{quantity},
  }};
}

[[nodiscard]] ParsedCommand parse_command(std::string_view line) {
  std::vector<std::string_view> fields;
  if (!canonical_fields(line, fields)) {
    return std::string_view{"noncanonical_command"};
  }
  if (fields[0] == "N") {
    return parse_new(fields);
  }
  if (fields[0] == "C") {
    return parse_cancel(fields);
  }
  if (fields[0] == "R") {
    return parse_replace(fields);
  }
  return std::string_view{"unknown_command"};
}

[[nodiscard]] WorkloadParseResult read_atlas_diff_v2_impl(
    std::istream& input, std::size_t maximum_catalog_count,
    std::optional<std::size_t> expected_command_count) {
  std::uint64_t line_number = 0U;
  std::string line;
  utility::Sha256 stream_hash;
  if (auto error = read_protocol_line(input, line, line_number, "missing_header", stream_hash);
      error.has_value()) {
    return *error;
  }

  auto parsed_header = parse_header(line);
  if (const auto* error = std::get_if<std::string_view>(&parsed_header)) {
    return WorkloadParseError{line_number, *error};
  }
  auto header = std::get<Header>(std::move(parsed_header));
  if (header.catalog_count > static_cast<std::uint64_t>(maximum_catalog_count) ||
      header.catalog_count > static_cast<std::uint64_t>(maximum_benchmark_catalog_count)) {
    return WorkloadParseError{line_number, "catalog_count_exceeds_limit"};
  }
  if (header.command_count > static_cast<std::uint64_t>(maximum_benchmark_command_count)) {
    return WorkloadParseError{line_number, "command_count_exceeds_limit"};
  }
  if (expected_command_count.has_value() &&
      header.command_count != static_cast<std::uint64_t>(*expected_command_count)) {
    return WorkloadParseError{line_number, "command_count_mismatch"};
  }
  if (header.checkpoint_interval != 0U) {
    return WorkloadParseError{line_number, "nonzero_checkpoint_interval"};
  }
  const auto expected_catalog_count = static_cast<std::size_t>(header.catalog_count);
  header.catalog.reserve(expected_catalog_count);
  std::unordered_set<domain::InstrumentId, domain::StrongValueHash<domain::InstrumentId>>
      instrument_ids;
  instrument_ids.reserve(expected_catalog_count);

  for (std::size_t index = 0U; index < expected_catalog_count; ++index) {
    if (auto error =
            read_protocol_line(input, line, line_number, "missing_instrument_record", stream_hash);
        error.has_value()) {
      return *error;
    }
    auto parsed = parse_instrument(line);
    if (const auto* error = std::get_if<std::string_view>(&parsed)) {
      return WorkloadParseError{line_number, *error};
    }
    auto instrument = std::get<InstrumentConfig>(parsed);
    if (!instrument_ids.insert(instrument.instrument_id).second) {
      return WorkloadParseError{line_number, "duplicate_instrument_id"};
    }
    if (!header.catalog.empty() && instrument.instrument_id < header.catalog.back().instrument_id) {
      return WorkloadParseError{line_number, "catalog_not_strictly_ascending"};
    }
    header.catalog.push_back(instrument);
  }

  BenchmarkWorkload result{
      .engine_config = header.engine_config,
      .catalog = std::move(header.catalog),
      .checkpoint_interval = header.checkpoint_interval,
      .stream_digest = {},
      .commands = {},
  };
  result.commands.reserve(static_cast<std::size_t>(header.command_count));
  for (std::uint64_t index = 0U; index < header.command_count; ++index) {
    if (auto error = read_protocol_line(input, line, line_number, "missing_command", stream_hash);
        error.has_value()) {
      return *error;
    }
    auto parsed = parse_command(line);
    if (const auto* error = std::get_if<std::string_view>(&parsed)) {
      return WorkloadParseError{line_number, *error};
    }
    result.commands.push_back(BenchmarkCommand{
        .command = std::get<domain::Command>(std::move(parsed)),
        .source_line = line_number,
    });
  }

  if (input.peek() != std::char_traits<char>::eof()) {
    if (auto error =
            read_protocol_line(input, line, line_number, "unexpected_trailing_input", stream_hash);
        error.has_value()) {
      return *error;
    }
    return WorkloadParseError{line_number, "unexpected_trailing_input"};
  }
  if (input.bad() || (input.fail() && !input.eof())) {
    return WorkloadParseError{line_number + 1U, "input_read_failure"};
  }
  result.stream_digest = stream_hash.finish();
  return result;
}

}  // namespace

WorkloadParseResult read_atlas_diff_v2(std::istream& input, std::size_t maximum_catalog_count,
                                       std::size_t expected_command_count) {
  return read_atlas_diff_v2_impl(input, maximum_catalog_count, expected_command_count);
}

WorkloadParseResult read_atlas_diff_v2_declared(std::istream& input,
                                                std::size_t maximum_catalog_count) {
  return read_atlas_diff_v2_impl(input, maximum_catalog_count, std::nullopt);
}

}  // namespace atlaslob::benchmark
