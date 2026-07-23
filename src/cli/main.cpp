#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

#include "atlaslob/domain/validation.hpp"

namespace {

using atlaslob::domain::InstrumentId;
using atlaslob::domain::NewOrder;
using atlaslob::domain::OrderId;
using atlaslob::domain::OrderType;
using atlaslob::domain::PriceTicks;
using atlaslob::domain::Quantity;
using atlaslob::domain::Side;
using atlaslob::domain::TimeInForce;

void print_usage(std::string_view program) {
  std::cerr << "Usage: " << program << " validate-demo\n";
}

int run_validation_demo() {
  const NewOrder order{
      .order_id = OrderId{1001U},
      .instrument_id = InstrumentId{1U},
      .side = Side::buy,
      .order_type = OrderType::limit,
      .time_in_force = TimeInForce::gtc,
      .limit_price = PriceTicks{10'125},
      .quantity = Quantity{10U},
  };

  const auto result = atlaslob::domain::validate(order);
  if (!result.accepted()) {
    std::cerr << "rejected reason=" << atlaslob::domain::to_string(result.reason) << '\n';
    return EXIT_FAILURE;
  }

  std::cout << "accepted"
            << " order_id=" << order.order_id.value()
            << " instrument_id=" << order.instrument_id.value()
            << " side=" << atlaslob::domain::to_string(order.side)
            << " type=" << atlaslob::domain::to_string(order.order_type)
            << " time_in_force=" << atlaslob::domain::to_string(order.time_in_force)
            << " price_ticks=" << order.limit_price->value()
            << " quantity=" << order.quantity.value() << '\n';
  return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 2 || std::string_view{argv[1]} != "validate-demo") {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }
  return run_validation_demo();
}
