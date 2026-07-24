#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

#include "atlaslob/domain/validation.hpp"
#include "domain_fixture.hpp"
#include "engine_fixture.hpp"

namespace {

using atlaslob::domain::ClientId;
using atlaslob::domain::InstrumentId;
using atlaslob::domain::NewOrder;
using atlaslob::domain::OrderId;
using atlaslob::domain::OrderType;
using atlaslob::domain::PriceTicks;
using atlaslob::domain::Quantity;
using atlaslob::domain::Side;
using atlaslob::domain::TimeInForce;

void print_usage(std::string_view program) {
  std::cerr << "Usage:\n"
            << "  " << program << " validate-demo\n"
            << "  " << program << " domain-fixture <path>\n"
            << "  " << program << " engine-fixture <instrument_id> <path>\n";
}

int run_validation_demo() {
  const NewOrder order{
      .client_id = ClientId{1U},
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
  if (argc == 2 && std::string_view{argv[1]} == "validate-demo") {
    return run_validation_demo();
  }

  if (argc == 3 && std::string_view{argv[1]} == "domain-fixture") {
    return atlaslob::cli::run_domain_fixture_file(argv[2], std::cout);
  }

  if (argc >= 2 && std::string_view{argv[1]} == "engine-fixture") {
    if (argc != 4) {
      print_usage(argv[0]);
      return atlaslob::cli::engine_fixture_input_error_exit_code;
    }
    return atlaslob::cli::run_engine_fixture_file(argv[2], argv[3], std::cout);
  }

  print_usage(argv[0]);
  return EXIT_FAILURE;
}
