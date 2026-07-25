#include "atlaslob/multi_instrument_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "atlaslob/domain/validation.hpp"
#include "command_executor.hpp"
#include "command_sequencer.hpp"
#include "execution_policy.hpp"
#include "instrument_book.hpp"
#include "match_plan.hpp"
#include "multi_instrument_engine_access.hpp"
#include "snapshot_sequence.hpp"
#include "top_of_book.hpp"

namespace atlaslob {
namespace {

struct Identity final {
  domain::InstrumentId instrument_id{};
  domain::ClientId client_id{};

  bool operator==(const Identity&) const = default;
};

using ActiveOrderDirectory =
    std::unordered_map<domain::OrderId, Identity, domain::StrongValueHash<domain::OrderId>>;

struct IdentityRemoval final {
  domain::OrderId order_id{};
  Identity identity{};
};

struct ValidationDecision final {
  domain::RejectReason reason{domain::RejectReason::none};
  std::optional<domain::OrderId> relevant_order_id{};

  [[nodiscard]] bool accepted() const noexcept { return reason == domain::RejectReason::none; }
};

[[nodiscard]] core::ExecutionPolicy make_execution_policy(
    const MatchingEngineConfig& config) noexcept {
  return {
      .max_order_quantity = config.max_order_quantity,
      .tick_increment = config.tick_increment,
      .max_active_orders = config.max_active_orders,
  };
}

template <typename Size>
[[nodiscard]] constexpr bool capacity_fits_u64(Size value) noexcept {
  static_assert(std::is_unsigned_v<Size>);
  if constexpr (std::numeric_limits<Size>::max() >
                static_cast<Size>(std::numeric_limits<std::uint64_t>::max())) {
    return value == std::numeric_limits<Size>::max() ||
           value <= static_cast<Size>(std::numeric_limits<std::uint64_t>::max());
  }
  static_cast<void>(value);
  return true;
}

[[nodiscard]] std::optional<domain::OrderId> nonzero(domain::OrderId order_id) noexcept {
  return order_id.value() == 0U ? std::nullopt : std::optional<domain::OrderId>{order_id};
}

[[nodiscard]] ValidationDecision reject(domain::RejectReason reason,
                                        domain::OrderId order_id) noexcept {
  return {
      .reason = reason,
      .relevant_order_id = nonzero(order_id),
  };
}

[[nodiscard]] domain::OrderId replacement_relevant_id(const domain::ReplaceOrder& order,
                                                      domain::RejectReason reason) noexcept {
  switch (reason) {
    case domain::RejectReason::invalid_order_id:
      return order.old_order_id.value() == 0U ? order.old_order_id : order.new_order_id;
    case domain::RejectReason::invalid_replacement_id:
    case domain::RejectReason::invalid_quantity:
    case domain::RejectReason::invalid_price:
      return order.new_order_id;
    default:
      return order.old_order_id;
  }
}

[[nodiscard]] bool is_tick_aligned(domain::PriceTicks price,
                                   domain::PriceTicks tick_increment) noexcept {
  return price.value() % tick_increment.value() == 0;
}

[[nodiscard]] EngineResult make_rejection(domain::Sequence sequence,
                                          domain::InstrumentId instrument_id,
                                          domain::CommandType command_type,
                                          const ValidationDecision& decision) {
  std::vector<domain::Event> events;
  events.reserve(1U);
  events.emplace_back(domain::RejectedEvent{
      .header =
          {
              .command_sequence = sequence,
              .event_index = 0U,
              .instrument_id = instrument_id,
          },
      .command_type = command_type,
      .reason = decision.reason,
      .order_id = decision.relevant_order_id,
  });
  return EngineResult::success(domain::EventBatch{std::move(events)});
}

[[nodiscard]] EngineResult translate(core::CommandExecutionResult result) {
  if (result) {
    return EngineResult::success(std::move(*result.batch));
  }
  return EngineResult::failure(result.admission_error ==
                                       core::CommandAdmissionError::sequence_exhausted
                                   ? EngineError::sequence_exhausted
                                   : EngineError::internal_failure);
}

[[nodiscard]] bool is_executor_capacity_rejection(const core::PreparedCommandExecution& prepared,
                                                  domain::CommandType command_type,
                                                  domain::OrderId relevant_order_id) noexcept {
  const auto* const batch = prepared.batch();
  if (batch == nullptr || batch->size() != 1U) {
    return false;
  }
  const auto* const rejected = std::get_if<domain::RejectedEvent>(&(*batch)[0]);
  return rejected != nullptr && rejected->command_type == command_type &&
         rejected->reason == domain::RejectReason::capacity_exceeded &&
         rejected->order_id == std::optional<domain::OrderId>{relevant_order_id};
}

template <domain::Side RestingSide>
[[nodiscard]] std::vector<PriceLevelSnapshot> snapshot_side(
    const core::BookSide<RestingSide>& side) {
  std::vector<PriceLevelSnapshot> levels;
  levels.reserve(side.level_count());
  for (const core::PriceLevel& level : side) {
    PriceLevelSnapshot level_snapshot{
        .price = level.price(),
        .aggregate_quantity = level.aggregate_quantity(),
        .orders = {},
    };
    level_snapshot.orders.reserve(level.order_count());
    for (const core::OrderNode* node = level.head(); node != nullptr; node = node->next()) {
      level_snapshot.orders.push_back({
          .order_id = node->order_id(),
          .client_id = node->client_id(),
          .instrument_id = node->instrument_id(),
          .side = node->side(),
          .price = node->price(),
          .remaining_quantity = node->remaining_quantity(),
          .priority_sequence = node->priority_sequence(),
      });
    }
    levels.push_back(std::move(level_snapshot));
  }
  return levels;
}

[[nodiscard]] std::uint64_t as_u64(std::size_t value) noexcept {
  if (!capacity_fits_u64(value)) {
    std::terminate();
  }
  return static_cast<std::uint64_t>(value);
}

}  // namespace

class MultiInstrumentEngine::Impl final {
 public:
  struct BookEntry final {
    explicit BookEntry(InstrumentConfig value)
        : config{value},
          book{config.instrument_id},
          executor{book, make_execution_policy(config.matching)} {}

