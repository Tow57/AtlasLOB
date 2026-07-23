#pragma once

#include "atlaslob/domain/types.hpp"

namespace atlaslob::core {

class HeapOrderStorage;
class OrderNodeDeleter;
class PriceLevel;

#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
namespace test {
class CoreAccess;
}
#endif

struct OrderNodeSpec final {
  domain::OrderId order_id{};
  domain::ClientId client_id{};
  domain::InstrumentId instrument_id{};
  domain::Side side{};
  domain::PriceTicks price{};
  domain::Quantity remaining_quantity{};
  domain::Sequence priority_sequence{};

  bool operator==(const OrderNodeSpec&) const = default;
};

class OrderNode final {
 public:
  OrderNode(const OrderNode&) = delete;
  OrderNode& operator=(const OrderNode&) = delete;
  OrderNode(OrderNode&&) = delete;
  OrderNode& operator=(OrderNode&&) = delete;

  [[nodiscard]] domain::OrderId order_id() const noexcept { return order_id_; }
  [[nodiscard]] domain::ClientId client_id() const noexcept { return client_id_; }
  [[nodiscard]] domain::InstrumentId instrument_id() const noexcept { return instrument_id_; }
  [[nodiscard]] domain::Side side() const noexcept { return side_; }
  [[nodiscard]] domain::PriceTicks price() const noexcept { return price_; }
  [[nodiscard]] domain::Quantity remaining_quantity() const noexcept { return remaining_quantity_; }
  [[nodiscard]] domain::Sequence priority_sequence() const noexcept { return priority_sequence_; }
  [[nodiscard]] const OrderNode* previous() const noexcept { return previous_; }
  [[nodiscard]] const OrderNode* next() const noexcept { return next_; }
  [[nodiscard]] const PriceLevel* price_level() const noexcept { return price_level_; }
  [[nodiscard]] bool is_linked() const noexcept {
    return previous_ != nullptr || next_ != nullptr || price_level_ != nullptr;
  }

 private:
  friend class HeapOrderStorage;
  friend class OrderNodeDeleter;
  friend class PriceLevel;
#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
  friend class test::CoreAccess;
#endif

  explicit OrderNode(const OrderNodeSpec& spec) noexcept
      : order_id_{spec.order_id},
        client_id_{spec.client_id},
        instrument_id_{spec.instrument_id},
        side_{spec.side},
        price_{spec.price},
        remaining_quantity_{spec.remaining_quantity},
        priority_sequence_{spec.priority_sequence} {}
  ~OrderNode() = default;

  domain::OrderId order_id_{};
  domain::ClientId client_id_{};
  domain::InstrumentId instrument_id_{};
  domain::Side side_{};
  domain::PriceTicks price_{};
  domain::Quantity remaining_quantity_{};
  domain::Sequence priority_sequence_{};
  OrderNode* previous_{};
  OrderNode* next_{};
  PriceLevel* price_level_{};
};

}  // namespace atlaslob::core
