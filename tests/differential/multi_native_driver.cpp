#include "multi_native_driver.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <istream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "atlaslob/digest.hpp"
#include "atlaslob/domain/commands.hpp"
#include "atlaslob/domain/events.hpp"
#include "atlaslob/multi_instrument_engine.hpp"

namespace atlaslob::differential {
namespace {

constexpr std::string_view adapter_schema{"atlas_diff_v2"};
constexpr std::string_view header_magic{"ATLAS_DIFF_V2"};

static_assert(std::numeric_limits<std::size_t>::digits <=
              std::numeric_limits<std::uint64_t>::digits);

struct DriverConfig final {
  MultiInstrumentEngineConfig engine{};
  std::vector<InstrumentConfig> catalog;
  std::uint64_t catalog_count{};
  std::uint64_t command_count{};
  std::uint64_t checkpoint_interval{};
};

struct CommandRecord final {
  domain::Command command;
  std::uint64_t line{};
};

struct ParsedInput final {
  DriverConfig config;
  std::vector<CommandRecord> commands;
};

struct HarnessError final {
  std::uint64_t line{};
  std::string_view code;
};

using ParsedStream = std::variant<ParsedInput, HarnessError>;
using ParsedCommand = std::variant<domain::Command, std::string_view>;

[[nodiscard]] bool canonical_fields(std::string_view line, std::vector<std::string_view>& output) {
  output.clear();
  if (line.empty() || line.front() == ' ' || line.back() == ' ') {
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
    if (field.find_first_of("\t\r\n\f\v") != std::string_view::npos) {
      return false;
    }
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

[[nodiscard]] std::optional<HarnessError> read_protocol_line(std::istream& input, std::string& line,
                                                             std::uint64_t& line_number,
                                                             std::string_view missing_code) {
  if (!std::getline(input, line)) {
    if (input.bad() || (input.fail() && !input.eof())) {
      return HarnessError{line_number + 1U, "input_read_failure"};
    }
    return HarnessError{line_number + 1U, missing_code};
  }
  ++line_number;
  if (line.find('\r') != std::string::npos || input.eof()) {
    return HarnessError{line_number, "invalid_line_ending"};
  }
  return std::nullopt;
}

[[nodiscard]] std::variant<DriverConfig, std::string_view> parse_header(std::string_view line) {
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

  DriverConfig config{
      .engine =
          {
              .max_total_active_orders = static_cast<std::size_t>(max_total),
          },
      .catalog = {},
      .catalog_count = catalog_count,
      .command_count = command_count,
      .checkpoint_interval = checkpoint_interval,
  };
  config.catalog.reserve(static_cast<std::size_t>(catalog_count));
  return config;
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

[[nodiscard]] ParsedStream parse_stream(std::istream& input) {
  std::uint64_t line_number = 0U;
  std::string line;
  if (auto error = read_protocol_line(input, line, line_number, "missing_header");
      error.has_value()) {
    return *error;
  }

  auto parsed_header = parse_header(line);
  if (const auto* error = std::get_if<std::string_view>(&parsed_header)) {
    return HarnessError{line_number, *error};
  }
  auto config = std::get<DriverConfig>(std::move(parsed_header));
  const auto expected_catalog_count = static_cast<std::size_t>(config.catalog_count);
  std::unordered_set<domain::InstrumentId, domain::StrongValueHash<domain::InstrumentId>>
      instrument_ids;
  instrument_ids.reserve(expected_catalog_count);

  for (std::size_t index = 0U; index < expected_catalog_count; ++index) {
    if (auto error = read_protocol_line(input, line, line_number, "missing_instrument_record");
        error.has_value()) {
      return *error;
    }
    auto parsed = parse_instrument(line);
    if (const auto* error = std::get_if<std::string_view>(&parsed)) {
      return HarnessError{line_number, *error};
    }
    auto instrument = std::get<InstrumentConfig>(parsed);
    if (!instrument_ids.insert(instrument.instrument_id).second) {
      return HarnessError{line_number, "duplicate_instrument_id"};
    }
    config.catalog.push_back(instrument);
  }
  std::sort(config.catalog.begin(), config.catalog.end(),
            [](const InstrumentConfig& left, const InstrumentConfig& right) {
              return left.instrument_id < right.instrument_id;
            });

  ParsedInput result{
      .config = std::move(config),
      .commands = {},
  };
  result.commands.reserve(static_cast<std::size_t>(result.config.command_count));
  for (std::uint64_t index = 0U; index < result.config.command_count; ++index) {
    if (auto error = read_protocol_line(input, line, line_number, "missing_command");
        error.has_value()) {
      return *error;
    }
    auto parsed = parse_command(line);
    if (const auto* error = std::get_if<std::string_view>(&parsed)) {
      return HarnessError{line_number, *error};
    }
    result.commands.push_back(CommandRecord{
        .command = std::get<domain::Command>(std::move(parsed)),
        .line = line_number,
    });
  }

  if (std::getline(input, line)) {
    ++line_number;
    return HarnessError{
        line_number,
        line.find('\r') == std::string::npos ? std::string_view{"unexpected_trailing_input"}
                                             : std::string_view{"invalid_line_ending"},
    };
  }
  if (input.bad() || (input.fail() && !input.eof())) {
    return HarnessError{line_number + 1U, "input_read_failure"};
  }
  return result;
}

template <typename Integer>
void write_decimal(std::ostream& output, Integer value) {
  static_assert(std::is_integral_v<Integer>);
  char buffer[32U]{};
  const auto [end, error] = std::to_chars(std::begin(buffer), std::end(buffer), value, 10);
  if (error != std::errc{}) {
    output.setstate(std::ios::badbit);
    return;
  }
  output.write(buffer, end - buffer);
}

template <typename Integer>
void write_quoted_decimal(std::ostream& output, Integer value) {
  output.put('"');
  write_decimal(output, value);
  output.put('"');
}

void write_top_level(std::ostream& output, const std::optional<domain::TopOfBookLevel>& level) {
  if (!level.has_value()) {
    output << "null";
    return;
  }
  output << "{\"price\":";
  write_quoted_decimal(output, level->price.value());
  output << ",\"aggregate_quantity\":";
  write_quoted_decimal(output, level->aggregate_quantity.value());
  output << '}';
}

void write_order(std::ostream& output, const OrderSnapshot& order) {
  output << "{\"order_id\":";
  write_quoted_decimal(output, order.order_id.value());
  output << ",\"client_id\":";
  write_quoted_decimal(output, order.client_id.value());
  output << ",\"instrument_id\":";
  write_quoted_decimal(output, order.instrument_id.value());
  output << ",\"side\":\"" << domain::to_string(order.side) << "\",\"price\":";
  write_quoted_decimal(output, order.price.value());
  output << ",\"remaining_quantity\":";
  write_quoted_decimal(output, order.remaining_quantity.value());
  output << ",\"priority_sequence\":";
  write_quoted_decimal(output, order.priority_sequence.value());
  output << '}';
}

void write_levels(std::ostream& output, const std::vector<PriceLevelSnapshot>& levels) {
  output.put('[');
  for (std::size_t level_index = 0U; level_index < levels.size(); ++level_index) {
    if (level_index != 0U) {
      output.put(',');
    }
    const auto& level = levels[level_index];
    output << "{\"price\":";
    write_quoted_decimal(output, level.price.value());
    output << ",\"aggregate_quantity\":";
    write_quoted_decimal(output, level.aggregate_quantity.value());
    output << ",\"orders\":[";
    for (std::size_t order_index = 0U; order_index < level.orders.size(); ++order_index) {
      if (order_index != 0U) {
        output.put(',');
      }
      write_order(output, level.orders[order_index]);
    }
    output << "]}";
  }
  output.put(']');
}

void write_instrument_config(std::ostream& output, const InstrumentConfig& config) {
  output << "{\"instrument_id\":";
  write_quoted_decimal(output, config.instrument_id.value());
  output << ",\"max_order_quantity\":";
  write_quoted_decimal(output, config.matching.max_order_quantity.value());
  output << ",\"tick_increment\":";
  write_quoted_decimal(output, config.matching.tick_increment.value());
  output << ",\"max_active_orders\":";
  write_quoted_decimal(output, static_cast<std::uint64_t>(config.matching.max_active_orders));
  output.put('}');
}

void write_catalog(std::ostream& output, const std::vector<InstrumentConfig>& catalog) {
  output.put('[');
  for (std::size_t index = 0U; index < catalog.size(); ++index) {
    if (index != 0U) {
      output.put(',');
    }
    write_instrument_config(output, catalog[index]);
  }
  output.put(']');
}

void write_instrument_snapshot(std::ostream& output, const InstrumentSnapshot& snapshot) {
  output << "{\"instrument_id\":";
  write_quoted_decimal(output, snapshot.instrument_id.value());
  output << ",\"active_order_count\":";
  write_quoted_decimal(output, snapshot.active_order_count);
  output << ",\"bids\":";
  write_levels(output, snapshot.bids);
  output << ",\"asks\":";
  write_levels(output, snapshot.asks);
  output.put('}');
}

void write_snapshot(std::ostream& output, const EngineSnapshot& snapshot) {
  output << "{\"semantics_version\":";
  write_quoted_decimal(output, snapshot.semantics_version);
  output << ",\"engine_config\":{\"max_total_active_orders\":";
  write_quoted_decimal(output,
                       static_cast<std::uint64_t>(snapshot.engine_config.max_total_active_orders));
  output << "},\"catalog\":";
  write_catalog(output, snapshot.catalog);
  output << ",\"last_sequence\":";
  write_quoted_decimal(output, snapshot.last_sequence.value());
  output << ",\"sequence_exhausted\":" << (snapshot.sequence_exhausted ? "true" : "false");
  output << ",\"active_order_count\":";
  write_quoted_decimal(output, snapshot.active_order_count);
  output << ",\"instruments\":[";
  for (std::size_t index = 0U; index < snapshot.instruments.size(); ++index) {
    if (index != 0U) {
      output.put(',');
    }
    write_instrument_snapshot(output, snapshot.instruments[index]);
  }
  output << "]}";
}

void write_event_header(std::ostream& output, const domain::Event& event) {
  const auto& header = domain::event_header(event);
  output << "{\"type\":\"" << domain::to_string(domain::event_type(event))
         << "\",\"command_sequence\":";
  write_quoted_decimal(output, header.command_sequence.value());
  output << ",\"event_index\":";
  write_quoted_decimal(output, header.event_index);
  output << ",\"instrument_id\":";
  write_quoted_decimal(output, header.instrument_id.value());
}

void write_event(std::ostream& output, const domain::Event& event) {
  write_event_header(output, event);
  std::visit(
      [&output](const auto& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, domain::AcceptedEvent>) {
          output << ",\"command_type\":\"" << domain::to_string(value.command_type) << '"';
        } else if constexpr (std::is_same_v<Value, domain::RejectedEvent>) {
          output << ",\"command_type\":\"" << domain::to_string(value.command_type)
                 << "\",\"reason\":\"" << domain::to_string(value.reason) << "\",\"order_id\":";
          if (value.order_id.has_value()) {
            write_quoted_decimal(output, value.order_id->value());
          } else {
            output << "null";
          }
        } else if constexpr (std::is_same_v<Value, domain::TradeEvent>) {
          output << ",\"aggressor_order_id\":";
          write_quoted_decimal(output, value.aggressor_order_id.value());
          output << ",\"resting_order_id\":";
          write_quoted_decimal(output, value.resting_order_id.value());
          output << ",\"aggressor_client_id\":";
          write_quoted_decimal(output, value.aggressor_client_id.value());
          output << ",\"resting_client_id\":";
          write_quoted_decimal(output, value.resting_client_id.value());
          output << ",\"aggressor_side\":\"" << domain::to_string(value.aggressor_side)
                 << "\",\"execution_price\":";
          write_quoted_decimal(output, value.execution_price.value());
          output << ",\"execution_quantity\":";
          write_quoted_decimal(output, value.execution_quantity.value());
          output << ",\"aggressor_remaining\":";
          write_quoted_decimal(output, value.aggressor_remaining.value());
          output << ",\"resting_remaining\":";
          write_quoted_decimal(output, value.resting_remaining.value());
        } else if constexpr (std::is_same_v<Value, domain::RestedEvent>) {
          output << ",\"order_id\":";
          write_quoted_decimal(output, value.order_id.value());
          output << ",\"client_id\":";
          write_quoted_decimal(output, value.client_id.value());
          output << ",\"side\":\"" << domain::to_string(value.side) << "\",\"price\":";
          write_quoted_decimal(output, value.price.value());
          output << ",\"remaining_quantity\":";
          write_quoted_decimal(output, value.remaining_quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::CanceledEvent>) {
          output << ",\"order_id\":";
          write_quoted_decimal(output, value.order_id.value());
          output << ",\"canceled_quantity\":";
          write_quoted_decimal(output, value.canceled_quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::ReplacedEvent>) {
          output << ",\"old_order_id\":";
          write_quoted_decimal(output, value.old_order_id.value());
          output << ",\"new_order_id\":";
          write_quoted_decimal(output, value.new_order_id.value());
        } else if constexpr (std::is_same_v<Value, domain::DoneEvent>) {
          output << ",\"order_id\":";
          write_quoted_decimal(output, value.order_id.value());
          output << ",\"reason\":\"" << domain::to_string(value.reason)
                 << "\",\"remaining_quantity\":";
          write_quoted_decimal(output, value.remaining_quantity.value());
        } else {
          static_assert(std::is_same_v<Value, domain::BookChangedEvent>);
          output << ",\"best_bid\":";
          write_top_level(output, value.best_bid);
          output << ",\"best_ask\":";
          write_top_level(output, value.best_ask);
        }
      },
      event);
  output.put('}');
}

void write_events(std::ostream& output, const domain::EventBatch& batch) {
  output.put('[');
  for (std::size_t index = 0U; index < batch.size(); ++index) {
    if (index != 0U) {
      output.put(',');
    }
    write_event(output, batch[index]);
  }
  output.put(']');
}

[[nodiscard]] constexpr std::string_view mode_name(MultiOutputMode mode) noexcept {
  return mode == MultiOutputMode::exact ? std::string_view{"exact"} : std::string_view{"compact"};
}

void write_error(std::ostream& output, const HarnessError& error) {
  output << "{\"schema\":\"" << adapter_schema << "\",\"kind\":\"error\",\"line\":";
  write_quoted_decimal(output, error.line);
  output << ",\"code\":\"" << error.code << "\"}\n";
}

void write_config(std::ostream& output, const DriverConfig& config, MultiOutputMode mode) {
  output << "{\"schema\":\"" << adapter_schema << "\",\"kind\":\"config\",\"mode\":\""
         << mode_name(mode) << "\",\"semantics_version\":";
  write_quoted_decimal(output, atlaslob_semantics_version);
  output << ",\"max_total_active_orders\":";
  write_quoted_decimal(output, static_cast<std::uint64_t>(config.engine.max_total_active_orders));
  output << ",\"catalog_count\":";
  write_quoted_decimal(output, config.catalog_count);
  output << ",\"catalog\":";
  write_catalog(output, config.catalog);
  output << ",\"command_count\":";
  write_quoted_decimal(output, config.command_count);
  output << ",\"checkpoint_interval\":";
  write_quoted_decimal(output, config.checkpoint_interval);
  output << "}\n";
}

[[nodiscard]] bool is_checkpoint(std::uint64_t command_index, std::uint64_t interval) noexcept {
  return interval != 0U && (command_index + 1U) % interval == 0U;
}

void write_result(std::ostream& output, const MultiInstrumentEngine& engine,
                  const CommandRecord& record, const EngineResult& result,
                  std::uint64_t command_index, MultiOutputMode mode, bool checkpoint) {
  output << "{\"schema\":\"" << adapter_schema << "\",\"kind\":\"result\",\"command_index\":";
  write_quoted_decimal(output, command_index);
  output << ",\"line\":";
  write_quoted_decimal(output, record.line);
  output << ",\"command_type\":\"" << domain::to_string(domain::command_type(record.command))
         << "\",\"outcome\":\"";

  const auto* const batch = result.batch();
  if (batch == nullptr) {
    output << "engine_error\",\"command_sequence\":null,\"engine_error\":\""
           << to_string(result.error())
           << "\",\"reject_reason\":null,\"event_digest\":null,\"events\":null";
  } else {
    output << (result.committed() ? "committed" : "rejected") << "\",\"command_sequence\":";
    write_quoted_decimal(output, batch->command_sequence().value());
    output << ",\"engine_error\":null,\"reject_reason\":";
    if (result.rejected()) {
      output.put('"');
      output << domain::to_string(std::get<domain::RejectedEvent>((*batch)[0]).reason);
      output.put('"');
    } else {
      output << "null";
    }
    output << ",\"event_digest\":\"" << event_digest(*batch).hex() << "\",\"events\":";
    if (mode == MultiOutputMode::exact) {
      write_events(output, *batch);
    } else {
      output << "null";
    }
  }

  output << ",\"post_state_digest\":\"" << engine.state_digest().hex()
         << "\",\"checkpoint_snapshot\":";
  if (checkpoint) {
    write_snapshot(output, engine.snapshot());
  } else {
    output << "null";
  }
  output << "}\n";
}

void write_final(std::ostream& output, const MultiInstrumentEngine& engine,
                 const DriverConfig& config, std::uint64_t commands_processed,
                 std::uint64_t committed, std::uint64_t rejected, std::uint64_t engine_errors) {
  const auto snapshot = engine.snapshot();
  output << "{\"schema\":\"" << adapter_schema << "\",\"kind\":\"final\",\"commands_declared\":";
  write_quoted_decimal(output, config.command_count);
  output << ",\"commands_processed\":";
  write_quoted_decimal(output, commands_processed);
  output << ",\"committed\":";
  write_quoted_decimal(output, committed);
  output << ",\"rejected\":";
  write_quoted_decimal(output, rejected);
  output << ",\"engine_errors\":";
  write_quoted_decimal(output, engine_errors);
  output << ",\"final_state_digest\":\"" << state_digest(snapshot).hex() << "\",\"snapshot\":";
  write_snapshot(output, snapshot);
  output << "}\n";
}

[[nodiscard]] int finish_output(std::ostream& output, int intended_exit_code) {
  output.flush();
  return output ? intended_exit_code : multi_native_driver_engine_error_exit_code;
}

[[nodiscard]] int run_impl(std::istream& input, std::ostream& output, MultiOutputMode mode) {
  ParsedStream parsed;
  try {
    parsed = parse_stream(input);
  } catch (const std::bad_alloc&) {
    write_error(output, HarnessError{0U, "resource_failure"});
    return finish_output(output, multi_native_driver_engine_error_exit_code);
  } catch (...) {
    write_error(output, HarnessError{0U, "adapter_exception"});
    return finish_output(output, multi_native_driver_engine_error_exit_code);
  }
  if (const auto* error = std::get_if<HarnessError>(&parsed)) {
    write_error(output, *error);
    return finish_output(output, multi_native_driver_input_error_exit_code);
  }
  auto stream = std::get<ParsedInput>(std::move(parsed));

  std::unique_ptr<MultiInstrumentEngine> engine;
  try {
    engine = std::make_unique<MultiInstrumentEngine>(
        std::span<const InstrumentConfig>{stream.config.catalog}, stream.config.engine);
  } catch (const std::bad_alloc&) {
    write_error(output, HarnessError{1U, "resource_failure"});
    return finish_output(output, multi_native_driver_engine_error_exit_code);
  } catch (const std::invalid_argument&) {
    write_error(output, HarnessError{1U, "invalid_engine_config"});
    return finish_output(output, multi_native_driver_input_error_exit_code);
  } catch (...) {
    write_error(output, HarnessError{1U, "engine_construction_failure"});
    return finish_output(output, multi_native_driver_engine_error_exit_code);
  }

  write_config(output, stream.config, mode);
  if (!output) {
    return finish_output(output, multi_native_driver_engine_error_exit_code);
  }

  std::uint64_t commands_processed = 0U;
  std::uint64_t committed = 0U;
  std::uint64_t rejected = 0U;
  std::uint64_t engine_errors = 0U;
  for (const auto& record : stream.commands) {
    std::optional<EngineResult> result;
    try {
      result.emplace(engine->execute(record.command));
    } catch (const std::bad_alloc&) {
      write_error(output, HarnessError{record.line, "resource_failure"});
      return finish_output(output, multi_native_driver_engine_error_exit_code);
    } catch (...) {
      write_error(output, HarnessError{record.line, "engine_exception"});
      return finish_output(output, multi_native_driver_engine_error_exit_code);
    }

    if (result->committed()) {
      ++committed;
    } else if (result->rejected()) {
      ++rejected;
    } else {
      ++engine_errors;
    }
    const auto command_index = commands_processed;
    ++commands_processed;
    write_result(output, *engine, record, *result, command_index, mode,
                 is_checkpoint(command_index, stream.config.checkpoint_interval));
    if (!output) {
      return finish_output(output, multi_native_driver_engine_error_exit_code);
    }
    if (!result->has_value()) {
      write_final(output, *engine, stream.config, commands_processed, committed, rejected,
                  engine_errors);
      return finish_output(output, multi_native_driver_engine_error_exit_code);
    }
  }

  write_final(output, *engine, stream.config, commands_processed, committed, rejected,
              engine_errors);
  return finish_output(output, multi_native_driver_success_exit_code);
}

}  // namespace

int run_multi_native_driver(std::istream& input, std::ostream& output, MultiOutputMode mode) {
  try {
    return run_impl(input, output, mode);
  } catch (...) {
    // A late exception can occur after a JSON record has started. Preserve the
    // partial evidence and report failure only through the process status.
    return multi_native_driver_engine_error_exit_code;
  }
}

}  // namespace atlaslob::differential

#ifndef ATLAS_DIFF_MULTI_NATIVE_NO_MAIN
namespace {

#if defined(_WIN32)
[[nodiscard]] bool configure_binary_standard_io() noexcept {
  return _setmode(_fileno(stdin), _O_BINARY) != -1 && _setmode(_fileno(stdout), _O_BINARY) != -1;
}
#else
[[nodiscard]] constexpr bool configure_binary_standard_io() noexcept { return true; }
#endif

[[nodiscard]] std::optional<atlaslob::differential::MultiOutputMode> parse_mode(int argc,
                                                                                char** argv) {
  if (argc == 1) {
    return atlaslob::differential::MultiOutputMode::exact;
  }
  if (argc == 2) {
    const std::string_view argument{argv[1]};
    if (argument == "exact" || argument == "--mode=exact") {
      return atlaslob::differential::MultiOutputMode::exact;
    }
    if (argument == "compact" || argument == "--mode=compact") {
      return atlaslob::differential::MultiOutputMode::compact;
    }
  }
  if (argc == 3 && std::string_view{argv[1]} == "--mode") {
    const std::string_view argument{argv[2]};
    if (argument == "exact") {
      return atlaslob::differential::MultiOutputMode::exact;
    }
    if (argument == "compact") {
      return atlaslob::differential::MultiOutputMode::compact;
    }
  }
  return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
  if (!configure_binary_standard_io()) {
    std::cerr << "atlas_diff_multi_native: failed to configure binary standard I/O\n";
    return atlaslob::differential::multi_native_driver_engine_error_exit_code;
  }
  const auto mode = parse_mode(argc, argv);
  if (!mode.has_value()) {
    std::cerr << "usage: atlas_diff_multi_native [exact|compact]\n";
    return atlaslob::differential::multi_native_driver_input_error_exit_code;
  }
  return atlaslob::differential::run_multi_native_driver(std::cin, std::cout, *mode);
}
#endif