    BookEntry(const BookEntry&) = delete;
    BookEntry& operator=(const BookEntry&) = delete;
    BookEntry(BookEntry&&) = delete;
    BookEntry& operator=(BookEntry&&) = delete;
    ~BookEntry() = default;

    InstrumentConfig config;
    core::InstrumentBook book;
    core::CommandExecutor executor;
  };

  explicit Impl(std::span<const InstrumentConfig> catalog, MultiInstrumentEngineConfig config)
      : config_{config} {
    if (catalog.empty()) {
      throw std::invalid_argument{"MultiInstrumentEngine requires a nonempty catalog"};
    }
    if (catalog.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
      throw std::invalid_argument{"instrument catalog is too large"};
    }
    if (!capacity_fits_u64(config_.max_total_active_orders)) {
      throw std::invalid_argument{"global active-order capacity exceeds canonical representation"};
    }

    for (const auto& instrument : catalog) {
      if (instrument.instrument_id.value() == 0U) {
        throw std::invalid_argument{"instrument IDs must be nonzero"};
      }
      if (!instrument.matching.valid()) {
        throw std::invalid_argument{"instrument configuration is invalid"};
      }
      if (!capacity_fits_u64(instrument.matching.max_active_orders)) {
        throw std::invalid_argument{
            "instrument active-order capacity exceeds canonical representation"};
      }
      const auto [position, inserted] = books_.emplace(instrument.instrument_id, nullptr);
      if (!inserted) {
        throw std::invalid_argument{"instrument IDs must be unique"};
      }
      position->second = std::make_unique<BookEntry>(instrument);
    }
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl() {
    if (preparation_active_) {
      std::terminate();
    }
  }

  [[nodiscard]] std::unique_ptr<core::PreparedMultiInstrumentCommand::Impl> prepare_state(
      const domain::NewOrder& order);
  [[nodiscard]] std::unique_ptr<core::PreparedMultiInstrumentCommand::Impl> prepare_state(
      const domain::CancelOrder& order);
  [[nodiscard]] std::unique_ptr<core::PreparedMultiInstrumentCommand::Impl> prepare_state(
      const domain::ReplaceOrder& order);

  [[nodiscard]] BookEntry* find_book(domain::InstrumentId instrument_id) noexcept {
    const auto position = books_.find(instrument_id);
    return position == books_.end() ? nullptr : position->second.get();
  }

  [[nodiscard]] const BookEntry* find_book(domain::InstrumentId instrument_id) const noexcept {
    const auto position = books_.find(instrument_id);
    return position == books_.end() ? nullptr : position->second.get();
  }

  [[nodiscard]] domain::Sequence reserved_sequence() const noexcept {
    return sequencer_.next_sequence();
  }

  [[nodiscard]] bool unavailable_sequence() const noexcept {
    return sequencer_.exhausted() || sequencer_.next_sequence().value() == 0U;
  }

  void publish_sequence(domain::Sequence expected) noexcept {
    const auto issued = sequencer_.issue();
    if (!issued || issued.sequence != expected) {
      std::terminate();
    }
  }

  [[nodiscard]] ValidationDecision validate(const domain::NewOrder& order,
                                            const BookEntry*& target) const noexcept {
    const auto pure = domain::validate(order);
    if (!pure.accepted()) {
      return reject(pure.reason, order.order_id);
    }

    target = find_book(order.instrument_id);
    if (target == nullptr) {
      return reject(domain::RejectReason::unknown_instrument, order.order_id);
    }
    if (!target->book.validate_invariants() || target->book.has_pending_preparation()) {
      return {};
    }
    if (order.quantity > target->config.matching.max_order_quantity) {
      return reject(domain::RejectReason::quantity_out_of_range, order.order_id);
    }
    if (order.order_type == domain::OrderType::limit &&
        !is_tick_aligned(*order.limit_price, target->config.matching.tick_increment)) {
      return reject(domain::RejectReason::invalid_tick, order.order_id);
    }
    if (active_orders_.contains(order.order_id)) {
      return reject(domain::RejectReason::duplicate_order_id, order.order_id);
    }
    return {};
  }

  [[nodiscard]] ValidationDecision validate(const domain::CancelOrder& order,
                                            const BookEntry*& target) const noexcept {
    const auto pure = domain::validate(order);
    if (!pure.accepted()) {
      return reject(pure.reason, order.order_id);
    }

    target = find_book(order.instrument_id);
    if (target == nullptr) {
      return reject(domain::RejectReason::unknown_instrument, order.order_id);
    }
    if (!target->book.validate_invariants() || target->book.has_pending_preparation()) {
      return {};
    }

    const auto identity = active_orders_.find(order.order_id);
    if (identity == active_orders_.end()) {
      return reject(domain::RejectReason::unknown_order_id, order.order_id);
    }
    if (identity->second.client_id != order.client_id) {
      return reject(domain::RejectReason::ownership_mismatch, order.order_id);
    }
    if (identity->second.instrument_id != order.instrument_id) {
      return reject(domain::RejectReason::instrument_mismatch, order.order_id);
    }
    return {};
  }

  [[nodiscard]] ValidationDecision validate(const domain::ReplaceOrder& order,
                                            const BookEntry*& target) const noexcept {
    const auto pure = domain::validate(order);
    if (!pure.accepted()) {
      return reject(pure.reason, replacement_relevant_id(order, pure.reason));
    }

    target = find_book(order.instrument_id);
    if (target == nullptr) {
      return reject(domain::RejectReason::unknown_instrument, order.old_order_id);
    }
    if (!target->book.validate_invariants() || target->book.has_pending_preparation()) {
      return {};
    }
    if (order.new_quantity > target->config.matching.max_order_quantity) {
      return reject(domain::RejectReason::quantity_out_of_range, order.new_order_id);
    }
    if (!is_tick_aligned(order.new_limit_price, target->config.matching.tick_increment)) {
      return reject(domain::RejectReason::invalid_tick, order.new_order_id);
    }

    const auto old_identity = active_orders_.find(order.old_order_id);
    if (old_identity == active_orders_.end()) {
      return reject(domain::RejectReason::unknown_order_id, order.old_order_id);
    }
    if (old_identity->second.client_id != order.client_id) {
      return reject(domain::RejectReason::ownership_mismatch, order.old_order_id);
    }
    if (old_identity->second.instrument_id != order.instrument_id) {
      return reject(domain::RejectReason::instrument_mismatch, order.old_order_id);
    }
    if (active_orders_.contains(order.new_order_id)) {
      return reject(domain::RejectReason::invalid_replacement_id, order.new_order_id);
    }
    return {};
  }

  [[nodiscard]] bool valid_target(const BookEntry* target) const noexcept {
    return target != nullptr && target->book.validate_invariants() &&
           !target->book.has_pending_preparation();
  }

  [[nodiscard]] bool within_capacities(const core::MatchPlan& plan, const BookEntry& target,
                                       bool removes_existing_order) const noexcept {
    const auto book_projection = core::project_active_order_count(
        plan, target.book.active_order_count(), removes_existing_order);
    const auto global_projection =
        core::project_active_order_count(plan, active_orders_.size(), removes_existing_order);
    return book_projection &&
           book_projection.within_limit(target.config.matching.max_active_orders) &&
           global_projection && global_projection.within_limit(config_.max_total_active_orders);
  }

  [[nodiscard]] bool validate_invariants() const noexcept {
    const auto next_sequence = sequencer_.next_sequence();
    if (sequencer_.exhausted() != (next_sequence.value() == 0U)) {
      return false;
    }
    const auto last_sequence = core::snapshot_last_sequence(next_sequence, sequencer_.exhausted());

    const auto count_priority = [this](domain::Sequence priority) noexcept {
      std::size_t count = 0U;
      for (const auto& [ignored_instrument, entry] : books_) {
        static_cast<void>(ignored_instrument);
        if (entry == nullptr) {
          return std::numeric_limits<std::size_t>::max();
        }
        const auto count_side = [&count, priority](const auto& side) noexcept {
          for (const core::PriceLevel& level : side) {
            for (const core::OrderNode* node = level.head(); node != nullptr; node = node->next()) {
              if (node->priority_sequence() == priority) {
                if (count == std::numeric_limits<std::size_t>::max()) {
                  return false;
                }
                ++count;
              }
            }
          }
          return true;
        };
        if (!count_side(entry->book.bids()) || !count_side(entry->book.asks())) {
          return std::numeric_limits<std::size_t>::max();
        }
      }
      return count;
    };

    std::size_t total = 0U;
    for (const auto& [instrument_id, entry_ptr] : books_) {
      if (entry_ptr == nullptr || entry_ptr->config.instrument_id != instrument_id ||
          !entry_ptr->config.matching.valid() || !entry_ptr->book.validate_invariants() ||
          (entry_ptr->book.has_pending_preparation() && !preparation_active_) ||
          entry_ptr->book.active_order_count() > entry_ptr->config.matching.max_active_orders) {
        return false;
      }
      if (entry_ptr->book.active_order_count() > std::numeric_limits<std::size_t>::max() - total) {
        return false;
      }
      total += entry_ptr->book.active_order_count();

      const auto verify_side = [this, &count_priority, last_sequence,
                                instrument_id](const auto& side) noexcept {
        for (const core::PriceLevel& level : side) {
          for (const core::OrderNode* node = level.head(); node != nullptr; node = node->next()) {
            if (node->instrument_id() != instrument_id || node->priority_sequence().value() == 0U ||
                node->priority_sequence() > last_sequence ||
                count_priority(node->priority_sequence()) != 1U) {
              return false;
            }
            const auto identity = active_orders_.find(node->order_id());
            if (identity == active_orders_.end() ||
                identity->second.instrument_id != instrument_id ||
                identity->second.client_id != node->client_id()) {
              return false;
            }
          }
        }
        return true;
      };
      if (!verify_side(entry_ptr->book.bids()) || !verify_side(entry_ptr->book.asks())) {
        return false;
      }
    }

    if (total != active_orders_.size() || total > config_.max_total_active_orders) {
      return false;
    }
    for (const auto& [order_id, identity] : active_orders_) {
      const auto* entry = find_book(identity.instrument_id);
      const auto* node = entry == nullptr ? nullptr : entry->book.find(order_id);
      if (node == nullptr || node->client_id() != identity.client_id ||
          node->instrument_id() != identity.instrument_id) {
        return false;
      }
    }
    return true;
  }

  MultiInstrumentEngineConfig config_;
  std::map<domain::InstrumentId, std::unique_ptr<BookEntry>> books_;
  ActiveOrderDirectory active_orders_;
  core::CommandSequencer sequencer_;
  bool preparation_active_{};
};

class core::PreparedMultiInstrumentCommand::Impl final {
 public:
  Impl(domain::Command command, EngineResult result) noexcept
      : command_{std::move(command)}, immediate_result_{std::move(result)} {}

