#include "native_driver.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <istream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "atlaslob/book_snapshot.hpp"
#include "atlaslob/digest.hpp"
#include "atlaslob/domain/commands.hpp"
#include "atlaslob/domain/events.hpp"
#include "atlaslob/matching_engine.hpp"

namespace atlaslob::differential {
namespace {

constexpr std::string_view adapter_schema{"atlas_diff_v1"};
constexpr std::string_view header_magic{"ATLAS_DIFF_V1"};

struct DriverConfig final {
  domain::InstrumentId instrument_id{};
  MatchingEngineConfig engine{};
  std::uint64_t snapshot_interval{};
};

struct HarnessError final {
  std::string_view code;
};

enum class ConstructionFailureInjection {
  none,
  allocation,
  exception,
};

using ParsedHeader = std::variant<DriverConfig, HarnessError>;
using ParsedCommand = std::variant<domain::Command, HarnessError>;

class DecimalAccumulator final {
 public:
  template <typename Integer>
  void add(Integer value) {
    static_assert(std::is_integral_v<Integer>);
    static_assert(std::is_unsigned_v<Integer>);

    std::size_t index = 0U;
    std::uint32_t carry = 0U;
    while (value != 0U || carry != 0U) {
      std::uint32_t chunk = 0U;
      if constexpr (std::numeric_limits<Integer>::digits < 30) {
        chunk = static_cast<std::uint32_t>(value);
        value = 0U;
      } else {
        chunk = static_cast<std::uint32_t>(value % static_cast<Integer>(decimal_base));
        value /= static_cast<Integer>(decimal_base);
      }
      if (index == limbs_.size()) {
        limbs_.push_back(0U);
      }
      const auto sum = static_cast<std::uint64_t>(limbs_[index]) + chunk + carry;
      limbs_[index] = static_cast<std::uint32_t>(sum % decimal_base);
      carry = static_cast<std::uint32_t>(sum / decimal_base);
      ++index;
    }
  }

  void write(std::ostream& output) const {
    if (limbs_.empty()) {
      output.put('0');
      return;
    }

    write_decimal_limb(output, limbs_.back(), false);
    for (auto index = limbs_.size() - 1U; index != 0U; --index) {
      write_decimal_limb(output, limbs_[index - 1U], true);
    }
  }

 private:
  static constexpr std::uint32_t decimal_base = 1'000'000'000U;
  static constexpr std::size_t limb_width = 9U;

  static void write_decimal_limb(std::ostream& output, std::uint32_t value, bool pad) {
    char buffer[limb_width]{};
    const auto [end, error] = std::to_chars(std::begin(buffer), std::end(buffer), value, 10);
    if (error != std::errc{}) {
      output.setstate(std::ios::badbit);
      return;
    }
    const auto width = static_cast<std::size_t>(end - std::begin(buffer));
    if (pad) {
      for (auto count = width; count < limb_width; ++count) {
        output.put('0');
      }
    }
    output.write(buffer, static_cast<std::streamsize>(width));
  }

