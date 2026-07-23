#include "domain_fixture_formatter.hpp"

#include <ostream>
#include <type_traits>
#include <variant>

#include "atlaslob/domain/commands.hpp"
#include "atlaslob/domain/validation.hpp"

namespace atlaslob::cli::detail {
namespace {

void write_validation(std::ostream& output, const domain::ValidationResult& result) {
  output << " validation=" << (result.accepted() ? "valid" : "invalid")
         << " reason=" << domain::to_string(result.reason) << '\n';
}

[[nodiscard]] bool write_new(std::ostream& output, std::size_t line_number,
                             const ParsedNewOrder& parsed) {
  const domain::NewOrder order{
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
  const auto validation = domain::validate(order);

  output << "line=" << line_number << " NEW"
         << " client_id=" << order.client_id.value() << " order_id=" << order.order_id.value()
         << " instrument_id=" << order.instrument_id.value()
         << " side=" << domain::to_string(order.side)
         << " type=" << domain::to_string(order.order_type)
         << " time_in_force=" << domain::to_string(order.time_in_force) << " price_ticks=";
  if (order.limit_price.has_value()) {
    output << order.limit_price->value();
  } else {
    output << '-';
  }
  output << " quantity=" << order.quantity.value();
  write_validation(output, validation);
  return validation.accepted();
}

[[nodiscard]] bool write_cancel(std::ostream& output, std::size_t line_number,
                                const ParsedCancelOrder& parsed) {
  const domain::CancelOrder order{
      .client_id = domain::ClientId{parsed.client_id},
      .order_id = domain::OrderId{parsed.order_id},
      .instrument_id = domain::InstrumentId{parsed.instrument_id},
  };
  const auto validation = domain::validate(order);

  output << "line=" << line_number << " CANCEL"
         << " client_id=" << order.client_id.value() << " order_id=" << order.order_id.value()
         << " instrument_id=" << order.instrument_id.value();
  write_validation(output, validation);
  return validation.accepted();
}

[[nodiscard]] bool write_replace(std::ostream& output, std::size_t line_number,
                                 const ParsedReplaceOrder& parsed) {
  const domain::ReplaceOrder order{
      .client_id = domain::ClientId{parsed.client_id},
      .old_order_id = domain::OrderId{parsed.old_order_id},
      .new_order_id = domain::OrderId{parsed.new_order_id},
      .instrument_id = domain::InstrumentId{parsed.instrument_id},
      .new_limit_price = domain::PriceTicks{parsed.new_limit_price},
      .new_quantity = domain::Quantity{parsed.new_quantity},
  };
  const auto validation = domain::validate(order);

  output << "line=" << line_number << " REPLACE"
         << " client_id=" << order.client_id.value()
         << " old_order_id=" << order.old_order_id.value()
         << " new_order_id=" << order.new_order_id.value()
         << " instrument_id=" << order.instrument_id.value()
         << " new_price_ticks=" << order.new_limit_price.value()
         << " new_quantity=" << order.new_quantity.value();
  write_validation(output, validation);
  return validation.accepted();
}

}  // namespace

void write_parse_error(std::ostream& output, std::size_t line_number, const ParseError& error) {
  output << "line=" << line_number << " PARSE_ERROR reason=" << error.reason << '\n';
}

bool write_validated_command(std::ostream& output, std::size_t line_number,
                             const ParsedCommand& command) {
  return std::visit(
      [&output, line_number](const auto& parsed) {
        using Parsed = std::remove_cvref_t<decltype(parsed)>;
        if constexpr (std::is_same_v<Parsed, ParsedNewOrder>) {
          return write_new(output, line_number, parsed);
        } else if constexpr (std::is_same_v<Parsed, ParsedCancelOrder>) {
          return write_cancel(output, line_number, parsed);
        } else {
          static_assert(std::is_same_v<Parsed, ParsedReplaceOrder>);
          return write_replace(output, line_number, parsed);
        }
      },
      command);
}

}  // namespace atlaslob::cli::detail