  Impl(MultiInstrumentEngine::Impl& owner, domain::Command command, domain::Sequence sequence,
       EngineResult result) noexcept
      : owner_{&owner},
        command_{std::move(command)},
        sequence_{sequence},
        immediate_result_{std::move(result)},
        publishes_sequence_{true} {}

  Impl(MultiInstrumentEngine::Impl& owner, domain::Command command, domain::Sequence sequence,
       core::PreparedCommandExecution book_preparation, std::vector<IdentityRemoval> removals,
       ActiveOrderDirectory::node_type addition) noexcept
      : owner_{&owner},
        command_{std::move(command)},
        sequence_{sequence},
        book_preparation_{std::move(book_preparation)},
        removals_{std::move(removals)},
        addition_{std::move(addition)},
        publishes_sequence_{true} {}

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  ~Impl() noexcept { abandon(); }

  [[nodiscard]] const domain::Command& command() const noexcept { return command_; }

  [[nodiscard]] const domain::EventBatch* batch() const noexcept {
    if (consumed_) {
      return nullptr;
    }
    if (immediate_result_.has_value()) {
      return immediate_result_->batch();
    }
    return book_preparation_.has_value() ? book_preparation_->batch() : nullptr;
  }

  [[nodiscard]] EngineError error() const noexcept {
    if (consumed_) {
      return EngineError::internal_failure;
    }
    if (immediate_result_.has_value()) {
      return immediate_result_->error();
    }
    if (!book_preparation_.has_value() || book_preparation_->has_value()) {
      return EngineError::none;
    }
    return book_preparation_->result().admission_error ==
                   core::CommandAdmissionError::sequence_exhausted
               ? EngineError::sequence_exhausted
               : EngineError::internal_failure;
  }

