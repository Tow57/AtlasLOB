#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "atlaslob/digest.hpp"
#include "atlaslob/matching_engine.hpp"

namespace {

using namespace atlaslob;

constexpr domain::InstrumentId instrument_id{7U};
constexpr domain::InstrumentId wrong_instrument_id{8U};
constexpr domain::Quantity maximum_order_quantity{50U};
constexpr domain::PriceTicks tick_increment{5};
constexpr std::size_t maximum_active_orders = 64U;
constexpr std::size_t operations_per_seed = 2'500U;
// Four documented seeds exercise 10,000 commands; the first seed is then run
// again to prove that the complete event/state transcript is deterministic.
constexpr std::array<std::uint64_t, 4U> stress_seeds{
    0xA71A'5EED'0000'0001ULL,
    0xA71A'5EED'0000'0002ULL,
    0xA71A'5EED'0000'0003ULL,
    0xA71A'5EED'0000'0004ULL,
};
static_assert(stress_seeds.size() * operations_per_seed >= 10'000U);

struct ModelOrder final {
  domain::OrderId order_id{};
  domain::ClientId client_id{};
  domain::InstrumentId instrument_id{};
  domain::Side side{};
  domain::PriceTicks price{};
  domain::Quantity remaining_quantity{};
  domain::Sequence priority_sequence{};
};

template <typename Comparator>
using ModelLevels = std::map<std::int64_t, std::deque<ModelOrder>, Comparator>;

using ModelBids = ModelLevels<std::greater<std::int64_t>>;
using ModelAsks = ModelLevels<std::less<std::int64_t>>;

struct ModelOutcome final {
  bool rejected{};
  std::vector<domain::Event> events;
};

struct Coverage final {
  std::size_t commits{};
  std::size_t rejections{};
  std::size_t trades{};
  std::size_t new_commands{};
  std::size_t cancel_commands{};
  std::size_t replace_commands{};
  std::size_t terminal_new_commits{};
  std::size_t unsupported_fok_rejections{};
  std::size_t wrong_route_rejections{};
  std::size_t wrong_owner_rejections{};
  std::size_t duplicate_id_rejections{};
  std::size_t unknown_id_rejections{};
  std::size_t capacity_rejections{};
};

void add_coverage(Coverage& total, const Coverage& addition) {
  total.commits += addition.commits;
  total.rejections += addition.rejections;
  total.trades += addition.trades;
  total.new_commands += addition.new_commands;
  total.cancel_commands += addition.cancel_commands;
  total.replace_commands += addition.replace_commands;
  total.terminal_new_commits += addition.terminal_new_commits;
  total.unsupported_fok_rejections += addition.unsupported_fok_rejections;
  total.wrong_route_rejections += addition.wrong_route_rejections;
  total.wrong_owner_rejections += addition.wrong_owner_rejections;
  total.duplicate_id_rejections += addition.duplicate_id_rejections;
  total.unknown_id_rejections += addition.unknown_id_rejections;
  total.capacity_rejections += addition.capacity_rejections;
}

struct Rejection final {
  domain::RejectReason reason{domain::RejectReason::none};
  std::optional<domain::OrderId> order_id{};
};

[[nodiscard]] std::optional<domain::OrderId> nonzero(domain::OrderId order_id) {
  return order_id.value() == 0U ? std::nullopt : std::optional<domain::OrderId>{order_id};
}

[[nodiscard]] bool valid_side(domain::Side side) {
  return side == domain::Side::buy || side == domain::Side::sell;
}

[[nodiscard]] bool valid_order_type(domain::OrderType order_type) {
  return order_type == domain::OrderType::limit || order_type == domain::OrderType::market;
}

[[nodiscard]] bool valid_time_in_force(domain::TimeInForce time_in_force) {
  return time_in_force == domain::TimeInForce::gtc || time_in_force == domain::TimeInForce::ioc ||
         time_in_force == domain::TimeInForce::fok;
}

[[nodiscard]] domain::RejectReason validate_new_shape(const domain::NewOrder& order) {
  if (order.client_id.value() == 0U) {
    return domain::RejectReason::invalid_client_id;
  }
  if (order.order_id.value() == 0U) {
    return domain::RejectReason::invalid_order_id;
  }
  if (order.instrument_id.value() == 0U) {
    return domain::RejectReason::invalid_instrument_id;
  }
  if (order.quantity.value() == 0U) {
    return domain::RejectReason::invalid_quantity;
  }
  if (!valid_side(order.side)) {
    return domain::RejectReason::invalid_side;
  }
  if (!valid_order_type(order.order_type)) {
    return domain::RejectReason::invalid_order_type;
  }
  if (!valid_time_in_force(order.time_in_force)) {
    return domain::RejectReason::invalid_time_in_force;
  }
  if (order.time_in_force == domain::TimeInForce::fok) {
    return domain::RejectReason::unsupported_time_in_force;
  }
  if (order.order_type == domain::OrderType::limit) {
    if (!order.limit_price.has_value()) {
      return domain::RejectReason::missing_limit_price;
    }
    if (order.limit_price->value() <= 0) {
      return domain::RejectReason::invalid_price;
    }
    return domain::RejectReason::none;
  }
  if (order.limit_price.has_value()) {
    return domain::RejectReason::unexpected_limit_price;
  }
  if (order.time_in_force != domain::TimeInForce::ioc) {
    return domain::RejectReason::invalid_order_type_time_in_force;
  }
  return domain::RejectReason::none;
}

[[nodiscard]] domain::RejectReason validate_cancel_shape(const domain::CancelOrder& order) {
  if (order.client_id.value() == 0U) {
    return domain::RejectReason::invalid_client_id;
  }
  if (order.order_id.value() == 0U) {
    return domain::RejectReason::invalid_order_id;
  }
  if (order.instrument_id.value() == 0U) {
    return domain::RejectReason::invalid_instrument_id;
  }
  return domain::RejectReason::none;
}

[[nodiscard]] domain::RejectReason validate_replace_shape(const domain::ReplaceOrder& order) {
  if (order.client_id.value() == 0U) {
    return domain::RejectReason::invalid_client_id;
  }
  if (order.old_order_id.value() == 0U || order.new_order_id.value() == 0U) {
    return domain::RejectReason::invalid_order_id;
  }
  if (order.old_order_id == order.new_order_id) {
    return domain::RejectReason::invalid_replacement_id;
  }
  if (order.instrument_id.value() == 0U) {
    return domain::RejectReason::invalid_instrument_id;
  }
  if (order.new_quantity.value() == 0U) {
    return domain::RejectReason::invalid_quantity;
  }
  if (order.new_limit_price.value() <= 0) {
    return domain::RejectReason::invalid_price;
  }
  return domain::RejectReason::none;
}

template <typename EventValue>
void append_event(std::vector<domain::Event>& events, domain::Sequence sequence,
                  domain::InstrumentId event_instrument, EventValue event) {
  event.header = {
      .command_sequence = sequence,
      .event_index = static_cast<std::uint32_t>(events.size()),
      .instrument_id = event_instrument,
  };
  events.emplace_back(std::move(event));
}

class ReferenceModel final {
 public:
  [[nodiscard]] ModelOutcome execute(const domain::Command& command) {
    const domain::Sequence sequence{next_sequence_++};
    last_sequence_ = sequence;
    return std::visit(
        [this, sequence](const auto& value) {
          using Value = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, domain::NewOrder>) {
            return execute_new(value, sequence);
          } else if constexpr (std::is_same_v<Value, domain::CancelOrder>) {
            return execute_cancel(value, sequence);
          } else {
            static_assert(std::is_same_v<Value, domain::ReplaceOrder>);
            return execute_replace(value, sequence);
          }
        },
        command);
  }

  [[nodiscard]] std::vector<ModelOrder> active_orders() const {
    std::vector<ModelOrder> result;
    result.reserve(active_order_count());
    append_orders(bids_, result);
    append_orders(asks_, result);
    return result;
  }

  [[nodiscard]] std::size_t active_order_count() const {
    return side_order_count(bids_) + side_order_count(asks_);
  }

  [[nodiscard]] BookTop top() const {
    return {
        .best_bid = top_level(bids_),
        .best_ask = top_level(asks_),
    };
  }

  [[nodiscard]] BookSnapshot snapshot() const {
    BookSnapshot result{
        .semantics_version = atlaslob_semantics_version,
        .instrument_id = instrument_id,
        .last_sequence = last_sequence_,
        .sequence_exhausted = false,
        .active_order_count = static_cast<std::uint64_t>(active_order_count()),
        .bids = {},
        .asks = {},
    };
    append_snapshot_levels(bids_, result.bids);
    append_snapshot_levels(asks_, result.asks);
    return result;
  }

  [[nodiscard]] domain::Sequence next_sequence() const { return domain::Sequence{next_sequence_}; }

 private:
  struct FillProjection final {
    std::uint64_t remaining{};
    std::size_t terminal_passives{};
  };

  template <typename Levels>
  [[nodiscard]] static std::size_t side_order_count(const Levels& levels) {
    std::size_t result = 0U;
    for (const auto& [price, orders] : levels) {
      static_cast<void>(price);
      result += orders.size();
    }
    return result;
  }

  template <typename Levels>
  static void append_orders(const Levels& levels, std::vector<ModelOrder>& destination) {
    for (const auto& [price, orders] : levels) {
      static_cast<void>(price);
      destination.insert(destination.end(), orders.begin(), orders.end());
    }
  }

  template <typename Levels>
  [[nodiscard]] static std::optional<domain::TopOfBookLevel> top_level(const Levels& levels) {
    if (levels.empty()) {
      return std::nullopt;
    }
    std::uint64_t aggregate = 0U;
    for (const auto& order : levels.begin()->second) {
      aggregate += order.remaining_quantity.value();
    }
    return domain::TopOfBookLevel{
        .price = domain::PriceTicks{levels.begin()->first},
        .aggregate_quantity = domain::Quantity{aggregate},
    };
  }

  template <typename Levels>
  static void append_snapshot_levels(const Levels& levels,
                                     std::vector<PriceLevelSnapshot>& destination) {
    destination.reserve(levels.size());
    for (const auto& [price, orders] : levels) {
      PriceLevelSnapshot level{
          .price = domain::PriceTicks{price},
          .aggregate_quantity = {},
          .orders = {},
      };
      level.orders.reserve(orders.size());
      std::uint64_t aggregate = 0U;
      for (const auto& order : orders) {
        aggregate += order.remaining_quantity.value();
        level.orders.push_back({
            .order_id = order.order_id,
            .client_id = order.client_id,
            .instrument_id = order.instrument_id,
            .side = order.side,
            .price = order.price,
            .remaining_quantity = order.remaining_quantity,
            .priority_sequence = order.priority_sequence,
        });
      }
      level.aggregate_quantity = domain::Quantity{aggregate};
      destination.push_back(std::move(level));
    }
  }

  [[nodiscard]] const ModelOrder* find(domain::OrderId order_id) const {
    if (const auto* order = find_in(bids_, order_id); order != nullptr) {
      return order;
    }
    return find_in(asks_, order_id);
  }

  template <typename Levels>
  [[nodiscard]] static const ModelOrder* find_in(const Levels& levels, domain::OrderId order_id) {
    for (const auto& [price, orders] : levels) {
      static_cast<void>(price);
      for (const auto& order : orders) {
        if (order.order_id == order_id) {
          return &order;
        }
      }
    }
    return nullptr;
  }

  template <typename Levels>
  [[nodiscard]] static bool erase_from(Levels& levels, domain::OrderId order_id) {
    for (auto level = levels.begin(); level != levels.end(); ++level) {
      auto& orders = level->second;
      const auto position =
          std::find_if(orders.begin(), orders.end(),
                       [order_id](const ModelOrder& order) { return order.order_id == order_id; });
      if (position == orders.end()) {
        continue;
      }
      orders.erase(position);
      if (orders.empty()) {
        levels.erase(level);
      }
      return true;
    }
    return false;
  }

  void erase(domain::OrderId order_id, domain::Side side) {
    const bool erased =
        side == domain::Side::buy ? erase_from(bids_, order_id) : erase_from(asks_, order_id);
    ASSERT_TRUE(erased);
  }

  [[nodiscard]] static bool crosses(const domain::NewOrder& aggressor, std::int64_t resting_price) {
    if (aggressor.order_type == domain::OrderType::market) {
      return true;
    }
    return aggressor.side == domain::Side::buy ? aggressor.limit_price->value() >= resting_price
                                               : aggressor.limit_price->value() <= resting_price;
  }

  template <typename Levels>
  [[nodiscard]] static FillProjection project_fills(const domain::NewOrder& aggressor,
                                                    const Levels& opposite) {
    FillProjection result{.remaining = aggressor.quantity.value()};
    for (const auto& [price, orders] : opposite) {
      if (result.remaining == 0U || !crosses(aggressor, price)) {
        break;
      }
      for (const auto& passive : orders) {
        if (result.remaining == 0U) {
          break;
        }
        if (result.remaining >= passive.remaining_quantity.value()) {
          result.remaining -= passive.remaining_quantity.value();
          ++result.terminal_passives;
        } else {
          result.remaining = 0U;
        }
      }
    }
    return result;
  }

  [[nodiscard]] FillProjection project_fills(const domain::NewOrder& aggressor) const {
    return aggressor.side == domain::Side::buy ? project_fills(aggressor, asks_)
                                               : project_fills(aggressor, bids_);
  }

  [[nodiscard]] static bool residual_rests(const domain::NewOrder& order, std::uint64_t remaining) {
    return remaining != 0U && order.order_type == domain::OrderType::limit &&
           order.time_in_force == domain::TimeInForce::gtc;
  }

  [[nodiscard]] bool capacity_allows(const domain::NewOrder& order, bool removes_old_order) const {
    const auto projected = project_fills(order);
    auto final_count =
        active_order_count() - projected.terminal_passives - (removes_old_order ? 1U : 0U);
    if (residual_rests(order, projected.remaining)) {
      ++final_count;
    }
    return final_count <= maximum_active_orders;
  }

  [[nodiscard]] std::optional<Rejection> validate_new(const domain::NewOrder& order) const {
    if (const auto reason = validate_new_shape(order); reason != domain::RejectReason::none) {
      return Rejection{.reason = reason, .order_id = nonzero(order.order_id)};
    }
    if (order.instrument_id != instrument_id) {
      return Rejection{
          .reason = domain::RejectReason::unknown_instrument,
          .order_id = order.order_id,
      };
    }
    if (order.quantity > maximum_order_quantity) {
      return Rejection{
          .reason = domain::RejectReason::quantity_out_of_range,
          .order_id = order.order_id,
      };
    }
    if (order.order_type == domain::OrderType::limit &&
        order.limit_price->value() % tick_increment.value() != 0) {
      return Rejection{
          .reason = domain::RejectReason::invalid_tick,
          .order_id = order.order_id,
      };
    }
    if (find(order.order_id) != nullptr) {
      return Rejection{
          .reason = domain::RejectReason::duplicate_order_id,
          .order_id = order.order_id,
      };
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<Rejection> validate_cancel(const domain::CancelOrder& order) const {
    if (const auto reason = validate_cancel_shape(order); reason != domain::RejectReason::none) {
      return Rejection{.reason = reason, .order_id = nonzero(order.order_id)};
    }
    if (order.instrument_id != instrument_id) {
      return Rejection{
          .reason = domain::RejectReason::unknown_instrument,
          .order_id = order.order_id,
      };
    }
    const auto* existing = find(order.order_id);
    if (existing == nullptr) {
      return Rejection{
          .reason = domain::RejectReason::unknown_order_id,
          .order_id = order.order_id,
      };
    }
    if (existing->client_id != order.client_id) {
      return Rejection{
          .reason = domain::RejectReason::ownership_mismatch,
          .order_id = order.order_id,
      };
    }
    if (existing->instrument_id != order.instrument_id) {
      return Rejection{
          .reason = domain::RejectReason::instrument_mismatch,
          .order_id = order.order_id,
      };
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<Rejection> validate_replace(const domain::ReplaceOrder& order) const {
    if (const auto reason = validate_replace_shape(order); reason != domain::RejectReason::none) {
      auto relevant_id = order.old_order_id;
      if (reason == domain::RejectReason::invalid_order_id && order.old_order_id.value() != 0U) {
        relevant_id = order.new_order_id;
      } else if (reason == domain::RejectReason::invalid_replacement_id ||
                 reason == domain::RejectReason::invalid_quantity ||
                 reason == domain::RejectReason::invalid_price) {
        relevant_id = order.new_order_id;
      }
      return Rejection{.reason = reason, .order_id = nonzero(relevant_id)};
    }
    if (order.instrument_id != instrument_id) {
      return Rejection{
          .reason = domain::RejectReason::unknown_instrument,
          .order_id = order.old_order_id,
      };
    }
    if (order.new_quantity > maximum_order_quantity) {
      return Rejection{
          .reason = domain::RejectReason::quantity_out_of_range,
          .order_id = order.new_order_id,
      };
    }
    if (order.new_limit_price.value() % tick_increment.value() != 0) {
      return Rejection{
          .reason = domain::RejectReason::invalid_tick,
          .order_id = order.new_order_id,
      };
    }
    const auto* existing = find(order.old_order_id);
    if (existing == nullptr) {
      return Rejection{
          .reason = domain::RejectReason::unknown_order_id,
          .order_id = order.old_order_id,
      };
    }
    if (existing->client_id != order.client_id) {
      return Rejection{
          .reason = domain::RejectReason::ownership_mismatch,
          .order_id = order.old_order_id,
      };
    }
    if (existing->instrument_id != order.instrument_id) {
      return Rejection{
          .reason = domain::RejectReason::instrument_mismatch,
          .order_id = order.old_order_id,
      };
    }
    if (find(order.new_order_id) != nullptr) {
      return Rejection{
          .reason = domain::RejectReason::invalid_replacement_id,
          .order_id = order.new_order_id,
      };
    }
    return std::nullopt;
  }

  [[nodiscard]] static ModelOutcome reject(domain::Sequence sequence,
                                           domain::InstrumentId event_instrument,
                                           domain::CommandType command_type,
                                           const Rejection& rejection) {
    ModelOutcome result{.rejected = true, .events = {}};
    append_event(result.events, sequence, event_instrument,
                 domain::RejectedEvent{
                     .command_type = command_type,
                     .reason = rejection.reason,
                     .order_id = rejection.order_id,
                 });
    return result;
  }

  template <typename Levels>
  static std::uint64_t match_into(const domain::NewOrder& aggressor, Levels& opposite,
                                  domain::Sequence sequence, std::vector<domain::Event>& events) {
    auto remaining = aggressor.quantity.value();
    auto level = opposite.begin();
    while (level != opposite.end() && remaining != 0U && crosses(aggressor, level->first)) {
      auto& queue = level->second;
      while (!queue.empty() && remaining != 0U) {
        auto& passive = queue.front();
        const auto execution = std::min(remaining, passive.remaining_quantity.value());
        remaining -= execution;
        passive.remaining_quantity =
            domain::Quantity{passive.remaining_quantity.value() - execution};
        append_event(events, sequence, aggressor.instrument_id,
                     domain::TradeEvent{
                         .aggressor_order_id = aggressor.order_id,
                         .resting_order_id = passive.order_id,
                         .aggressor_client_id = aggressor.client_id,
                         .resting_client_id = passive.client_id,
                         .aggressor_side = aggressor.side,
                         .execution_price = passive.price,
                         .execution_quantity = domain::Quantity{execution},
                         .aggressor_remaining = domain::Quantity{remaining},
                         .resting_remaining = passive.remaining_quantity,
                     });
        if (passive.remaining_quantity.value() == 0U) {
          queue.pop_front();
        }
      }
      if (queue.empty()) {
        level = opposite.erase(level);
      } else {
        ++level;
      }
    }
    return remaining;
  }

  [[nodiscard]] std::uint64_t match(const domain::NewOrder& aggressor, domain::Sequence sequence,
                                    std::vector<domain::Event>& events) {
    return aggressor.side == domain::Side::buy ? match_into(aggressor, asks_, sequence, events)
                                               : match_into(aggressor, bids_, sequence, events);
  }

  void rest(const domain::NewOrder& order, std::uint64_t remaining,
            domain::Sequence priority_sequence) {
    ModelOrder rested{
        .order_id = order.order_id,
        .client_id = order.client_id,
        .instrument_id = order.instrument_id,
        .side = order.side,
        .price = *order.limit_price,
        .remaining_quantity = domain::Quantity{remaining},
        .priority_sequence = priority_sequence,
    };
    if (order.side == domain::Side::buy) {
      bids_[order.limit_price->value()].push_back(rested);
    } else {
      asks_[order.limit_price->value()].push_back(rested);
    }
  }

  static void append_aggressor_terminal(std::vector<domain::Event>& events,
                                        const domain::NewOrder& order, domain::Sequence sequence,
                                        std::uint64_t remaining) {
    if (residual_rests(order, remaining)) {
      append_event(events, sequence, order.instrument_id,
                   domain::RestedEvent{
                       .order_id = order.order_id,
                       .client_id = order.client_id,
                       .side = order.side,
                       .price = *order.limit_price,
                       .remaining_quantity = domain::Quantity{remaining},
                   });
      return;
    }
    const auto reason = remaining == 0U ? domain::DoneReason::filled
                                        : (order.order_type == domain::OrderType::market
                                               ? domain::DoneReason::market_exhausted
                                               : domain::DoneReason::ioc_residual_canceled);
    append_event(events, sequence, order.instrument_id,
                 domain::DoneEvent{
                     .order_id = order.order_id,
                     .reason = reason,
                     .remaining_quantity = domain::Quantity{remaining},
                 });
  }

  static void append_book_change(std::vector<domain::Event>& events, domain::Sequence sequence,
                                 const BookTop& before, const BookTop& after) {
    if (before == after) {
      return;
    }
    append_event(events, sequence, instrument_id,
                 domain::BookChangedEvent{
                     .best_bid = after.best_bid,
                     .best_ask = after.best_ask,
                 });
  }

  [[nodiscard]] ModelOutcome execute_new(const domain::NewOrder& order, domain::Sequence sequence) {
    if (const auto rejection = validate_new(order); rejection.has_value()) {
      return reject(sequence, order.instrument_id, domain::CommandType::new_order, *rejection);
    }
    if (!capacity_allows(order, false)) {
      return reject(sequence, order.instrument_id, domain::CommandType::new_order,
                    Rejection{
                        .reason = domain::RejectReason::capacity_exceeded,
                        .order_id = order.order_id,
                    });
    }

    const auto before = top();
    ModelOutcome result;
    append_event(result.events, sequence, order.instrument_id,
                 domain::AcceptedEvent{.command_type = domain::CommandType::new_order});
    const auto remaining = match(order, sequence, result.events);
    if (residual_rests(order, remaining)) {
      rest(order, remaining, sequence);
    }
    append_aggressor_terminal(result.events, order, sequence, remaining);
    append_book_change(result.events, sequence, before, top());
    return result;
  }

  [[nodiscard]] ModelOutcome execute_cancel(const domain::CancelOrder& order,
                                            domain::Sequence sequence) {
    if (const auto rejection = validate_cancel(order); rejection.has_value()) {
      return reject(sequence, order.instrument_id, domain::CommandType::cancel, *rejection);
    }

    const auto before = top();
    const ModelOrder canceled = *find(order.order_id);
    ModelOutcome result;
    append_event(result.events, sequence, order.instrument_id,
                 domain::AcceptedEvent{.command_type = domain::CommandType::cancel});
    append_event(result.events, sequence, order.instrument_id,
                 domain::CanceledEvent{
                     .order_id = order.order_id,
                     .canceled_quantity = canceled.remaining_quantity,
                 });
    append_event(result.events, sequence, order.instrument_id,
                 domain::DoneEvent{
                     .order_id = order.order_id,
                     .reason = domain::DoneReason::canceled,
                     .remaining_quantity = canceled.remaining_quantity,
                 });
    erase(canceled.order_id, canceled.side);
    append_book_change(result.events, sequence, before, top());
    return result;
  }

  [[nodiscard]] ModelOutcome execute_replace(const domain::ReplaceOrder& command,
                                             domain::Sequence sequence) {
    if (const auto rejection = validate_replace(command); rejection.has_value()) {
      return reject(sequence, command.instrument_id, domain::CommandType::replace, *rejection);
    }

    const ModelOrder old = *find(command.old_order_id);
    const domain::NewOrder replacement{
        .client_id = old.client_id,
        .order_id = command.new_order_id,
        .instrument_id = old.instrument_id,
        .side = old.side,
        .order_type = domain::OrderType::limit,
        .time_in_force = domain::TimeInForce::gtc,
        .limit_price = command.new_limit_price,
        .quantity = command.new_quantity,
    };
    if (!capacity_allows(replacement, true)) {
      return reject(sequence, command.instrument_id, domain::CommandType::replace,
                    Rejection{
                        .reason = domain::RejectReason::capacity_exceeded,
                        .order_id = command.new_order_id,
                    });
    }

    const auto before = top();
    ModelOutcome result;
    append_event(result.events, sequence, command.instrument_id,
                 domain::AcceptedEvent{.command_type = domain::CommandType::replace});
    append_event(result.events, sequence, command.instrument_id,
                 domain::ReplacedEvent{
                     .old_order_id = command.old_order_id,
                     .new_order_id = command.new_order_id,
                 });
    append_event(result.events, sequence, command.instrument_id,
                 domain::CanceledEvent{
                     .order_id = command.old_order_id,
                     .canceled_quantity = old.remaining_quantity,
                 });
    append_event(result.events, sequence, command.instrument_id,
                 domain::DoneEvent{
                     .order_id = command.old_order_id,
                     .reason = domain::DoneReason::replaced,
                     .remaining_quantity = old.remaining_quantity,
                 });

    erase(old.order_id, old.side);
    const auto remaining = match(replacement, sequence, result.events);
    if (residual_rests(replacement, remaining)) {
      rest(replacement, remaining, sequence);
    }
    append_aggressor_terminal(result.events, replacement, sequence, remaining);
    append_book_change(result.events, sequence, before, top());
    return result;
  }

  ModelBids bids_;
  ModelAsks asks_;
  std::uint64_t next_sequence_{1U};
  domain::Sequence last_sequence_{};
};

[[nodiscard]] std::uint32_t different_client(domain::ClientId client_id) {
  return client_id.value() % 4U + 1U;
}

class CommandGenerator final {
 public:
  explicit CommandGenerator(std::uint64_t seed) : random_{seed} {}

  [[nodiscard]] domain::Command next(const ReferenceModel& model, std::size_t step) {
    const auto active = model.active_orders();
    if (active.size() >= 56U) {
      return valid_cancel(active);
    }

    switch (step % 24U) {
      case 0U:
        return valid_limit(domain::TimeInForce::gtc);
      case 1U:
        return valid_limit(domain::TimeInForce::ioc);
      case 2U:
        return valid_market();
      case 3U:
        return unsupported_fok();
      case 4U:
        return active.empty() ? domain::Command{valid_limit(domain::TimeInForce::gtc)}
                              : domain::Command{valid_cancel(active)};
      case 5U:
        return unknown_cancel(step);
      case 6U:
        return active.empty() ? domain::Command{valid_limit(domain::TimeInForce::gtc)}
                              : domain::Command{wrong_owner_cancel(active)};
      case 7U:
        return active.empty() ? domain::Command{valid_limit(domain::TimeInForce::gtc)}
                              : domain::Command{wrong_route_cancel(active)};
      case 8U:
        return active.empty() ? domain::Command{valid_limit(domain::TimeInForce::gtc)}
                              : domain::Command{valid_replace(active)};
      case 9U:
        return unknown_replace(step);
      case 10U:
        return active.empty() ? domain::Command{valid_limit(domain::TimeInForce::gtc)}
                              : domain::Command{wrong_owner_replace(active)};
      case 11U:
        return active.empty() ? domain::Command{valid_limit(domain::TimeInForce::gtc)}
                              : domain::Command{duplicate_replace(active)};
      case 12U:
        return active.empty() ? domain::Command{valid_limit(domain::TimeInForce::gtc)}
                              : domain::Command{duplicate_new(active)};
      case 13U:
        return wrong_route_new();
      case 14U:
        return excessive_quantity_new();
      case 15U:
        return invalid_tick_new();
      case 16U:
        return invalid_client_new();
      case 17U:
        return valid_limit((random_() & 1U) == 0U ? domain::TimeInForce::gtc
                                                  : domain::TimeInForce::ioc);
      case 18U:
        return valid_market();
      case 19U:
        return active.empty() ? domain::Command{valid_limit(domain::TimeInForce::gtc)}
                              : domain::Command{valid_replace(active)};
      case 20U:
        return zero_id_cancel();
      case 21U:
        return active.empty() ? domain::Command{valid_limit(domain::TimeInForce::gtc)}
                              : domain::Command{same_id_replace(active)};
      case 22U:
        return invalid_market_gtc();
      case 23U:
        return invalid_side_new();
    }
    return valid_limit(domain::TimeInForce::gtc);
  }

 private:
  [[nodiscard]] domain::OrderId fresh_id() { return domain::OrderId{next_order_id_++}; }

  [[nodiscard]] domain::ClientId random_client() {
    return domain::ClientId{1U + static_cast<std::uint32_t>(random_() % 4U)};
  }

  [[nodiscard]] domain::Side random_side() {
    return (random_() & 1U) == 0U ? domain::Side::buy : domain::Side::sell;
  }

  [[nodiscard]] domain::PriceTicks random_price() {
    return domain::PriceTicks{80 + 5 * static_cast<std::int64_t>(random_() % 9U)};
  }

  [[nodiscard]] domain::Quantity random_quantity() {
    return domain::Quantity{1U + random_() % 20U};
  }

  [[nodiscard]] const ModelOrder& select(const std::vector<ModelOrder>& active) {
    return active[static_cast<std::size_t>(random_() % active.size())];
  }

  [[nodiscard]] domain::NewOrder valid_limit(domain::TimeInForce time_in_force) {
    return {
        .client_id = random_client(),
        .order_id = fresh_id(),
        .instrument_id = instrument_id,
        .side = random_side(),
        .order_type = domain::OrderType::limit,
        .time_in_force = time_in_force,
        .limit_price = random_price(),
        .quantity = random_quantity(),
    };
  }

  [[nodiscard]] domain::NewOrder valid_market() {
    return {
        .client_id = random_client(),
        .order_id = fresh_id(),
        .instrument_id = instrument_id,
        .side = random_side(),
        .order_type = domain::OrderType::market,
        .time_in_force = domain::TimeInForce::ioc,
        .limit_price = std::nullopt,
        .quantity = random_quantity(),
    };
  }

  [[nodiscard]] domain::NewOrder unsupported_fok() {
    auto order = valid_limit(domain::TimeInForce::fok);
    return order;
  }

  [[nodiscard]] domain::CancelOrder valid_cancel(const std::vector<ModelOrder>& active) {
    const auto& order = select(active);
    return {
        .client_id = order.client_id,
        .order_id = order.order_id,
        .instrument_id = instrument_id,
    };
  }

  [[nodiscard]] domain::CancelOrder unknown_cancel(std::size_t step) {
    return {
        .client_id = random_client(),
        .order_id = domain::OrderId{900'000U + static_cast<std::uint64_t>(step)},
        .instrument_id = instrument_id,
    };
  }

  [[nodiscard]] domain::CancelOrder wrong_owner_cancel(const std::vector<ModelOrder>& active) {
    const auto& order = select(active);
    return {
        .client_id = domain::ClientId{different_client(order.client_id)},
        .order_id = order.order_id,
        .instrument_id = instrument_id,
    };
  }

  [[nodiscard]] domain::CancelOrder wrong_route_cancel(const std::vector<ModelOrder>& active) {
    const auto& order = select(active);
    return {
        .client_id = order.client_id,
        .order_id = order.order_id,
        .instrument_id = wrong_instrument_id,
    };
  }

  [[nodiscard]] domain::ReplaceOrder valid_replace(const std::vector<ModelOrder>& active) {
    const auto& old = select(active);
    return {
        .client_id = old.client_id,
        .old_order_id = old.order_id,
        .new_order_id = fresh_id(),
        .instrument_id = instrument_id,
        .new_limit_price = random_price(),
        .new_quantity = random_quantity(),
    };
  }

  [[nodiscard]] domain::ReplaceOrder unknown_replace(std::size_t step) {
    return {
        .client_id = random_client(),
        .old_order_id = domain::OrderId{800'000U + static_cast<std::uint64_t>(step)},
        .new_order_id = fresh_id(),
        .instrument_id = instrument_id,
        .new_limit_price = random_price(),
        .new_quantity = random_quantity(),
    };
  }

  [[nodiscard]] domain::ReplaceOrder wrong_owner_replace(const std::vector<ModelOrder>& active) {
    auto command = valid_replace(active);
    const auto old = std::find_if(
        active.begin(), active.end(),
        [&command](const ModelOrder& order) { return order.order_id == command.old_order_id; });
    command.client_id = domain::ClientId{different_client(old->client_id)};
    return command;
  }

  [[nodiscard]] domain::ReplaceOrder duplicate_replace(const std::vector<ModelOrder>& active) {
    const auto& old = select(active);
    const auto& duplicate = select(active);
    return {
        .client_id = old.client_id,
        .old_order_id = old.order_id,
        .new_order_id = duplicate.order_id,
        .instrument_id = instrument_id,
        .new_limit_price = random_price(),
        .new_quantity = random_quantity(),
    };
  }

  [[nodiscard]] domain::NewOrder duplicate_new(const std::vector<ModelOrder>& active) {
    auto order = valid_limit(domain::TimeInForce::gtc);
    order.order_id = select(active).order_id;
    return order;
  }

  [[nodiscard]] domain::NewOrder wrong_route_new() {
    auto order = valid_limit(domain::TimeInForce::gtc);
    order.instrument_id = wrong_instrument_id;
    return order;
  }

  [[nodiscard]] domain::NewOrder excessive_quantity_new() {
    auto order = valid_limit(domain::TimeInForce::gtc);
    order.quantity = domain::Quantity{maximum_order_quantity.value() + 1U};
    return order;
  }

  [[nodiscard]] domain::NewOrder invalid_tick_new() {
    auto order = valid_limit(domain::TimeInForce::gtc);
    order.limit_price = domain::PriceTicks{101};
    return order;
  }

  [[nodiscard]] domain::NewOrder invalid_client_new() {
    auto order = valid_limit(domain::TimeInForce::gtc);
    order.client_id = domain::ClientId{};
    return order;
  }

  [[nodiscard]] static domain::CancelOrder zero_id_cancel() {
    return {
        .client_id = domain::ClientId{1U},
        .order_id = domain::OrderId{},
        .instrument_id = instrument_id,
    };
  }

  [[nodiscard]] domain::ReplaceOrder same_id_replace(const std::vector<ModelOrder>& active) {
    const auto& old = select(active);
    return {
        .client_id = old.client_id,
        .old_order_id = old.order_id,
        .new_order_id = old.order_id,
        .instrument_id = instrument_id,
        .new_limit_price = random_price(),
        .new_quantity = random_quantity(),
    };
  }

  [[nodiscard]] domain::NewOrder invalid_market_gtc() {
    auto order = valid_market();
    order.time_in_force = domain::TimeInForce::gtc;
    return order;
  }

  [[nodiscard]] domain::NewOrder invalid_side_new() {
    auto order = valid_limit(domain::TimeInForce::gtc);
    order.side = static_cast<domain::Side>(0U);
    return order;
  }

  std::mt19937_64 random_;
  std::uint64_t next_order_id_{1U};
};

[[nodiscard]] std::string command_text(const domain::Command& command) {
  return std::visit(
      [](const auto& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        std::ostringstream stream;
        if constexpr (std::is_same_v<Value, domain::NewOrder>) {
          stream << "NEW c=" << value.client_id.value() << " o=" << value.order_id.value()
                 << " i=" << value.instrument_id.value()
                 << " s=" << static_cast<unsigned>(value.side)
                 << " t=" << static_cast<unsigned>(value.order_type)
                 << " tif=" << static_cast<unsigned>(value.time_in_force) << " p=";
          if (value.limit_price.has_value()) {
            stream << value.limit_price->value();
          } else {
            stream << '-';
          }
          stream << " q=" << value.quantity.value();
        } else if constexpr (std::is_same_v<Value, domain::CancelOrder>) {
          stream << "CANCEL c=" << value.client_id.value() << " o=" << value.order_id.value()
                 << " i=" << value.instrument_id.value();
        } else {
          static_assert(std::is_same_v<Value, domain::ReplaceOrder>);
          stream << "REPLACE c=" << value.client_id.value() << " old=" << value.old_order_id.value()
                 << " new=" << value.new_order_id.value() << " i=" << value.instrument_id.value()
                 << " p=" << value.new_limit_price.value() << " q=" << value.new_quantity.value();
        }
        return stream.str();
      },
      command);
}

[[nodiscard]] std::string trace_text(std::uint64_t seed, std::size_t step,
                                     const std::deque<std::string>& recent) {
  std::ostringstream stream;
  stream << "seed=0x" << std::hex << seed << std::dec << " step=" << step << " recent=[";
  bool first = true;
  for (const auto& command : recent) {
    if (!first) {
      stream << " | ";
    }
    stream << command;
    first = false;
  }
  stream << ']';
  return stream.str();
}

struct RunFingerprint final {
  std::vector<Digest256> state_digests;
  std::vector<Digest256> event_digests;
};

void record_coverage(const domain::Command& command, const ModelOutcome& outcome,
                     Coverage& coverage) {
  if (std::holds_alternative<domain::NewOrder>(command)) {
    ++coverage.new_commands;
  } else if (std::holds_alternative<domain::CancelOrder>(command)) {
    ++coverage.cancel_commands;
  } else {
    ++coverage.replace_commands;
  }

  if (outcome.rejected) {
    ++coverage.rejections;
    const auto& rejection = std::get<domain::RejectedEvent>(outcome.events.front());
    switch (rejection.reason) {
      case domain::RejectReason::unsupported_time_in_force:
        ++coverage.unsupported_fok_rejections;
        break;
      case domain::RejectReason::unknown_instrument:
        ++coverage.wrong_route_rejections;
        break;
      case domain::RejectReason::ownership_mismatch:
        ++coverage.wrong_owner_rejections;
        break;
      case domain::RejectReason::duplicate_order_id:
      case domain::RejectReason::invalid_replacement_id:
        ++coverage.duplicate_id_rejections;
        break;
      case domain::RejectReason::unknown_order_id:
        ++coverage.unknown_id_rejections;
        break;
      case domain::RejectReason::capacity_exceeded:
        ++coverage.capacity_rejections;
        break;
      default:
        break;
    }
  } else {
    ++coverage.commits;
    if (const auto* order = std::get_if<domain::NewOrder>(&command);
        order != nullptr && (order->order_type == domain::OrderType::market ||
                             order->time_in_force == domain::TimeInForce::ioc)) {
      ++coverage.terminal_new_commits;
    }
  }

  coverage.trades += static_cast<std::size_t>(
      std::count_if(outcome.events.begin(), outcome.events.end(), [](const domain::Event& event) {
        return domain::event_type(event) == domain::EventType::trade;
      }));
}

void execute_and_compare(MatchingEngine& engine, ReferenceModel& model,
                         const domain::Command& command, RunFingerprint* fingerprint,
                         Coverage& coverage) {
  const auto expected_sequence = model.next_sequence();
  const auto expected = model.execute(command);
  auto actual = engine.execute(command);
  ASSERT_EQ(actual.error, EngineError::none);
  ASSERT_TRUE(actual.has_value());
  ASSERT_EQ(actual.rejected(), expected.rejected);
  ASSERT_EQ(actual.committed(), !expected.rejected);
  ASSERT_TRUE(actual.batch.has_value());
  ASSERT_EQ(actual.batch->size(), expected.events.size());
  ASSERT_EQ(actual.batch->command_sequence(), expected_sequence);
  for (std::size_t event_index = 0U; event_index < expected.events.size(); ++event_index) {
    ASSERT_EQ((*actual.batch)[event_index], expected.events[event_index])
        << "event_index=" << event_index;
  }

  const domain::EventBatch expected_batch{expected.events};
  const auto expected_event_digest = event_digest(expected_batch);
  const auto actual_event_digest = event_digest(*actual.batch);
  ASSERT_EQ(actual_event_digest, expected_event_digest)
      << "actual=" << actual_event_digest.hex() << " expected=" << expected_event_digest.hex();

  const auto expected_snapshot = model.snapshot();
  const auto actual_snapshot = engine.snapshot();
  ASSERT_EQ(actual_snapshot, expected_snapshot);
  ASSERT_EQ(engine.active_order_count(), model.active_order_count());
  ASSERT_EQ(engine.empty(), model.active_order_count() == 0U);
  ASSERT_EQ(engine.top(), model.top());
  ASSERT_EQ(engine.next_sequence(), model.next_sequence());
  ASSERT_FALSE(engine.sequence_exhausted());

  const auto expected_state_digest = state_digest(expected_snapshot);
  const auto actual_state_digest = engine.state_digest();
  ASSERT_EQ(actual_state_digest, expected_state_digest)
      << "actual=" << actual_state_digest.hex() << " expected=" << expected_state_digest.hex();
  ASSERT_EQ(state_digest(actual_snapshot), expected_state_digest);

  record_coverage(command, expected, coverage);
  if (fingerprint != nullptr) {
    // Persist the SUT values so the rerun assertion verifies engine output,
    // rather than merely comparing two independently repeated model outputs.
    fingerprint->event_digests.push_back(actual_event_digest);
    fingerprint->state_digests.push_back(actual_state_digest);
  }
}

void run_seed(std::uint64_t seed, RunFingerprint& fingerprint, Coverage& coverage) {
  const MatchingEngineConfig config{
      .max_order_quantity = maximum_order_quantity,
      .tick_increment = tick_increment,
      .max_active_orders = maximum_active_orders,
  };
  MatchingEngine engine{instrument_id, config};
  ReferenceModel model;
  CommandGenerator generator{seed};
  std::deque<std::string> recent;
  fingerprint.state_digests.reserve(operations_per_seed);
  fingerprint.event_digests.reserve(operations_per_seed);

  for (std::size_t step = 0U; step < operations_per_seed; ++step) {
    const auto command = generator.next(model, step);
    recent.push_back(command_text(command));
    if (recent.size() > 6U) {
      recent.pop_front();
    }
    SCOPED_TRACE(trace_text(seed, step, recent));

    execute_and_compare(engine, model, command, &fingerprint, coverage);
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
  }
}

void run_directed_capacity_scenario(Coverage& coverage) {
  const MatchingEngineConfig config{
      .max_order_quantity = maximum_order_quantity,
      .tick_increment = tick_increment,
      .max_active_orders = maximum_active_orders,
  };
  MatchingEngine engine{instrument_id, config};
  ReferenceModel model;

  for (std::size_t index = 0U; index < maximum_active_orders; ++index) {
    SCOPED_TRACE(::testing::Message() << "directed capacity fill index=" << index);
    const domain::Command command = domain::NewOrder{
        .client_id = domain::ClientId{1U + static_cast<std::uint32_t>(index % 4U)},
        .order_id = domain::OrderId{1U + static_cast<std::uint64_t>(index)},
        .instrument_id = instrument_id,
        .side = domain::Side::buy,
        .order_type = domain::OrderType::limit,
        .time_in_force = domain::TimeInForce::gtc,
        .limit_price = domain::PriceTicks{100},
        .quantity = domain::Quantity{2U},
    };
    execute_and_compare(engine, model, command, nullptr, coverage);
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
  }
  ASSERT_EQ(engine.active_order_count(), maximum_active_orders);
  ASSERT_EQ(model.active_order_count(), maximum_active_orders);

  const auto capacity_rejections_before = coverage.capacity_rejections;
  const domain::Command over_capacity = domain::NewOrder{
      .client_id = domain::ClientId{9U},
      .order_id = domain::OrderId{1'000U},
      .instrument_id = instrument_id,
      .side = domain::Side::buy,
      .order_type = domain::OrderType::limit,
      .time_in_force = domain::TimeInForce::gtc,
      .limit_price = domain::PriceTicks{95},
      .quantity = domain::Quantity{1U},
  };
  {
    SCOPED_TRACE("directed over-capacity resting New");
    execute_and_compare(engine, model, over_capacity, nullptr, coverage);
  }
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
  ASSERT_EQ(coverage.capacity_rejections, capacity_rejections_before + 1U);
  ASSERT_EQ(engine.active_order_count(), maximum_active_orders);

  const auto terminal_commits_before = coverage.terminal_new_commits;
  const auto commits_before = coverage.commits;
  const auto trades_before = coverage.trades;
  const domain::Command terminal_at_capacity = domain::NewOrder{
      .client_id = domain::ClientId{9U},
      .order_id = domain::OrderId{1'001U},
      .instrument_id = instrument_id,
      .side = domain::Side::sell,
      .order_type = domain::OrderType::market,
      .time_in_force = domain::TimeInForce::ioc,
      .limit_price = std::nullopt,
      .quantity = domain::Quantity{1U},
  };
  {
    SCOPED_TRACE("directed terminal market New at capacity");
    execute_and_compare(engine, model, terminal_at_capacity, nullptr, coverage);
  }
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
  EXPECT_EQ(coverage.terminal_new_commits, terminal_commits_before + 1U);
  EXPECT_EQ(coverage.commits, commits_before + 1U);
  EXPECT_EQ(coverage.trades, trades_before + 1U);
  EXPECT_EQ(engine.active_order_count(), maximum_active_orders);
  EXPECT_EQ(engine.top().best_bid,
            (domain::TopOfBookLevel{
                .price = domain::PriceTicks{100},
                .aggregate_quantity = domain::Quantity{2U * maximum_active_orders - 1U},
            }));
}

TEST(MatchingEngineModelStress,
     MatchesIndependentReferenceForTenThousandDeterministicMixedCommands) {
  std::array<RunFingerprint, stress_seeds.size()> fingerprints;
  Coverage total_coverage;
  for (std::size_t seed_index = 0U; seed_index < stress_seeds.size(); ++seed_index) {
    Coverage seed_coverage;
    run_seed(stress_seeds[seed_index], fingerprints[seed_index], seed_coverage);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    add_coverage(total_coverage, seed_coverage);
  }

  RunFingerprint repeated;
  Coverage repeated_coverage;
  run_seed(stress_seeds.front(), repeated, repeated_coverage);
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
  EXPECT_EQ(repeated.event_digests, fingerprints.front().event_digests);
  EXPECT_EQ(repeated.state_digests, fingerprints.front().state_digests);

  run_directed_capacity_scenario(total_coverage);
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
  EXPECT_GT(total_coverage.commits, 0U);
  EXPECT_GT(total_coverage.rejections, 0U);
  EXPECT_GT(total_coverage.trades, 0U);
  EXPECT_GT(total_coverage.new_commands, 0U);
  EXPECT_GT(total_coverage.cancel_commands, 0U);
  EXPECT_GT(total_coverage.replace_commands, 0U);
  EXPECT_GT(total_coverage.terminal_new_commits, 0U);
  EXPECT_GT(total_coverage.unsupported_fok_rejections, 0U);
  EXPECT_GT(total_coverage.wrong_route_rejections, 0U);
  EXPECT_GT(total_coverage.wrong_owner_rejections, 0U);
  EXPECT_GT(total_coverage.duplicate_id_rejections, 0U);
  EXPECT_GT(total_coverage.unknown_id_rejections, 0U);
  EXPECT_GT(total_coverage.capacity_rejections, 0U);
}

}  // namespace
