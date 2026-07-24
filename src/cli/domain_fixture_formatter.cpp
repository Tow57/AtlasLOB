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
                             const domain::NewOrder& order) {
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
                                const domain::CancelOrder& order) {
  const auto validation = domain::validate(order);

  output << "line=" << line_number << " CANCEL"
         << " client_id=" << order.client_id.value() << " order_id=" << order.order_id.value()
         << " instrument_id=" << order.instrument_id.value();
  write_validation(output, validation);
  return validation.accepted();
}

[[nodiscard]] bool write_replace(std::ostream& output, std::size_t line_number,
                                 const domain::ReplaceOrder& order) {
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
  const auto normalized = to_domain_command(command);
  return std::visit(
      [&output, line_number](const auto& order) {
        using Order = std::remove_cvref_t<decltype(order)>;
        if constexpr (std::is_same_v<Order, domain::NewOrder>) {
          return write_new(output, line_number, order);
        } else if constexpr (std::is_same_v<Order, domain::CancelOrder>) {
          return write_cancel(output, line_number, order);
        } else {
          static_assert(std::is_same_v<Order, domain::ReplaceOrder>);
          return write_replace(output, line_number, order);
        }
      },
      normalized);
}

}  // namespace atlaslob::cli::detail