  [[nodiscard]] EngineResult commit() noexcept {
    if (consumed_) {
      return EngineResult::failure(EngineError::internal_failure);
    }

    if (owner_ == nullptr) {
      consumed_ = true;
      if (!immediate_result_.has_value()) {
        return EngineResult::failure(EngineError::internal_failure);
      }
      return std::move(*immediate_result_);
    }

    if (immediate_result_.has_value()) {
      if (!publishes_sequence_ || immediate_result_->batch() == nullptr) {
        std::terminate();
      }
      owner_->publish_sequence(sequence_);
      auto result = std::move(*immediate_result_);
      release_owner();
      if (!owner_invariants_after_release_) {
        std::terminate();
      }
      consumed_ = true;
      return result;
    }

    if (!book_preparation_.has_value() || !book_preparation_->has_value() || !publishes_sequence_) {
      std::terminate();
    }

    if (book_preparation_->rejected()) {
      if (!removals_.empty() || !addition_.empty()) {
        std::terminate();
      }
      auto book_result = book_preparation_->commit();
      if (!book_result || !batch_rejected(book_result)) {
        std::terminate();
      }
      book_preparation_.reset();
      owner_->publish_sequence(sequence_);
      auto result = translate(std::move(book_result));
      release_owner();
      if (!owner_invariants_after_release_) {
        std::terminate();
      }
      consumed_ = true;
      return result;
    }

    if (!book_preparation_->committed()) {
      std::terminate();
    }

    if (!identity_delta_valid()) {
      std::terminate();
    }

    if (!addition_.empty()) {
      if (owner_->active_orders_.contains(addition_.key())) {
        std::terminate();
      }
      const auto inserted = owner_->active_orders_.insert(std::move(addition_));
      if (!inserted.inserted) {
        std::terminate();
      }
    }
    for (const auto& removal : removals_) {
      if (owner_->active_orders_.erase(removal.order_id) != 1U) {
        std::terminate();
      }
    }

    auto book_result = book_preparation_->commit();
    if (!book_result || batch_rejected(book_result)) {
      std::terminate();
    }
    book_preparation_.reset();
    owner_->publish_sequence(sequence_);
    auto result = translate(std::move(book_result));
    release_owner();
    if (!owner_invariants_after_release_) {
      std::terminate();
    }
    consumed_ = true;
    return result;
  }

 private:
  [[nodiscard]] static bool batch_rejected(const core::CommandExecutionResult& result) noexcept {
    return result.batch.has_value() &&
           domain::event_type((*result.batch)[0]) == domain::EventType::rejected;
  }