  std::vector<std::uint32_t> limbs_;
};

struct SideSummary final {
  DecimalAccumulator level_count;
  DecimalAccumulator order_count;
  DecimalAccumulator aggregate_quantity;
};

struct StateEvidence final {
  BookSnapshot snapshot;
  BookTop top;
  std::uint64_t active_order_count{};
  bool empty{};
  domain::Sequence next_sequence{};
  bool sequence_exhausted{};
  SideSummary bids;
  SideSummary asks;
  Digest256 digest;
};

[[nodiscard]] constexpr bool is_space(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' ||
         value == '\v';
}

[[nodiscard]] std::vector<std::string_view> tokens(std::string_view line) {
  std::vector<std::string_view> result;
  std::size_t cursor = 0U;
  while (cursor < line.size()) {
    while (cursor < line.size() && is_space(line[cursor])) {
      ++cursor;
    }
    if (cursor == line.size()) {
      break;
    }
    const auto begin = cursor;
    while (cursor < line.size() && !is_space(line[cursor])) {
      ++cursor;
    }
    result.push_back(line.substr(begin, cursor - begin));
  }
  return result;
}

[[nodiscard]] bool ignored_line(std::string_view line) noexcept {
  std::size_t cursor = 0U;
  while (cursor < line.size() && is_space(line[cursor])) {
    ++cursor;
  }
  return cursor == line.size() || line[cursor] == '#';
}

template <typename Integer>
[[nodiscard]] bool parse_integer(std::string_view token, Integer& destination) noexcept {
  static_assert(std::is_integral_v<Integer>);
  Integer value{};
  const auto* const begin = token.data();
  const auto* const end = begin + token.size();
  const auto [parsed_end, error] = std::from_chars(begin, end, value, 10);
  if (token.empty() || error != std::errc{} || parsed_end != end) {
    return false;
  }
  destination = value;
  return true;
}

[[nodiscard]] bool parse_u8(std::string_view token, std::uint8_t& destination) noexcept {
  std::uint16_t value{};
  if (!parse_integer(token, value) ||
      value > static_cast<std::uint16_t>(std::numeric_limits<std::uint8_t>::max())) {
    return false;
  }
  destination = static_cast<std::uint8_t>(value);
  return true;
}

[[nodiscard]] ParsedHeader parse_header(std::string_view line) {
  const auto fields = tokens(line);
  if (fields.size() != 6U) {
    return HarnessError{"invalid_header_field_count"};
  }
  if (fields[0] != header_magic) {
    return HarnessError{"unsupported_header"};
  }

  std::uint32_t instrument{};
  std::uint64_t max_quantity{};
  std::int64_t tick_increment{};
  std::uint64_t max_active_orders{};
  std::uint64_t snapshot_interval{};
  if (!parse_integer(fields[1], instrument)) {
    return HarnessError{"invalid_header_instrument"};
  }
  if (!parse_integer(fields[2], max_quantity)) {
    return HarnessError{"invalid_header_max_quantity"};
  }
  if (!parse_integer(fields[3], tick_increment)) {
    return HarnessError{"invalid_header_tick_increment"};
  }
  if (!parse_integer(fields[4], max_active_orders) ||
      max_active_orders > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return HarnessError{"invalid_header_max_active_orders"};
  }
  if (!parse_integer(fields[5], snapshot_interval)) {
    return HarnessError{"invalid_header_snapshot_interval"};
  }

  DriverConfig config{
      .instrument_id = domain::InstrumentId{instrument},
      .engine =
          {
              .max_order_quantity = domain::Quantity{max_quantity},
              .tick_increment = domain::PriceTicks{tick_increment},
              .max_active_orders = static_cast<std::size_t>(max_active_orders),
          },
      .snapshot_interval = snapshot_interval,
  };
  if (config.instrument_id.value() == 0U || !config.engine.valid()) {
    return HarnessError{"invalid_engine_config"};
  }
  return config;
}

[[nodiscard]] ParsedCommand parse_new(const std::vector<std::string_view>& fields) {
  if (fields.size() != 10U) {
    return HarnessError{"invalid_new_field_count"};
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
  if (!parse_integer(fields[1], client)) {
    return HarnessError{"invalid_new_client"};
  }
  if (!parse_integer(fields[2], order)) {
    return HarnessError{"invalid_new_order"};
  }
  if (!parse_integer(fields[3], instrument)) {
    return HarnessError{"invalid_new_instrument"};
  }
  if (!parse_u8(fields[4], side)) {
    return HarnessError{"invalid_new_side_code"};
  }
  if (!parse_u8(fields[5], order_type)) {
    return HarnessError{"invalid_new_order_type_code"};
  }
  if (!parse_u8(fields[6], time_in_force)) {
    return HarnessError{"invalid_new_time_in_force_code"};
  }
  if (!parse_u8(fields[7], price_present) || price_present > 1U) {
    return HarnessError{"invalid_new_price_presence"};
  }
  if (!parse_integer(fields[8], price)) {
    return HarnessError{"invalid_new_price"};
  }
  if (price_present == 0U && price != 0) {
    return HarnessError{"nonzero_absent_price_placeholder"};
  }
  if (!parse_integer(fields[9], quantity)) {
    return HarnessError{"invalid_new_quantity"};
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
    return HarnessError{"invalid_cancel_field_count"};
  }

  std::uint32_t client{};
  std::uint64_t order{};
  std::uint32_t instrument{};
  if (!parse_integer(fields[1], client)) {
    return HarnessError{"invalid_cancel_client"};
  }
  if (!parse_integer(fields[2], order)) {
    return HarnessError{"invalid_cancel_order"};
  }
  if (!parse_integer(fields[3], instrument)) {
    return HarnessError{"invalid_cancel_instrument"};
  }
  return domain::Command{domain::CancelOrder{
      .client_id = domain::ClientId{client},
      .order_id = domain::OrderId{order},
      .instrument_id = domain::InstrumentId{instrument},
  }};
}

[[nodiscard]] ParsedCommand parse_replace(const std::vector<std::string_view>& fields) {
  if (fields.size() != 7U) {
    return HarnessError{"invalid_replace_field_count"};
  }

  std::uint32_t client{};
  std::uint64_t old_order{};
  std::uint64_t new_order{};
  std::uint32_t instrument{};
  std::int64_t price{};
  std::uint64_t quantity{};
  if (!parse_integer(fields[1], client)) {
    return HarnessError{"invalid_replace_client"};
  }
  if (!parse_integer(fields[2], old_order)) {
    return HarnessError{"invalid_replace_old_order"};
  }
  if (!parse_integer(fields[3], new_order)) {
    return HarnessError{"invalid_replace_new_order"};
  }
  if (!parse_integer(fields[4], instrument)) {
    return HarnessError{"invalid_replace_instrument"};
  }
  if (!parse_integer(fields[5], price)) {
    return HarnessError{"invalid_replace_price"};
  }
  if (!parse_integer(fields[6], quantity)) {
    return HarnessError{"invalid_replace_quantity"};
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
  const auto fields = tokens(line);
  if (fields.empty()) {
    return HarnessError{"empty_command"};
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
  return HarnessError{"unknown_command"};
}

[[nodiscard]] std::string_view mode_name(OutputMode mode) noexcept {
  return mode == OutputMode::exact ? std::string_view{"exact"} : std::string_view{"compact"};
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

void write_quoted_u64(std::ostream& output, std::uint64_t value) {
  output.put('"');
  write_decimal(output, value);
  output.put('"');
}

void write_quoted_i64(std::ostream& output, std::int64_t value) {
  output.put('"');
  write_decimal(output, value);
  output.put('"');
}

void write_quoted_u32(std::ostream& output, std::uint32_t value) {
  output.put('"');
  write_decimal(output, value);
  output.put('"');
}

void write_quoted_decimal(std::ostream& output, const DecimalAccumulator& value) {
  output.put('"');
  value.write(output);
  output.put('"');
}

[[nodiscard]] SideSummary summarize_side(const std::vector<PriceLevelSnapshot>& levels) {
  SideSummary summary;
  summary.level_count.add(levels.size());
  for (const auto& level : levels) {
    summary.order_count.add(level.orders.size());
    summary.aggregate_quantity.add(level.aggregate_quantity.value());
  }
  return summary;
}

[[nodiscard]] BookTop snapshot_top(const BookSnapshot& snapshot) {
  BookTop top;
  if (!snapshot.bids.empty()) {
    top.best_bid = domain::TopOfBookLevel{
        .price = snapshot.bids.front().price,
        .aggregate_quantity = snapshot.bids.front().aggregate_quantity,
    };
  }
  if (!snapshot.asks.empty()) {
    top.best_ask = domain::TopOfBookLevel{
        .price = snapshot.asks.front().price,
        .aggregate_quantity = snapshot.asks.front().aggregate_quantity,
    };
  }
  return top;
}

[[nodiscard]] StateEvidence capture_state(const MatchingEngine& engine) {
  auto snapshot = engine.snapshot();
  const auto top = engine.top();
  const auto active_order_count = engine.active_order_count();
  const auto empty = engine.empty();
  const auto next_sequence = engine.next_sequence();
  const auto sequence_exhausted = engine.sequence_exhausted();

  if constexpr (std::numeric_limits<std::size_t>::digits >
                std::numeric_limits<std::uint64_t>::digits) {
    if (active_order_count > std::numeric_limits<std::uint64_t>::max()) {
      throw std::logic_error{"native evidence active count exceeds its snapshot representation"};
    }
  }
  if (static_cast<std::uint64_t>(active_order_count) != snapshot.active_order_count ||
      empty != (snapshot.active_order_count == 0U) || top != snapshot_top(snapshot) ||
      sequence_exhausted != snapshot.sequence_exhausted) {
    throw std::logic_error{"native evidence observers differ from the public snapshot"};
  }
  if (sequence_exhausted) {
    if (next_sequence.value() != 0U) {
      throw std::logic_error{"exhausted native evidence has a nonzero next sequence"};
    }
  } else if (snapshot.last_sequence.value() == std::numeric_limits<std::uint64_t>::max() ||
             next_sequence.value() != snapshot.last_sequence.value() + 1U) {
    throw std::logic_error{"native evidence sequence observers differ from the public snapshot"};
  }

  auto bids = summarize_side(snapshot.bids);
  auto asks = summarize_side(snapshot.asks);
  const auto digest = state_digest(snapshot);
  return StateEvidence{
      .snapshot = std::move(snapshot),
      .top = top,
      .active_order_count = static_cast<std::uint64_t>(active_order_count),
      .empty = empty,
      .next_sequence = next_sequence,
      .sequence_exhausted = sequence_exhausted,
      .bids = std::move(bids),
      .asks = std::move(asks),
      .digest = digest,
  };
}

void write_top_level(std::ostream& output, const std::optional<domain::TopOfBookLevel>& level) {
  if (!level.has_value()) {
    output << "null";
    return;
  }
  output << "{\"price\":";
  write_quoted_i64(output, level->price.value());
  output << ",\"aggregate_quantity\":";
  write_quoted_u64(output, level->aggregate_quantity.value());
  output << '}';
}

void write_order_snapshot(std::ostream& output, const OrderSnapshot& order) {
  output << "{\"order_id\":";
  write_quoted_u64(output, order.order_id.value());
  output << ",\"client_id\":";
  write_quoted_u32(output, order.client_id.value());
  output << ",\"instrument_id\":";
  write_quoted_u32(output, order.instrument_id.value());
  output << ",\"side\":\"" << domain::to_string(order.side) << "\",\"price\":";
  write_quoted_i64(output, order.price.value());
  output << ",\"remaining_quantity\":";
  write_quoted_u64(output, order.remaining_quantity.value());
  output << ",\"priority_sequence\":";
  write_quoted_u64(output, order.priority_sequence.value());
  output << '}';
}

void write_levels(std::ostream& output, const std::vector<PriceLevelSnapshot>& levels) {
  output << '[';
  for (std::size_t level_index = 0U; level_index < levels.size(); ++level_index) {
    if (level_index != 0U) {
      output << ',';
    }
    const auto& level = levels[level_index];
    output << "{\"price\":";
    write_quoted_i64(output, level.price.value());
    output << ",\"aggregate_quantity\":";
    write_quoted_u64(output, level.aggregate_quantity.value());
    output << ",\"orders\":[";
    for (std::size_t order_index = 0U; order_index < level.orders.size(); ++order_index) {
      if (order_index != 0U) {
        output << ',';
      }
      write_order_snapshot(output, level.orders[order_index]);
    }
    output << "]}";
  }
  output << ']';
}

void write_snapshot(std::ostream& output, const BookSnapshot& snapshot) {
  output << "{\"semantics_version\":";
  write_decimal(output, snapshot.semantics_version);
  output << ",\"instrument_id\":";
  write_quoted_u32(output, snapshot.instrument_id.value());
  output << ",\"last_sequence\":";
  write_quoted_u64(output, snapshot.last_sequence.value());
  output << ",\"sequence_exhausted\":" << (snapshot.sequence_exhausted ? "true" : "false")
         << ",\"active_order_count\":";
  write_quoted_u64(output, snapshot.active_order_count);
  output << ",\"bids\":";
  write_levels(output, snapshot.bids);
  output << ",\"asks\":";
  write_levels(output, snapshot.asks);
  output << '}';
}

void write_event_header(std::ostream& output, const domain::Event& event) {
  const auto& header = domain::event_header(event);
  output << "{\"type\":\"" << domain::to_string(domain::event_type(event))
         << "\",\"command_sequence\":";
  write_quoted_u64(output, header.command_sequence.value());
  output << ",\"event_index\":";
  write_decimal(output, header.event_index);
  output << ",\"instrument_id\":";
  write_quoted_u32(output, header.instrument_id.value());
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
            write_quoted_u64(output, value.order_id->value());
          } else {
            output << "null";
          }
        } else if constexpr (std::is_same_v<Value, domain::TradeEvent>) {
          output << ",\"aggressor_order_id\":";
          write_quoted_u64(output, value.aggressor_order_id.value());
          output << ",\"resting_order_id\":";
          write_quoted_u64(output, value.resting_order_id.value());
          output << ",\"aggressor_client_id\":";
          write_quoted_u32(output, value.aggressor_client_id.value());
          output << ",\"resting_client_id\":";
          write_quoted_u32(output, value.resting_client_id.value());
          output << ",\"aggressor_side\":\"" << domain::to_string(value.aggressor_side)
                 << "\",\"execution_price\":";
          write_quoted_i64(output, value.execution_price.value());
          output << ",\"execution_quantity\":";
          write_quoted_u64(output, value.execution_quantity.value());
          output << ",\"aggressor_remaining\":";
          write_quoted_u64(output, value.aggressor_remaining.value());
          output << ",\"resting_remaining\":";
          write_quoted_u64(output, value.resting_remaining.value());
        } else if constexpr (std::is_same_v<Value, domain::RestedEvent>) {
          output << ",\"order_id\":";
          write_quoted_u64(output, value.order_id.value());
          output << ",\"client_id\":";
          write_quoted_u32(output, value.client_id.value());
          output << ",\"side\":\"" << domain::to_string(value.side) << "\",\"price\":";
          write_quoted_i64(output, value.price.value());
          output << ",\"remaining_quantity\":";
          write_quoted_u64(output, value.remaining_quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::CanceledEvent>) {
          output << ",\"order_id\":";
          write_quoted_u64(output, value.order_id.value());
          output << ",\"canceled_quantity\":";
          write_quoted_u64(output, value.canceled_quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::ReplacedEvent>) {
          output << ",\"old_order_id\":";
          write_quoted_u64(output, value.old_order_id.value());
          output << ",\"new_order_id\":";
          write_quoted_u64(output, value.new_order_id.value());
        } else if constexpr (std::is_same_v<Value, domain::DoneEvent>) {
          output << ",\"order_id\":";
          write_quoted_u64(output, value.order_id.value());
          output << ",\"reason\":\"" << domain::to_string(value.reason)
                 << "\",\"remaining_quantity\":";
          write_quoted_u64(output, value.remaining_quantity.value());
        } else {
          static_assert(std::is_same_v<Value, domain::BookChangedEvent>);
          output << ",\"best_bid\":";
          write_top_level(output, value.best_bid);
          output << ",\"best_ask\":";
          write_top_level(output, value.best_ask);
        }
      },
      event);
  output << '}';
}

void write_events(std::ostream& output, const domain::EventBatch& batch) {
  output << '[';
  for (std::size_t index = 0U; index < batch.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    write_event(output, batch[index]);
  }
  output << ']';
}

void write_state(std::ostream& output, const StateEvidence& state) {
  output << "{\"active_order_count\":";
  write_quoted_u64(output, state.active_order_count);
  output << ",\"bid_level_count\":";
  write_quoted_decimal(output, state.bids.level_count);
  output << ",\"bid_order_count\":";
  write_quoted_decimal(output, state.bids.order_count);
  output << ",\"bid_aggregate_quantity\":";
  write_quoted_decimal(output, state.bids.aggregate_quantity);
  output << ",\"ask_level_count\":";
  write_quoted_decimal(output, state.asks.level_count);
  output << ",\"ask_order_count\":";
  write_quoted_decimal(output, state.asks.order_count);
  output << ",\"ask_aggregate_quantity\":";
  write_quoted_decimal(output, state.asks.aggregate_quantity);
  output << ",\"empty\":" << (state.empty ? "true" : "false") << ",\"next_sequence\":";
  write_quoted_u64(output, state.next_sequence.value());
  output << ",\"sequence_exhausted\":" << (state.sequence_exhausted ? "true" : "false")
         << ",\"best_bid\":";
  write_top_level(output, state.top.best_bid);
  output << ",\"best_ask\":";
  write_top_level(output, state.top.best_ask);
  output << ",\"state_digest\":\"" << state.digest.hex() << "\"}";
}

void write_config(std::ostream& output, const DriverConfig& config, OutputMode mode) {
  output << "{\"schema\":\"" << adapter_schema << "\",\"kind\":\"config\",\"mode\":\""
         << mode_name(mode) << "\",\"semantics_version\":";
  write_decimal(output, atlaslob_semantics_version);
  output << ",\"instrument_id\":";
  write_quoted_u32(output, config.instrument_id.value());
  output << ",\"max_order_quantity\":";
  write_quoted_u64(output, config.engine.max_order_quantity.value());
  output << ",\"tick_increment\":";
  write_quoted_i64(output, config.engine.tick_increment.value());
  output << ",\"max_active_orders\":";
  write_quoted_u64(output, static_cast<std::uint64_t>(config.engine.max_active_orders));
  output << ",\"snapshot_interval\":";
  write_quoted_u64(output, config.snapshot_interval);
  output << "}\n";
}

void write_harness_error(std::ostream& output, std::uint64_t line, std::string_view code) {
  output << "{\"schema\":\"" << adapter_schema << "\",\"kind\":\"error\",\"line\":";
  write_quoted_u64(output, line);
  output << ",\"code\":\"" << code << "\"}\n";
}

[[nodiscard]] std::string_view command_name(const domain::Command& command) noexcept {
  return domain::to_string(domain::command_type(command));
}

void write_result(std::ostream& output, const MatchingEngine& engine,
                  const domain::Command& command, const EngineResult& result,
                  std::uint64_t command_index, std::uint64_t line, OutputMode mode,
                  bool checkpoint) {
  const auto state = capture_state(engine);
  output << "{\"schema\":\"" << adapter_schema << "\",\"kind\":\"result\",\"command_index\":";
  write_quoted_u64(output, command_index);
  output << ",\"line\":";
  write_quoted_u64(output, line);
  output << ",\"command_type\":\"" << command_name(command) << "\",\"outcome\":\"";

  const auto* const batch = result.batch();
  if (batch == nullptr) {
    output << "engine_error\",\"command_sequence\":null,\"engine_error\":\""
           << to_string(result.error())
           << "\",\"reject_reason\":null,\"event_digest\":null,\"events\":null,\"state\":";
  } else {
    output << (result.committed() ? "committed" : "rejected") << "\",\"command_sequence\":";
    write_quoted_u64(output, batch->command_sequence().value());
    output << ",\"engine_error\":null,\"reject_reason\":";
    if (result.rejected()) {
      output << '"' << domain::to_string(std::get<domain::RejectedEvent>((*batch)[0]).reason)
             << '"';
    } else {
      output << "null";
    }
    output << ",\"event_digest\":\"" << event_digest(*batch).hex() << "\",\"events\":";
    if (mode == OutputMode::exact) {
      write_events(output, *batch);
    } else {
      output << "null";
    }
    output << ",\"state\":";
  }
  write_state(output, state);
  output << ",\"snapshot\":";
  if (checkpoint) {
    write_snapshot(output, state.snapshot);
  } else {
    output << "null";
  }
  output << "}\n";
}

void write_final(std::ostream& output, const MatchingEngine& engine,
                 std::uint64_t commands_processed) {
  const auto state = capture_state(engine);
  output << "{\"schema\":\"" << adapter_schema << "\",\"kind\":\"final\",\"commands_processed\":";
  write_quoted_u64(output, commands_processed);
  output << ",\"state\":";
  write_state(output, state);
  output << ",\"snapshot\":";
  write_snapshot(output, state.snapshot);
  output << "}\n";
}

[[nodiscard]] bool is_checkpoint(std::uint64_t command_index, std::uint64_t interval) noexcept {
  return interval != 0U && (command_index + 1U) % interval == 0U;
}

[[nodiscard]] int finish_terminal_record(std::ostream& output, int intended_exit_code) {
  output.flush();
  return output ? intended_exit_code : native_driver_engine_error_exit_code;
}

[[nodiscard]] std::unique_ptr<MatchingEngine> construct_engine(
    const DriverConfig& config, ConstructionFailureInjection failure) {
  if (failure == ConstructionFailureInjection::allocation) {
    throw std::bad_alloc{};
  }
  if (failure == ConstructionFailureInjection::exception) {
    throw std::runtime_error{"injected native driver construction failure"};
  }
  return std::make_unique<MatchingEngine>(config.instrument_id, config.engine);
}

[[nodiscard]] int run_native_driver_impl(std::istream& input, std::ostream& output, OutputMode mode,
                                         ConstructionFailureInjection construction_failure) {
  std::uint64_t line_number = 0U;
  std::string line;
  std::optional<DriverConfig> config;

  while (std::getline(input, line)) {
    ++line_number;
    if (ignored_line(line)) {
      continue;
    }
    ParsedHeader parsed;
    try {
      parsed = parse_header(line);
    } catch (const std::bad_alloc&) {
      write_harness_error(output, line_number, "resource_failure");
      return finish_terminal_record(output, native_driver_engine_error_exit_code);
    } catch (...) {
      write_harness_error(output, line_number, "adapter_exception");
      return finish_terminal_record(output, native_driver_engine_error_exit_code);
    }
    if (const auto* error = std::get_if<HarnessError>(&parsed)) {
      write_harness_error(output, line_number, error->code);
      return finish_terminal_record(output, native_driver_input_error_exit_code);
    }
    config = std::get<DriverConfig>(parsed);
    break;
  }

  if (!config.has_value()) {
    write_harness_error(
        output, line_number + 1U,
        input.bad() ? std::string_view{"input_read_failure"} : std::string_view{"missing_header"});
    return finish_terminal_record(output, native_driver_input_error_exit_code);
  }

  std::unique_ptr<MatchingEngine> engine;
  try {
    engine = construct_engine(*config, construction_failure);
  } catch (const std::bad_alloc&) {
    write_harness_error(output, line_number, "resource_failure");
    return finish_terminal_record(output, native_driver_engine_error_exit_code);
  } catch (const std::invalid_argument&) {
    write_harness_error(output, line_number, "invalid_engine_config");
    return finish_terminal_record(output, native_driver_input_error_exit_code);
  } catch (const std::exception&) {
    write_harness_error(output, line_number, "engine_construction_failure");
    return finish_terminal_record(output, native_driver_engine_error_exit_code);
  }
  write_config(output, *config, mode);
  if (!output) {
    return finish_terminal_record(output, native_driver_engine_error_exit_code);
  }

  std::uint64_t command_index = 0U;
  while (std::getline(input, line)) {
    ++line_number;
    if (ignored_line(line)) {
      continue;
    }

    ParsedCommand parsed;
    try {
      parsed = parse_command(line);
    } catch (const std::bad_alloc&) {
      write_harness_error(output, line_number, "resource_failure");
      return finish_terminal_record(output, native_driver_engine_error_exit_code);
    } catch (...) {
      write_harness_error(output, line_number, "adapter_exception");
      return finish_terminal_record(output, native_driver_engine_error_exit_code);
    }
    if (const auto* error = std::get_if<HarnessError>(&parsed)) {
      write_harness_error(output, line_number, error->code);
      return finish_terminal_record(output, native_driver_input_error_exit_code);
    }
    auto command = std::get<domain::Command>(std::move(parsed));

    std::optional<EngineResult> result;
    try {
      result.emplace(engine->execute(command));
    } catch (const std::bad_alloc&) {
      write_harness_error(output, line_number, "resource_failure");
      return finish_terminal_record(output, native_driver_engine_error_exit_code);
    } catch (...) {
      write_harness_error(output, line_number, "engine_exception");
      return finish_terminal_record(output, native_driver_engine_error_exit_code);
    }
    write_result(output, *engine, command, *result, command_index, line_number, mode,
                 is_checkpoint(command_index, config->snapshot_interval));
    if (!output) {
      return finish_terminal_record(output, native_driver_engine_error_exit_code);
    }
    ++command_index;
    if (!result->has_value()) {
      write_final(output, *engine, command_index);
      return finish_terminal_record(output, native_driver_engine_error_exit_code);
    }
  }

  if (input.bad() || (input.fail() && !input.eof())) {
    write_harness_error(output, line_number + 1U, "input_read_failure");
    return finish_terminal_record(output, native_driver_input_error_exit_code);
  }
  write_final(output, *engine, command_index);
  return finish_terminal_record(output, native_driver_success_exit_code);
}

}  // namespace

int run_native_driver(std::istream& input, std::ostream& output, OutputMode mode) {
  try {
    return run_native_driver_impl(input, output, mode, ConstructionFailureInjection::none);
  } catch (...) {
    // Any exception that escapes a pre-record parsing or execution boundary may
    // have occurred after a JSON record began. Do not append a second record to
    // potentially partial output; report the process failure through the exit
    // status only.
    return native_driver_engine_error_exit_code;
  }
}

#if defined(ATLAS_DIFF_NATIVE_NO_MAIN)
int run_native_driver_with_construction_failure_for_test(
    std::istream& input, std::ostream& output, NativeDriverConstructionFailureForTest failure,
    OutputMode mode) {
  const auto injection = failure == NativeDriverConstructionFailureForTest::allocation
                             ? ConstructionFailureInjection::allocation
                             : ConstructionFailureInjection::exception;
  try {
    return run_native_driver_impl(input, output, mode, injection);
  } catch (...) {
    return native_driver_engine_error_exit_code;
  }
}
#endif

}  // namespace atlaslob::differential

#ifndef ATLAS_DIFF_NATIVE_NO_MAIN
namespace {

[[nodiscard]] std::optional<atlaslob::differential::OutputMode> parse_mode(int argc, char** argv) {
  if (argc == 1) {
    return atlaslob::differential::OutputMode::exact;
  }
  if (argc == 2) {
    const std::string_view argument{argv[1]};
    if (argument == "exact" || argument == "--mode=exact") {
      return atlaslob::differential::OutputMode::exact;
    }
    if (argument == "compact" || argument == "--mode=compact") {
      return atlaslob::differential::OutputMode::compact;
    }
  }
  if (argc == 3 && std::string_view{argv[1]} == "--mode") {
    const std::string_view argument{argv[2]};
    if (argument == "exact") {
      return atlaslob::differential::OutputMode::exact;
    }
    if (argument == "compact") {
      return atlaslob::differential::OutputMode::compact;
    }
  }
  return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
  const auto mode = parse_mode(argc, argv);
  if (!mode.has_value()) {
    std::cerr << "usage: atlas_diff_native [exact|compact]\n";
    return atlaslob::differential::native_driver_input_error_exit_code;
  }
  return atlaslob::differential::run_native_driver(std::cin, std::cout, *mode);
}
#endif