  [[nodiscard]] std::size_t removal_count(domain::OrderId order_id) const noexcept {
    std::size_t count = 0U;
    for (const auto& removal : removals_) {
      if (removal.order_id == order_id) {
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] bool identity_delta_valid() const noexcept {
    if (owner_ == nullptr || !book_preparation_.has_value()) {
      return false;
    }
    const auto* const prepared_batch = book_preparation_->batch();
    if (prepared_batch == nullptr || prepared_batch->empty() ||
        domain::event_type((*prepared_batch)[0]) != domain::EventType::accepted) {
      return false;
    }

    for (std::size_t index = 0U; index < removals_.size(); ++index) {
      const auto& removal = removals_[index];
      if (removal.order_id.value() == 0U) {
        return false;
      }
      const auto position = owner_->active_orders_.find(removal.order_id);
      if (position == owner_->active_orders_.end() || position->second != removal.identity) {
        return false;
      }
      for (std::size_t previous = 0U; previous < index; ++previous) {
        if (removals_[previous].order_id == removal.order_id) {
          return false;
        }
      }
      if (!addition_.empty() && addition_.key() == removal.order_id) {
        return false;
      }
    }

    std::size_t rested_count = 0U;
    for (const auto& event : prepared_batch->events()) {
      if (const auto* trade = std::get_if<domain::TradeEvent>(&event);
          trade != nullptr && trade->resting_remaining.value() == 0U) {
        if (removal_count(trade->resting_order_id) != 1U) {
          return false;
        }
        for (const auto& removal : removals_) {
          if (removal.order_id == trade->resting_order_id &&
              (removal.identity.instrument_id != trade->header.instrument_id ||
               removal.identity.client_id != trade->resting_client_id)) {
            return false;
          }
        }
      }
      const auto* rested = std::get_if<domain::RestedEvent>(&event);
      if (rested == nullptr) {
        continue;
      }
      ++rested_count;
      if (rested_count != 1U || addition_.empty() || addition_.key() != rested->order_id ||
          addition_.mapped().instrument_id != rested->header.instrument_id ||
          addition_.mapped().client_id != rested->client_id) {
        return false;
      }
      const bool matches_command = std::visit(
          [rested](const auto& command) noexcept {
            using Command = std::remove_cvref_t<decltype(command)>;
            if constexpr (std::is_same_v<Command, domain::NewOrder>) {
              return rested->order_id == command.order_id &&
                     rested->client_id == command.client_id &&
                     rested->header.instrument_id == command.instrument_id;
            } else if constexpr (std::is_same_v<Command, domain::ReplaceOrder>) {
              return rested->order_id == command.new_order_id &&
                     rested->client_id == command.client_id &&
                     rested->header.instrument_id == command.instrument_id;
            } else {
              return false;
            }
          },
          command_);
      if (!matches_command) {
        return false;
      }
    }
    if (addition_.empty() != (rested_count == 0U)) {
      return false;
    }
    if (!addition_.empty() && owner_->active_orders_.contains(addition_.key())) {
      return false;
    }

    for (const auto& removal : removals_) {
      bool expected = std::visit(
          [&removal](const auto& command) noexcept {
            using Command = std::remove_cvref_t<decltype(command)>;
            if constexpr (std::is_same_v<Command, domain::CancelOrder>) {
              return removal.order_id == command.order_id &&
                     removal.identity.instrument_id == command.instrument_id &&
                     removal.identity.client_id == command.client_id;
            } else if constexpr (std::is_same_v<Command, domain::ReplaceOrder>) {
              return removal.order_id == command.old_order_id &&
                     removal.identity.instrument_id == command.instrument_id &&
                     removal.identity.client_id == command.client_id;
            } else {
              return false;
            }
          },
          command_);
      for (const auto& event : prepared_batch->events()) {
        const auto* trade = std::get_if<domain::TradeEvent>(&event);
        expected = expected || (trade != nullptr && trade->resting_remaining.value() == 0U &&
                                trade->resting_order_id == removal.order_id);
      }
      if (!expected) {
        return false;
      }
    }

    const auto mandatory_removal = std::visit(
        [](const auto& command) noexcept {
          using Command = std::remove_cvref_t<decltype(command)>;
          if constexpr (std::is_same_v<Command, domain::CancelOrder>) {
            return std::optional<domain::OrderId>{command.order_id};
          } else if constexpr (std::is_same_v<Command, domain::ReplaceOrder>) {
            return std::optional<domain::OrderId>{command.old_order_id};
          } else {
            return std::optional<domain::OrderId>{};
          }
        },
        command_);
    return !mandatory_removal.has_value() || removal_count(*mandatory_removal) == 1U;
  }

  void abandon() noexcept {
    if (consumed_) {
      return;
    }
    book_preparation_.reset();
    addition_ = {};
    release_owner();
    consumed_ = true;
  }

  void release_owner() noexcept {
    if (owner_ == nullptr) {
      owner_invariants_after_release_ = true;
      return;
    }
    if (!owner_->preparation_active_) {
      std::terminate();
    }
    auto* const owner = owner_;
    owner_->preparation_active_ = false;
    owner_ = nullptr;
    owner_invariants_after_release_ = owner->validate_invariants();
  }

  MultiInstrumentEngine::Impl* owner_{};
  domain::Command command_;
  domain::Sequence sequence_{};
  std::optional<EngineResult> immediate_result_;
  std::optional<core::PreparedCommandExecution> book_preparation_;
  std::vector<IdentityRemoval> removals_;
  ActiveOrderDirectory::node_type addition_;
  bool publishes_sequence_{};
  bool consumed_{};
  bool owner_invariants_after_release_{true};
};

core::PreparedMultiInstrumentCommand::PreparedMultiInstrumentCommand(
    std::unique_ptr<Impl> implementation) noexcept
    : impl_{std::move(implementation)} {}

core::PreparedMultiInstrumentCommand::PreparedMultiInstrumentCommand(
    PreparedMultiInstrumentCommand&& other) noexcept = default;

core::PreparedMultiInstrumentCommand& core::PreparedMultiInstrumentCommand::operator=(
    PreparedMultiInstrumentCommand&& other) noexcept = default;

core::PreparedMultiInstrumentCommand::~PreparedMultiInstrumentCommand() noexcept = default;

const domain::Command& core::PreparedMultiInstrumentCommand::command() const noexcept {
  static const domain::Command empty{domain::NewOrder{}};
  return impl_ == nullptr ? empty : impl_->command();
}

const domain::EventBatch* core::PreparedMultiInstrumentCommand::batch() const noexcept {
  return impl_ == nullptr ? nullptr : impl_->batch();
}

EngineError core::PreparedMultiInstrumentCommand::error() const noexcept {
  return impl_ == nullptr ? EngineError::internal_failure : impl_->error();
}

bool core::PreparedMultiInstrumentCommand::has_value() const noexcept { return batch() != nullptr; }

bool core::PreparedMultiInstrumentCommand::rejected() const noexcept {
  const auto* value = batch();
  return value != nullptr && domain::event_type((*value)[0]) == domain::EventType::rejected;
}

bool core::PreparedMultiInstrumentCommand::committed() const noexcept {
  const auto* value = batch();
  return value != nullptr && domain::event_type((*value)[0]) == domain::EventType::accepted;
}

EngineResult core::PreparedMultiInstrumentCommand::commit() noexcept {
  return impl_ == nullptr ? EngineResult::failure(EngineError::internal_failure) : impl_->commit();
}

std::unique_ptr<core::PreparedMultiInstrumentCommand::Impl>
MultiInstrumentEngine::Impl::prepare_state(const domain::NewOrder& order) {
  const domain::Command command{order};
  if (preparation_active_) {
    return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
        command, EngineResult::failure(EngineError::internal_failure));
  }
  if (unavailable_sequence()) {
    return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
        command, EngineResult::failure(EngineError::sequence_exhausted));
  }

  preparation_active_ = true;
  try {
    const auto sequence = reserved_sequence();
    const BookEntry* target = nullptr;
    const auto decision = validate(order, target);
    if (!valid_target(target) && decision.accepted()) {
      preparation_active_ = false;
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          command, EngineResult::failure(EngineError::internal_failure));
    }
    if (!decision.accepted()) {
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          *this, command, sequence,
          make_rejection(sequence, order.instrument_id, domain::CommandType::new_order, decision));
    }

    auto planned = core::plan_matches(order, target->book);
    if (!planned) {
      preparation_active_ = false;
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          command, EngineResult::failure(EngineError::internal_failure));
    }
    if (!within_capacities(planned.plan, *target, false)) {
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          *this, command, sequence,
          make_rejection(sequence, order.instrument_id, domain::CommandType::new_order,
                         reject(domain::RejectReason::capacity_exceeded, order.order_id)));
    }

    std::vector<IdentityRemoval> removals;
    removals.reserve(planned.plan.trades.size());
    for (const auto& trade : planned.plan.trades) {
      if (trade.resting_remaining_after.value() != 0U) {
        continue;
      }
      const auto position = active_orders_.find(trade.resting_order_id);
      if (position == active_orders_.end()) {
        preparation_active_ = false;
        return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
            command, EngineResult::failure(EngineError::internal_failure));
      }
      removals.push_back({
          .order_id = trade.resting_order_id,
          .identity = position->second,
      });
    }

    ActiveOrderDirectory::node_type addition;
    if (planned.plan.residual_disposition == core::ResidualDisposition::rest) {
      if (active_orders_.size() == std::numeric_limits<std::size_t>::max()) {
        preparation_active_ = false;
        return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
            command, EngineResult::failure(EngineError::internal_failure));
      }
      // Reserve the destination bucket capacity before extracting a node from
      // the same map type. With no overlapping preparation, commit's
      // allocator-compatible node insertion cannot allocate or rehash.
      active_orders_.reserve(active_orders_.size() + 1U);
      ActiveOrderDirectory staging;
      const auto [position, inserted] =
          staging.emplace(order.order_id, Identity{
                                              .instrument_id = order.instrument_id,
                                              .client_id = order.client_id,
                                          });
      if (!inserted) {
        std::terminate();
      }
      addition = staging.extract(position);
    }

    auto prepared = const_cast<BookEntry*>(target)->executor.prepare_at(order, sequence);
    if (!prepared.has_value()) {
      preparation_active_ = false;
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          command, EngineResult::failure(EngineError::internal_failure));
    }
    if (prepared.rejected()) {
      if (!is_executor_capacity_rejection(prepared, domain::CommandType::new_order,
                                          order.order_id)) {
        preparation_active_ = false;
        return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
            command, EngineResult::failure(EngineError::internal_failure));
      }
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          *this, command, sequence, std::move(prepared), std::vector<IdentityRemoval>{},
          ActiveOrderDirectory::node_type{});
    }

    return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
        *this, command, sequence, std::move(prepared), std::move(removals), std::move(addition));
  } catch (...) {
    preparation_active_ = false;
    throw;
  }
}

std::unique_ptr<core::PreparedMultiInstrumentCommand::Impl>
MultiInstrumentEngine::Impl::prepare_state(const domain::CancelOrder& order) {
  const domain::Command command{order};
  if (preparation_active_) {
    return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
        command, EngineResult::failure(EngineError::internal_failure));
  }
  if (unavailable_sequence()) {
    return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
        command, EngineResult::failure(EngineError::sequence_exhausted));
  }

  preparation_active_ = true;
  try {
    const auto sequence = reserved_sequence();
    const BookEntry* target = nullptr;
    const auto decision = validate(order, target);
    if (!valid_target(target) && decision.accepted()) {
      preparation_active_ = false;
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          command, EngineResult::failure(EngineError::internal_failure));
    }
    if (!decision.accepted()) {
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          *this, command, sequence,
          make_rejection(sequence, order.instrument_id, domain::CommandType::cancel, decision));
    }

    const auto position = active_orders_.find(order.order_id);
    if (position == active_orders_.end()) {
      preparation_active_ = false;
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          command, EngineResult::failure(EngineError::internal_failure));
    }
    std::vector<IdentityRemoval> removals;
    removals.reserve(1U);
    removals.push_back({
        .order_id = order.order_id,
        .identity = position->second,
    });

    auto prepared = const_cast<BookEntry*>(target)->executor.prepare_at(order, sequence);
    if (!prepared.has_value() || prepared.rejected()) {
      preparation_active_ = false;
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          command, EngineResult::failure(EngineError::internal_failure));
    }

    return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
        *this, command, sequence, std::move(prepared), std::move(removals),
        ActiveOrderDirectory::node_type{});
  } catch (...) {
    preparation_active_ = false;
    throw;
  }
}

std::unique_ptr<core::PreparedMultiInstrumentCommand::Impl>
MultiInstrumentEngine::Impl::prepare_state(const domain::ReplaceOrder& order) {
  const domain::Command command{order};
  if (preparation_active_) {
    return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
        command, EngineResult::failure(EngineError::internal_failure));
  }
  if (unavailable_sequence()) {
    return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
        command, EngineResult::failure(EngineError::sequence_exhausted));
  }

  preparation_active_ = true;
  try {
    const auto sequence = reserved_sequence();
    const BookEntry* target = nullptr;
    const auto decision = validate(order, target);
    if (!valid_target(target) && decision.accepted()) {
      preparation_active_ = false;
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          command, EngineResult::failure(EngineError::internal_failure));
    }
    if (!decision.accepted()) {
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          *this, command, sequence,
          make_rejection(sequence, order.instrument_id, domain::CommandType::replace, decision));
    }

    const auto* old_node = target->book.find(order.old_order_id);
    if (old_node == nullptr) {
      preparation_active_ = false;
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          command, EngineResult::failure(EngineError::internal_failure));
    }
    const domain::NewOrder replacement{
        .client_id = old_node->client_id(),
        .order_id = order.new_order_id,
        .instrument_id = old_node->instrument_id(),
        .side = old_node->side(),
        .order_type = domain::OrderType::limit,
        .time_in_force = domain::TimeInForce::gtc,
        .limit_price = order.new_limit_price,
        .quantity = order.new_quantity,
    };
    auto planned = core::plan_matches(replacement, target->book);
    if (!planned) {
      preparation_active_ = false;
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          command, EngineResult::failure(EngineError::internal_failure));
    }
    if (!within_capacities(planned.plan, *target, true)) {
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          *this, command, sequence,
          make_rejection(sequence, order.instrument_id, domain::CommandType::replace,
                         reject(domain::RejectReason::capacity_exceeded, order.new_order_id)));
    }

    std::vector<IdentityRemoval> removals;
    removals.reserve(planned.plan.trades.size() + 1U);
    const auto old_position = active_orders_.find(order.old_order_id);
    if (old_position == active_orders_.end()) {
      preparation_active_ = false;
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          command, EngineResult::failure(EngineError::internal_failure));
    }
    removals.push_back({
        .order_id = order.old_order_id,
        .identity = old_position->second,
    });
    for (const auto& trade : planned.plan.trades) {
      if (trade.resting_remaining_after.value() != 0U) {
        continue;
      }
      const auto position = active_orders_.find(trade.resting_order_id);
      if (position == active_orders_.end()) {
        preparation_active_ = false;
        return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
            command, EngineResult::failure(EngineError::internal_failure));
      }
      removals.push_back({
          .order_id = trade.resting_order_id,
          .identity = position->second,
      });
    }

    ActiveOrderDirectory::node_type addition;
    if (planned.plan.residual_disposition == core::ResidualDisposition::rest) {
      if (active_orders_.size() == std::numeric_limits<std::size_t>::max()) {
        preparation_active_ = false;
        return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
            command, EngineResult::failure(EngineError::internal_failure));
      }
      // See the NewOrder path: both bucket capacity and the exact node are
      // owned before publication crosses the no-allocation commit boundary.
      active_orders_.reserve(active_orders_.size() + 1U);
      ActiveOrderDirectory staging;
      const auto [position, inserted] =
          staging.emplace(order.new_order_id, Identity{
                                                  .instrument_id = order.instrument_id,
                                                  .client_id = order.client_id,
                                              });
      if (!inserted) {
        std::terminate();
      }
      addition = staging.extract(position);
    }

    auto prepared = const_cast<BookEntry*>(target)->executor.prepare_at(order, sequence);
    if (!prepared.has_value()) {
      preparation_active_ = false;
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          command, EngineResult::failure(EngineError::internal_failure));
    }
    if (prepared.rejected()) {
      if (!is_executor_capacity_rejection(prepared, domain::CommandType::replace,
                                          order.new_order_id)) {
        preparation_active_ = false;
        return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
            command, EngineResult::failure(EngineError::internal_failure));
      }
      return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
          *this, command, sequence, std::move(prepared), std::vector<IdentityRemoval>{},
          ActiveOrderDirectory::node_type{});
    }

    return std::make_unique<core::PreparedMultiInstrumentCommand::Impl>(
        *this, command, sequence, std::move(prepared), std::move(removals), std::move(addition));
  } catch (...) {
    preparation_active_ = false;
    throw;
  }
}

core::PreparedMultiInstrumentCommand core::MultiInstrumentEngineAccess::prepare(
    MultiInstrumentEngine& engine, const domain::Command& command) {
  return PreparedMultiInstrumentCommand{std::visit(
      [&engine](const auto& value) { return engine.impl_->prepare_state(value); }, command)};
}

MultiInstrumentEngine::MultiInstrumentEngine(std::span<const InstrumentConfig> catalog,
                                             MultiInstrumentEngineConfig config)
    : impl_{std::make_unique<Impl>(catalog, config)} {}

MultiInstrumentEngine::~MultiInstrumentEngine() noexcept = default;

EngineResult MultiInstrumentEngine::execute(const domain::NewOrder& order) {
  return core::MultiInstrumentEngineAccess::prepare(*this, domain::Command{order}).commit();
}

EngineResult MultiInstrumentEngine::execute(const domain::CancelOrder& order) {
  return core::MultiInstrumentEngineAccess::prepare(*this, domain::Command{order}).commit();
}

EngineResult MultiInstrumentEngine::execute(const domain::ReplaceOrder& order) {
  return core::MultiInstrumentEngineAccess::prepare(*this, domain::Command{order}).commit();
}

EngineResult MultiInstrumentEngine::execute(const domain::Command& command) {
  return core::MultiInstrumentEngineAccess::prepare(*this, command).commit();
}

bool MultiInstrumentEngine::contains_instrument(domain::InstrumentId instrument_id) const noexcept {
  return impl_->find_book(instrument_id) != nullptr;
}

std::size_t MultiInstrumentEngine::active_order_count() const noexcept {
  return impl_->active_orders_.size();
}

std::optional<BookTop> MultiInstrumentEngine::top(
    domain::InstrumentId instrument_id) const noexcept {
  const auto* entry = impl_->find_book(instrument_id);
  if (entry == nullptr) {
    return std::nullopt;
  }
  const auto top = core::snapshot_top_of_book(entry->book);
  return BookTop{
      .best_bid = top.best_bid,
      .best_ask = top.best_ask,
  };
}

std::optional<InstrumentSnapshot> MultiInstrumentEngine::snapshot(
    domain::InstrumentId instrument_id) const {
  const auto* entry = impl_->find_book(instrument_id);
  if (entry == nullptr) {
    return std::nullopt;
  }
  if (!entry->book.validate_invariants() ||
      (entry->book.has_pending_preparation() && !impl_->preparation_active_)) {
    std::terminate();
  }
  return InstrumentSnapshot{
      .instrument_id = instrument_id,
      .active_order_count = as_u64(entry->book.active_order_count()),
      .bids = snapshot_side(entry->book.bids()),
      .asks = snapshot_side(entry->book.asks()),
  };
}

EngineSnapshot MultiInstrumentEngine::snapshot() const {
  if (!impl_->validate_invariants()) {
    std::terminate();
  }

  EngineSnapshot result{
      .semantics_version = atlaslob_semantics_version,
      .engine_config = impl_->config_,
      .catalog = {},
      .last_sequence = core::snapshot_last_sequence(impl_->sequencer_.next_sequence(),
                                                    impl_->sequencer_.exhausted()),
      .sequence_exhausted = impl_->sequencer_.exhausted(),
      .active_order_count = as_u64(impl_->active_orders_.size()),
      .instruments = {},
  };
  result.catalog.reserve(impl_->books_.size());
  result.instruments.reserve(impl_->books_.size());
  for (const auto& [instrument_id, entry] : impl_->books_) {
    result.catalog.push_back(entry->config);
    result.instruments.push_back({
        .instrument_id = instrument_id,
        .active_order_count = as_u64(entry->book.active_order_count()),
        .bids = snapshot_side(entry->book.bids()),
        .asks = snapshot_side(entry->book.asks()),
    });
  }
  return result;
}

Digest256 MultiInstrumentEngine::state_digest() const { return atlaslob::state_digest(snapshot()); }

domain::Sequence MultiInstrumentEngine::next_sequence() const noexcept {
  return impl_->sequencer_.next_sequence();
}

bool MultiInstrumentEngine::sequence_exhausted() const noexcept {
  return impl_->sequencer_.exhausted();
}

#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
bool MultiInstrumentEngine::validate_invariants_for_testing() const noexcept {
  return impl_->validate_invariants();
}

void MultiInstrumentEngine::set_next_sequence_for_testing(domain::Sequence next_sequence) {
  if (!impl_->active_orders_.empty()) {
    throw std::logic_error{"test sequence injection requires an empty engine"};
  }
  impl_->sequencer_.set_next_sequence_for_testing(next_sequence);
}

void MultiInstrumentEngine::set_before_event_allocation_hook_for_testing(
    domain::InstrumentId instrument_id, BeforeEventAllocationHook hook) {
  auto* entry = impl_->find_book(instrument_id);
  if (entry == nullptr) {
    throw std::invalid_argument{"unknown test instrument"};
  }
  entry->executor.set_before_event_allocation_hook_for_testing(hook);
}

void MultiInstrumentEngine::erase_active_identity_for_testing(domain::OrderId order_id) {
  if (impl_->active_orders_.erase(order_id) != 1U) {
    throw std::invalid_argument{"unknown test order"};
  }
}

void MultiInstrumentEngine::set_max_total_active_orders_for_testing(
    std::size_t max_total_active_orders) noexcept {
  impl_->config_.max_total_active_orders = max_total_active_orders;
}

void MultiInstrumentEngine::set_instrument_max_active_orders_for_testing(
    domain::InstrumentId instrument_id, std::size_t max_active_orders) {
  auto* entry = impl_->find_book(instrument_id);
  if (entry == nullptr) {
    throw std::invalid_argument{"unknown test instrument"};
  }
  entry->config.matching.max_active_orders = max_active_orders;
}

void MultiInstrumentEngine::set_sequence_state_for_testing(domain::Sequence next_sequence,
                                                           bool exhausted) noexcept {
  impl_->sequencer_.set_state_for_testing(next_sequence, exhausted);
}
#endif

}  // namespace atlaslob
