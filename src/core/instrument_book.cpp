#include "instrument_book.hpp"

#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace atlaslob::core {
namespace {

[[nodiscard]] InstrumentBookStatus make_status(
    InstrumentBookError error, StorageError storage_error = StorageError::none,
    ActiveOrderIndexError index_error = ActiveOrderIndexError::none,
    BookSideError side_error = BookSideError::none,
    PriceLevelError level_error = PriceLevelError::none) noexcept {
  return {
      .error = error,
      .storage_error = storage_error,
      .index_error = index_error,
      .side_error = side_error,
      .level_error = level_error,
  };
}

[[nodiscard]] RemovedOrder copy_order(const OrderNode& node) noexcept {
  return {
      .order_id = node.order_id(),
      .client_id = node.client_id(),
      .instrument_id = node.instrument_id(),
      .side = node.side(),
      .price = node.price(),
      .remaining_quantity = node.remaining_quantity(),
      .priority_sequence = node.priority_sequence(),
  };
}

[[nodiscard]] InstrumentBookStatus validate_rest_spec(const OrderNodeSpec& spec,
                                                      domain::InstrumentId instrument_id) noexcept {
  if (spec.order_id.value() == 0) {
    return make_status(InstrumentBookError::storage_failure, StorageError::invalid_order_id);
  }
  if (spec.client_id.value() == 0) {
    return make_status(InstrumentBookError::storage_failure, StorageError::invalid_client_id);
  }
  if (spec.instrument_id.value() == 0) {
    return make_status(InstrumentBookError::invalid_instrument_id,
                       StorageError::invalid_instrument_id);
  }
  if (spec.instrument_id != instrument_id) {
    return make_status(InstrumentBookError::instrument_mismatch);
  }
  if (!domain::is_valid(spec.side)) {
    return make_status(InstrumentBookError::invalid_side, StorageError::invalid_side);
  }
  if (spec.price.value() <= 0) {
    return make_status(InstrumentBookError::storage_failure, StorageError::invalid_price);
  }
  if (spec.remaining_quantity.value() == 0) {
    return make_status(InstrumentBookError::storage_failure,
                       StorageError::invalid_remaining_quantity);
  }
  if (spec.priority_sequence.value() == 0) {
    return make_status(InstrumentBookError::storage_failure,
                       StorageError::invalid_priority_sequence);
  }
  return {};
}

[[nodiscard]] bool would_cross(const OrderNodeSpec& spec, const BidBookSide& bids,
                               const AskBookSide& asks) noexcept {
  if (spec.side == domain::Side::buy) {
    const auto* const best_ask = asks.best_level();
    return best_ask != nullptr && spec.price >= best_ask->price();
  }
  const auto* const best_bid = bids.best_level();
  return best_bid != nullptr && spec.price <= best_bid->price();
}

[[nodiscard]] InstrumentBookStatus validate_append_target(const PriceLevel& target,
                                                          const OrderNode& node) noexcept {
  if (!target.validate_invariants()) {
    return make_status(InstrumentBookError::book_invariant_violation);
  }
  if (target.price() != node.price()) {
    return make_status(InstrumentBookError::price_level_failure, StorageError::none,
                       ActiveOrderIndexError::none, BookSideError::none,
                       PriceLevelError::price_mismatch);
  }
  if (target.tail() != nullptr && node.priority_sequence() <= target.tail()->priority_sequence()) {
    return make_status(InstrumentBookError::price_level_failure, StorageError::none,
                       ActiveOrderIndexError::none, BookSideError::none,
                       PriceLevelError::nonmonotonic_priority);
  }
  if (target.aggregate_quantity().value() >
      std::numeric_limits<std::uint64_t>::max() - node.remaining_quantity().value()) {
    return make_status(InstrumentBookError::price_level_failure, StorageError::none,
                       ActiveOrderIndexError::none, BookSideError::none,
                       PriceLevelError::aggregate_overflow);
  }
  if (target.order_count() == std::numeric_limits<std::size_t>::max()) {
    return make_status(InstrumentBookError::price_level_failure, StorageError::none,
                       ActiveOrderIndexError::none, BookSideError::none,
                       PriceLevelError::order_count_overflow);
  }
  return {};
}

[[nodiscard]] InstrumentBookStatus validate_replace_append_target(
    const PriceLevel& target, const OrderNode& residual, const OrderNode& old_order) noexcept {
  if (target.price() != old_order.price() || old_order.price_level() != &target) {
    return validate_append_target(target, residual);
  }
  if (!target.validate_invariants()) {
    return make_status(InstrumentBookError::book_invariant_violation);
  }
  if (target.price() != residual.price()) {
    return make_status(InstrumentBookError::price_level_failure, StorageError::none,
                       ActiveOrderIndexError::none, BookSideError::none,
                       PriceLevelError::price_mismatch);
  }
  if (target.order_count() == 0U ||
      target.aggregate_quantity().value() < old_order.remaining_quantity().value()) {
    return make_status(InstrumentBookError::book_invariant_violation);
  }

  const auto projected_count = target.order_count() - 1U;
  const auto projected_aggregate =
      target.aggregate_quantity().value() - old_order.remaining_quantity().value();
  const auto* const projected_tail =
      target.tail() == &old_order ? old_order.previous() : target.tail();
  if (projected_tail != nullptr &&
      residual.priority_sequence() <= projected_tail->priority_sequence()) {
    return make_status(InstrumentBookError::price_level_failure, StorageError::none,
                       ActiveOrderIndexError::none, BookSideError::none,
                       PriceLevelError::nonmonotonic_priority);
  }
  if (projected_aggregate >
      std::numeric_limits<std::uint64_t>::max() - residual.remaining_quantity().value()) {
    return make_status(InstrumentBookError::price_level_failure, StorageError::none,
                       ActiveOrderIndexError::none, BookSideError::none,
                       PriceLevelError::aggregate_overflow);
  }
  if (projected_count == std::numeric_limits<std::size_t>::max()) {
    return make_status(InstrumentBookError::price_level_failure, StorageError::none,
                       ActiveOrderIndexError::none, BookSideError::none,
                       PriceLevelError::order_count_overflow);
  }
  return {};
}

[[nodiscard]] const PrevalidatedBookReduction* find_reduction(
    const OrderNode& node, std::span<const PrevalidatedBookReduction> reductions) noexcept {
  for (const auto& reduction : reductions) {
    if (reduction.node == &node) {
      return &reduction;
    }
  }
  return nullptr;
}

[[nodiscard]] bool survives_batch(const PriceLevel& level,
                                  std::span<const PrevalidatedBookReduction> reductions) noexcept {
  for (const auto* node = level.head(); node != nullptr; node = node->next()) {
    const auto* const reduction = find_reduction(*node, reductions);
    if (reduction == nullptr || reduction->remaining_after.value() != 0U) {
      return true;
    }
  }
  return false;
}

template <domain::Side RestingSide>
[[nodiscard]] std::optional<domain::PriceTicks> projected_best_price(
    const BookSide<RestingSide>& side,
    std::span<const PrevalidatedBookReduction> reductions) noexcept {
  for (const PriceLevel& level : side) {
    if (survives_batch(level, reductions)) {
      return level.price();
    }
  }
  return std::nullopt;
}

}  // namespace

InstrumentBook::InstrumentBook(domain::InstrumentId instrument_id) : instrument_id_{instrument_id} {
  if (instrument_id_.value() == 0) {
    throw std::invalid_argument{"InstrumentBook requires a nonzero instrument ID"};
  }
}

InstrumentBook::~InstrumentBook() noexcept { drain(); }

InstrumentBook::PreparedRest::PreparedRest(InstrumentBook& owner, OrderNode& node,
                                           std::unique_ptr<PriceLevel> staging_level,
                                           PreparedLevel prepared_level,
                                           OrderNode* replacement_old) noexcept
    : owner_{&owner},
      node_{&node},
      staging_level_{std::move(staging_level)},
      prepared_level_{std::move(prepared_level)},
      replacement_old_{replacement_old} {}

InstrumentBook::PreparedRest::PreparedRest(PreparedRest&& other) noexcept
    : owner_{std::exchange(other.owner_, nullptr)},
      node_{std::exchange(other.node_, nullptr)},
      staging_level_{std::move(other.staging_level_)},
      prepared_level_{std::move(other.prepared_level_)},
      replacement_old_{std::exchange(other.replacement_old_, nullptr)},
      status_{std::exchange(other.status_, {})} {}

InstrumentBook::PreparedRest& InstrumentBook::PreparedRest::operator=(
    PreparedRest&& other) noexcept {
  if (this != &other) {
    rollback();
    owner_ = std::exchange(other.owner_, nullptr);
    node_ = std::exchange(other.node_, nullptr);
    staging_level_ = std::move(other.staging_level_);
    prepared_level_ = std::move(other.prepared_level_);
    replacement_old_ = std::exchange(other.replacement_old_, nullptr);
    status_ = std::exchange(other.status_, {});
  }
  return *this;
}

InstrumentBook::PreparedRest::~PreparedRest() noexcept { rollback(); }

void InstrumentBook::PreparedRest::rollback() noexcept {
  if (owner_ == nullptr) {
    return;
  }
  if (node_ == nullptr || staging_level_ == nullptr || owner_->pending_node_ != node_ ||
      owner_->pending_level_ != staging_level_.get()) {
    std::terminate();
  }

  const auto level_error = staging_level_->erase(*node_);
  if (level_error != PriceLevelError::none) {
    std::terminate();
  }
  const auto index_error = owner_->index_.erase(*node_);
  if (index_error != ActiveOrderIndexError::none) {
    std::terminate();
  }
  const auto storage_error = owner_->storage_.destroy(*node_);
  if (storage_error != StorageError::none) {
    std::terminate();
  }

  auto* const owner = std::exchange(owner_, nullptr);
  owner->pending_node_ = nullptr;
  owner->pending_level_ = nullptr;
  node_ = nullptr;
  staging_level_.reset();
  prepared_level_ = std::monostate{};
  replacement_old_ = nullptr;
  owner->enforce_postconditions();
}

RestOrderResult InstrumentBook::PreparedRest::commit() noexcept {
  if (!has_value()) {
    return {
        .node = nullptr,
        .status =
            status_.valid() ? make_status(InstrumentBookError::book_invariant_violation) : status_,
    };
  }
  if (replacement_old_ != nullptr) {
    return {
        .node = nullptr,
        .status = make_status(InstrumentBookError::replacement_transaction_required),
    };
  }
  if (owner_->pending_node_ != node_ || owner_->pending_level_ != staging_level_.get() ||
      !owner_->validate_invariants()) {
    std::terminate();
  }

  const auto staging_result = staging_level_->validate_invariants();
  if (!staging_result || staging_level_->order_count() != 1U || staging_level_->head() != node_ ||
      staging_level_->tail() != node_ || node_->price_level() != staging_level_.get()) {
    std::terminate();
  }

  PriceLevel* target = nullptr;
  BookSideError publish_error = BookSideError::none;
  if (node_->side() == domain::Side::buy) {
    if (!std::holds_alternative<BidBookSide::DetachedLevel>(prepared_level_)) {
      std::terminate();
    }
    auto& prepared = std::get<BidBookSide::DetachedLevel>(prepared_level_);
    target = owner_->bids_.find_level(node_->price());
    if (target == nullptr) {
      target = prepared.level();
    }
    const auto* const best_ask = owner_->asks_.best_level();
    if (best_ask != nullptr && node_->price() >= best_ask->price()) {
      std::terminate();
    }
  } else if (node_->side() == domain::Side::sell) {
    if (!std::holds_alternative<AskBookSide::DetachedLevel>(prepared_level_)) {
      std::terminate();
    }
    auto& prepared = std::get<AskBookSide::DetachedLevel>(prepared_level_);
    target = owner_->asks_.find_level(node_->price());
    if (target == nullptr) {
      target = prepared.level();
    }
    const auto* const best_bid = owner_->bids_.best_level();
    if (best_bid != nullptr && node_->price() <= best_bid->price()) {
      std::terminate();
    }
  } else {
    std::terminate();
  }

  if (target == nullptr || !validate_append_target(*target, *node_)) {
    std::terminate();
  }

  const auto staging_erase_error = staging_level_->erase(*node_);
  if (staging_erase_error != PriceLevelError::none) {
    std::terminate();
  }
  const auto append_error = target->append(*node_);
  if (append_error != PriceLevelError::none) {
    std::terminate();
  }

  if (node_->side() == domain::Side::buy && owner_->bids_.find_level(node_->price()) == nullptr) {
    publish_error = std::get<BidBookSide::DetachedLevel>(prepared_level_).publish();
  } else if (node_->side() == domain::Side::sell &&
             owner_->asks_.find_level(node_->price()) == nullptr) {
    publish_error = std::get<AskBookSide::DetachedLevel>(prepared_level_).publish();
  }
  if (publish_error != BookSideError::none) {
    std::terminate();
  }

  auto* const committed_node = node_;
  auto* const owner = std::exchange(owner_, nullptr);
  owner->pending_node_ = nullptr;
  owner->pending_level_ = nullptr;
  node_ = nullptr;
  staging_level_.reset();
  prepared_level_ = std::monostate{};
  replacement_old_ = nullptr;
  owner->enforce_postconditions();
  return {.node = committed_node, .status = {}};
}

InstrumentBook::PreparedRest InstrumentBook::prepare_rest(const OrderNodeSpec& spec) {
  return prepare_rest_impl(spec, nullptr);
}

InstrumentBook::PreparedRest InstrumentBook::prepare_replace_rest(const OrderNodeSpec& spec,
                                                                  OrderNode& old_order) {
  return prepare_rest_impl(spec, &old_order);
}

InstrumentBook::PreparedRest InstrumentBook::prepare_rest_impl(const OrderNodeSpec& spec,
                                                               OrderNode* replacement_old) {
  const auto spec_status = validate_rest_spec(spec, instrument_id_);
  if (!spec_status) {
    return PreparedRest{spec_status};
  }
  if (!validate_invariants()) {
    return PreparedRest{make_status(InstrumentBookError::book_invariant_violation)};
  }
  if (pending_node_ != nullptr || pending_level_ != nullptr) {
    return PreparedRest{make_status(InstrumentBookError::preparation_in_progress)};
  }
  if (replacement_old != nullptr) {
    const auto old_status = preflight_node(*replacement_old);
    if (!old_status) {
      return PreparedRest{old_status};
    }
    if (spec.order_id == replacement_old->order_id() ||
        spec.client_id != replacement_old->client_id() ||
        spec.instrument_id != replacement_old->instrument_id() ||
        spec.side != replacement_old->side()) {
      return PreparedRest{make_status(InstrumentBookError::replacement_mismatch)};
    }
  }
  if (storage_.find(spec.order_id) != nullptr || index_.find(spec.order_id) != nullptr) {
    return PreparedRest{make_status(InstrumentBookError::duplicate_order_id,
                                    StorageError::duplicate_order_id,
                                    ActiveOrderIndexError::duplicate_order_id)};
  }

  PreparedRest::PreparedLevel prepared_level;
  if (spec.side == domain::Side::buy) {
    auto prepared = bids_.prepare_detached_level(spec.price);
    if (!prepared) {
      return PreparedRest{make_status(InstrumentBookError::book_side_failure, StorageError::none,
                                      ActiveOrderIndexError::none, prepared.error())};
    }
    prepared_level.emplace<BidBookSide::DetachedLevel>(std::move(prepared));
  } else {
    auto prepared = asks_.prepare_detached_level(spec.price);
    if (!prepared) {
      return PreparedRest{make_status(InstrumentBookError::book_side_failure, StorageError::none,
                                      ActiveOrderIndexError::none, prepared.error())};
    }
    prepared_level.emplace<AskBookSide::DetachedLevel>(std::move(prepared));
  }

  auto staging_level = std::make_unique<PriceLevel>(spec.price);
  const auto created = storage_.create(spec);
  if (!created) {
    const auto error = created.error == StorageError::duplicate_order_id
                           ? InstrumentBookError::duplicate_order_id
                           : InstrumentBookError::storage_failure;
    return PreparedRest{make_status(error, created.error)};
  }

  auto* const node = created.node;
  const auto cleanup_node = [this, node, &staging_level](bool indexed) noexcept {
    if (node->is_linked() &&
        (staging_level == nullptr || staging_level->erase(*node) != PriceLevelError::none)) {
      std::terminate();
    }
    if (indexed && index_.erase(*node) != ActiveOrderIndexError::none) {
      std::terminate();
    }
    if (storage_.destroy(*node) != StorageError::none) {
      std::terminate();
    }
  };

  if (const auto* existing = spec.side == domain::Side::buy ? bids_.find_level(spec.price)
                                                            : asks_.find_level(spec.price);
      existing != nullptr) {
    const auto target_status = replacement_old == nullptr ? validate_append_target(*existing, *node)
                                                          : validate_replace_append_target(
                                                                *existing, *node, *replacement_old);
    if (!target_status) {
      cleanup_node(false);
      return PreparedRest{target_status};
    }
  }

  const auto append_error = staging_level->append(*node);
  if (append_error != PriceLevelError::none) {
    cleanup_node(false);
    return PreparedRest{make_status(InstrumentBookError::price_level_failure, StorageError::none,
                                    ActiveOrderIndexError::none, BookSideError::none,
                                    append_error)};
  }

  try {
    const auto index_error = index_.insert(*node);
    if (index_error != ActiveOrderIndexError::none) {
      cleanup_node(false);
      return PreparedRest{
          make_status(InstrumentBookError::index_failure, StorageError::none, index_error)};
    }
  } catch (...) {
    cleanup_node(false);
    throw;
  }

  PreparedRest result{*this, *node, std::move(staging_level), std::move(prepared_level),
                      replacement_old};
  pending_node_ = node;
  pending_level_ = result.staging_level_.get();
  enforce_postconditions();
  return result;
}

RestOrderResult InstrumentBook::rest(const OrderNodeSpec& spec) {
  const auto spec_status = validate_rest_spec(spec, instrument_id_);
  if (!spec_status) {
    return {.node = nullptr, .status = spec_status};
  }
  if (!validate_invariants()) {
    return {
        .node = nullptr,
        .status = make_status(InstrumentBookError::book_invariant_violation),
    };
  }
  if (pending_node_ != nullptr || pending_level_ != nullptr) {
    return {
        .node = nullptr,
        .status = make_status(InstrumentBookError::preparation_in_progress),
    };
  }
  if (storage_.find(spec.order_id) != nullptr || index_.find(spec.order_id) != nullptr) {
    return {
        .node = nullptr,
        .status =
            make_status(InstrumentBookError::duplicate_order_id, StorageError::duplicate_order_id,
                        ActiveOrderIndexError::duplicate_order_id),
    };
  }
  if (would_cross(spec, bids_, asks_)) {
    return {
        .node = nullptr,
        .status = make_status(InstrumentBookError::would_cross_book),
    };
  }

  auto prepared = prepare_rest(spec);
  if (!prepared) {
    return {.node = nullptr, .status = prepared.status()};
  }
  return prepared.commit();
}

OrderNode* InstrumentBook::find(domain::OrderId order_id) noexcept {
  auto* const node = index_.find(order_id);
  return node == pending_node_ ? nullptr : node;
}

const OrderNode* InstrumentBook::find(domain::OrderId order_id) const noexcept {
  const auto* const node = index_.find(order_id);
  return node == pending_node_ ? nullptr : node;
}

InstrumentBookStatus InstrumentBook::preflight_node(const OrderNode& node) const noexcept {
  if (!validate_invariants()) {
    return make_status(InstrumentBookError::book_invariant_violation);
  }
  if (!storage_.owns(node)) {
    return make_status(InstrumentBookError::node_not_owned);
  }
  const auto* const indexed = index_.find(node.order_id());
  if (indexed == nullptr || indexed != &node) {
    return make_status(InstrumentBookError::node_not_indexed);
  }
  if (node.instrument_id() != instrument_id_) {
    return make_status(InstrumentBookError::instrument_mismatch);
  }
  if (!domain::is_valid(node.side())) {
    return make_status(InstrumentBookError::side_mismatch);
  }
  if (!node.is_linked() || node.price_level() == nullptr) {
    return make_status(InstrumentBookError::node_not_linked);
  }

  const auto* const level = node.price_level();
  const auto* const expected_level = node.side() == domain::Side::buy
                                         ? bids_.find_level(node.price())
                                         : asks_.find_level(node.price());
  if (expected_level != level || level->price() != node.price()) {
    return make_status(InstrumentBookError::price_level_mismatch);
  }
  if (!level->validate_invariants()) {
    return make_status(InstrumentBookError::book_invariant_violation);
  }

  if ((node.previous() == nullptr && level->head() != &node) ||
      (node.previous() != nullptr && node.previous()->next() != &node) ||
      (node.next() == nullptr && level->tail() != &node) ||
      (node.next() != nullptr && node.next()->previous() != &node)) {
    return make_status(InstrumentBookError::price_level_mismatch);
  }

  return {};
}

InstrumentBookStatus InstrumentBook::reduce_remaining(OrderNode& node,
                                                      domain::Quantity reduction) noexcept {
  const auto preflight = preflight_node(node);
  if (!preflight) {
    return preflight;
  }

  auto* const level = node.side() == domain::Side::buy ? bids_.find_level(node.price())
                                                       : asks_.find_level(node.price());
  if (level == nullptr || level != node.price_level()) {
    return make_status(InstrumentBookError::price_level_mismatch);
  }
  const auto error = level->reduce_remaining(node, reduction);
  if (error != PriceLevelError::none) {
    return make_status(InstrumentBookError::price_level_failure, StorageError::none,
                       ActiveOrderIndexError::none, BookSideError::none, error);
  }
  enforce_postconditions();
  return {};
}

RemoveOrderResult InstrumentBook::remove(OrderNode& node) noexcept {
  const auto preflight = preflight_node(node);
  if (!preflight) {
    return {.order = {}, .status = preflight};
  }
  return remove_prevalidated(node);
}

RemoveOrderResult InstrumentBook::remove_prevalidated(OrderNode& node) noexcept {
  const auto removed = copy_order(node);
  auto* const level = node.side() == domain::Side::buy ? bids_.find_level(node.price())
                                                       : asks_.find_level(node.price());
  if (level == nullptr || level != node.price_level()) {
    return {
        .order = {},
        .status = make_status(InstrumentBookError::price_level_mismatch),
    };
  }
  const auto level_error = level->erase(node);
  if (level_error != PriceLevelError::none) {
    return {
        .order = {},
        .status = make_status(InstrumentBookError::price_level_failure, StorageError::none,
                              ActiveOrderIndexError::none, BookSideError::none, level_error),
    };
  }

  const auto index_error = index_.erase(node);
  if (index_error != ActiveOrderIndexError::none) {
    std::terminate();
  }

  if (level->empty()) {
    const auto side_error =
        removed.side == domain::Side::buy ? bids_.erase_level(*level) : asks_.erase_level(*level);
    if (side_error != BookSideError::none) {
      std::terminate();
    }
  }

  const auto storage_error = storage_.destroy(node);
  if (storage_error != StorageError::none) {
    std::terminate();
  }

  enforce_postconditions();
  return {.order = removed, .status = {}};
}

RemoveOrderResult InstrumentBook::cancel(domain::OrderId order_id) noexcept {
  if (!validate_invariants()) {
    return {
        .order = {},
        .status = make_status(InstrumentBookError::book_invariant_violation),
    };
  }
  auto* const node = find(order_id);
  if (node == nullptr) {
    return {
        .order = {},
        .status = make_status(InstrumentBookError::unknown_order_id),
    };
  }
  return remove_prevalidated(*node);
}

PrevalidatedBatchStatus InstrumentBook::apply_prevalidated_batch(
    std::span<const PrevalidatedBookReduction> reductions, PreparedRest* prepared_rest) noexcept {
  const auto fail = [](PrevalidatedBatchError error, std::size_t failing_reduction = 0U) noexcept {
    return PrevalidatedBatchStatus{
        .error = error,
        .failing_reduction = failing_reduction,
    };
  };

  if (!validate_invariants()) {
    return fail(PrevalidatedBatchError::book_invariant_violation);
  }

  if (prepared_rest == nullptr) {
    if (pending_node_ != nullptr || pending_level_ != nullptr) {
      return fail(PrevalidatedBatchError::preparation_mismatch);
    }
  } else {
    if (!prepared_rest->has_value() || prepared_rest->owner_ != this ||
        prepared_rest->node_ != pending_node_ ||
        prepared_rest->staging_level_.get() != pending_level_ ||
        prepared_rest->replacement_old_ != nullptr) {
      return fail(PrevalidatedBatchError::preparation_mismatch);
    }

    const auto staging_result = prepared_rest->staging_level_->validate_invariants();
    if (!staging_result || prepared_rest->staging_level_->order_count() != 1U ||
        prepared_rest->staging_level_->head() != prepared_rest->node_ ||
        prepared_rest->staging_level_->tail() != prepared_rest->node_ ||
        prepared_rest->node_->price_level() != prepared_rest->staging_level_.get()) {
      return fail(PrevalidatedBatchError::preparation_mismatch);
    }
  }

  for (std::size_t index = 0U; index < reductions.size(); ++index) {
    const auto& reduction = reductions[index];
    const auto* const node = reduction.node;
    if (node == nullptr || node == pending_node_ || storage_.find(reduction.order_id) != node ||
        index_.find(reduction.order_id) != node || find(reduction.order_id) != node) {
      return fail(PrevalidatedBatchError::invalid_binding, index);
    }
    if (node->order_id() != reduction.order_id || node->client_id() != reduction.client_id ||
        node->instrument_id() != instrument_id_ || node->side() != reduction.side ||
        node->price() != reduction.price ||
        node->remaining_quantity() != reduction.remaining_before ||
        node->priority_sequence() != reduction.priority_sequence || !node->is_linked() ||
        node->price_level() == nullptr) {
      return fail(PrevalidatedBatchError::invalid_binding, index);
    }

    const auto* const expected_level = reduction.side == domain::Side::buy
                                           ? bids_.find_level(reduction.price)
                                           : asks_.find_level(reduction.price);
    if (!domain::is_valid(reduction.side) || expected_level == nullptr ||
        expected_level != node->price_level()) {
      return fail(PrevalidatedBatchError::invalid_binding, index);
    }

    if (reduction.reduction.value() == 0U || reduction.reduction > reduction.remaining_before ||
        reduction.remaining_after.value() !=
            reduction.remaining_before.value() - reduction.reduction.value()) {
      return fail(PrevalidatedBatchError::invalid_reduction, index);
    }

    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (reductions[previous].node == node ||
          reductions[previous].order_id == reduction.order_id) {
        return fail(PrevalidatedBatchError::duplicate_binding, index);
      }
    }

    if (prepared_rest != nullptr && reduction.side == prepared_rest->node_->side()) {
      return fail(PrevalidatedBatchError::preparation_mismatch, index);
    }
  }

  if (prepared_rest != nullptr) {
    const auto* const residual = prepared_rest->node_;
    PriceLevel* target = nullptr;
    if (residual->side() == domain::Side::buy) {
      if (!std::holds_alternative<BidBookSide::DetachedLevel>(prepared_rest->prepared_level_)) {
        return fail(PrevalidatedBatchError::preparation_mismatch);
      }
      auto& detached = std::get<BidBookSide::DetachedLevel>(prepared_rest->prepared_level_);
      if (!detached) {
        return fail(PrevalidatedBatchError::preparation_mismatch);
      }
      target = bids_.find_level(residual->price());
      if (target == nullptr) {
        target = detached.level();
      }
      const auto projected_ask = projected_best_price(asks_, reductions);
      if (projected_ask.has_value() && residual->price() >= projected_ask.value()) {
        return fail(PrevalidatedBatchError::residual_would_cross);
      }
    } else if (residual->side() == domain::Side::sell) {
      if (!std::holds_alternative<AskBookSide::DetachedLevel>(prepared_rest->prepared_level_)) {
        return fail(PrevalidatedBatchError::preparation_mismatch);
      }
      auto& detached = std::get<AskBookSide::DetachedLevel>(prepared_rest->prepared_level_);
      if (!detached) {
        return fail(PrevalidatedBatchError::preparation_mismatch);
      }
      target = asks_.find_level(residual->price());
      if (target == nullptr) {
        target = detached.level();
      }
      const auto projected_bid = projected_best_price(bids_, reductions);
      if (projected_bid.has_value() && residual->price() <= projected_bid.value()) {
        return fail(PrevalidatedBatchError::residual_would_cross);
      }
    } else {
      return fail(PrevalidatedBatchError::preparation_mismatch);
    }

    if (target == nullptr || !validate_append_target(*target, *residual)) {
      return fail(PrevalidatedBatchError::residual_append_failure);
    }
  }

  // No recoverable error may cross this boundary. Every component operation
  // below was made infallible by the complete preflight above.
  for (const auto& reduction : reductions) {
    apply_reduction_no_check(reduction);
  }
  if (prepared_rest != nullptr) {
    static_cast<void>(commit_prepared_no_check(*prepared_rest));
  }

  // This is deliberately the only whole-book invariant check after mutation.
  if (!validate_invariants()) {
    std::terminate();
  }
  return {};
}

PrevalidatedBatchStatus InstrumentBook::apply_prevalidated_replace_batch(
    const PrevalidatedBookReduction& old_reduction,
    std::span<const PrevalidatedBookReduction> passive_reductions,
    PreparedRest* prepared_rest) noexcept {
  const auto fail = [](PrevalidatedBatchError error, std::size_t failing_reduction = 0U) noexcept {
    return PrevalidatedBatchStatus{
        .error = error,
        .failing_reduction = failing_reduction,
    };
  };
  const auto validate_binding =
      [this, &fail](const PrevalidatedBookReduction& reduction,
                    std::size_t failing_reduction) noexcept -> PrevalidatedBatchStatus {
    const auto* const node = reduction.node;
    if (node == nullptr || node == pending_node_ || storage_.find(reduction.order_id) != node ||
        index_.find(reduction.order_id) != node || find(reduction.order_id) != node) {
      return fail(PrevalidatedBatchError::invalid_binding, failing_reduction);
    }
    if (node->order_id() != reduction.order_id || node->client_id() != reduction.client_id ||
        node->instrument_id() != instrument_id_ || node->side() != reduction.side ||
        node->price() != reduction.price ||
        node->remaining_quantity() != reduction.remaining_before ||
        node->priority_sequence() != reduction.priority_sequence || !node->is_linked() ||
        node->price_level() == nullptr) {
      return fail(PrevalidatedBatchError::invalid_binding, failing_reduction);
    }

    const auto* const expected_level = reduction.side == domain::Side::buy
                                           ? bids_.find_level(reduction.price)
                                           : asks_.find_level(reduction.price);
    if (!domain::is_valid(reduction.side) || expected_level == nullptr ||
        expected_level != node->price_level()) {
      return fail(PrevalidatedBatchError::invalid_binding, failing_reduction);
    }
    if (reduction.reduction.value() == 0U || reduction.reduction > reduction.remaining_before ||
        reduction.remaining_after.value() !=
            reduction.remaining_before.value() - reduction.reduction.value()) {
      return fail(PrevalidatedBatchError::invalid_reduction, failing_reduction);
    }
    return {};
  };

  if (!validate_invariants()) {
    return fail(PrevalidatedBatchError::book_invariant_violation);
  }

  if (prepared_rest == nullptr) {
    if (pending_node_ != nullptr || pending_level_ != nullptr) {
      return fail(PrevalidatedBatchError::preparation_mismatch);
    }
  } else {
    if (!prepared_rest->has_value() || prepared_rest->owner_ != this ||
        prepared_rest->node_ != pending_node_ ||
        prepared_rest->staging_level_.get() != pending_level_ ||
        prepared_rest->replacement_old_ != old_reduction.node) {
      return fail(PrevalidatedBatchError::preparation_mismatch);
    }

    const auto staging_result = prepared_rest->staging_level_->validate_invariants();
    if (!staging_result || prepared_rest->staging_level_->order_count() != 1U ||
        prepared_rest->staging_level_->head() != prepared_rest->node_ ||
        prepared_rest->staging_level_->tail() != prepared_rest->node_ ||
        prepared_rest->node_->price_level() != prepared_rest->staging_level_.get()) {
      return fail(PrevalidatedBatchError::preparation_mismatch);
    }
  }

  if (const auto old_status = validate_binding(old_reduction, 0U); !old_status) {
    return old_status;
  }
  if (old_reduction.remaining_after.value() != 0U ||
      old_reduction.reduction != old_reduction.remaining_before) {
    return fail(PrevalidatedBatchError::invalid_reduction);
  }

  for (std::size_t index = 0U; index < passive_reductions.size(); ++index) {
    const auto& reduction = passive_reductions[index];
    const auto failing_reduction = index + 1U;
    if (const auto status = validate_binding(reduction, failing_reduction); !status) {
      return status;
    }
    if (reduction.side == old_reduction.side || reduction.node == old_reduction.node ||
        reduction.order_id == old_reduction.order_id) {
      return fail(PrevalidatedBatchError::preparation_mismatch, failing_reduction);
    }
    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (passive_reductions[previous].node == reduction.node ||
          passive_reductions[previous].order_id == reduction.order_id) {
        return fail(PrevalidatedBatchError::duplicate_binding, failing_reduction);
      }
    }
  }

  if (prepared_rest != nullptr) {
    const auto* const residual = prepared_rest->node_;
    const auto* const old_order = old_reduction.node;
    if (residual == nullptr || old_order == nullptr || residual->side() != old_order->side() ||
        residual->client_id() != old_order->client_id() ||
        residual->instrument_id() != old_order->instrument_id() ||
        residual->order_id() == old_order->order_id()) {
      return fail(PrevalidatedBatchError::preparation_mismatch);
    }

    PriceLevel* visible_target = nullptr;
    PriceLevel* detached_target = nullptr;
    if (residual->side() == domain::Side::buy) {
      if (!std::holds_alternative<BidBookSide::DetachedLevel>(prepared_rest->prepared_level_)) {
        return fail(PrevalidatedBatchError::preparation_mismatch);
      }
      auto& detached = std::get<BidBookSide::DetachedLevel>(prepared_rest->prepared_level_);
      if (!detached) {
        return fail(PrevalidatedBatchError::preparation_mismatch);
      }
      visible_target = bids_.find_level(residual->price());
      detached_target = detached.level();
      const auto projected_ask = projected_best_price(asks_, passive_reductions);
      if (projected_ask.has_value() && residual->price() >= projected_ask.value()) {
        return fail(PrevalidatedBatchError::residual_would_cross);
      }
    } else if (residual->side() == domain::Side::sell) {
      if (!std::holds_alternative<AskBookSide::DetachedLevel>(prepared_rest->prepared_level_)) {
        return fail(PrevalidatedBatchError::preparation_mismatch);
      }
      auto& detached = std::get<AskBookSide::DetachedLevel>(prepared_rest->prepared_level_);
      if (!detached) {
        return fail(PrevalidatedBatchError::preparation_mismatch);
      }
      visible_target = asks_.find_level(residual->price());
      detached_target = detached.level();
      const auto projected_bid = projected_best_price(bids_, passive_reductions);
      if (projected_bid.has_value() && residual->price() <= projected_bid.value()) {
        return fail(PrevalidatedBatchError::residual_would_cross);
      }
    } else {
      return fail(PrevalidatedBatchError::preparation_mismatch);
    }

    const bool removes_visible_target = visible_target != nullptr &&
                                        visible_target == old_order->price_level() &&
                                        visible_target->order_count() == 1U;
    InstrumentBookStatus append_status{};
    if (visible_target == nullptr || removes_visible_target) {
      append_status = detached_target == nullptr
                          ? make_status(InstrumentBookError::book_invariant_violation)
                          : validate_append_target(*detached_target, *residual);
    } else {
      append_status = validate_replace_append_target(*visible_target, *residual, *old_order);
    }
    if (!append_status) {
      return fail(PrevalidatedBatchError::residual_append_failure);
    }
  }

  // No recoverable error may cross this boundary. The exact old order is
  // terminally removed first, followed by passive fills and optional residual
  // publication. Each terminal removal preserves unlink -> index erase ->
  // empty-level erase -> storage destruction.
  apply_reduction_no_check(old_reduction);
  for (const auto& reduction : passive_reductions) {
    apply_reduction_no_check(reduction);
  }
  if (prepared_rest != nullptr) {
    static_cast<void>(commit_prepared_no_check(*prepared_rest));
  }

  if (!validate_invariants()) {
    std::terminate();
  }
  return {};
}

void InstrumentBook::apply_reduction_no_check(const PrevalidatedBookReduction& reduction) noexcept {
  auto& node = *reduction.node;
  auto* const level = reduction.side == domain::Side::buy ? bids_.find_level(reduction.price)
                                                          : asks_.find_level(reduction.price);
  if (level == nullptr) {
    std::terminate();
  }

  if (reduction.remaining_after.value() != 0U) {
    if (level->reduce_remaining(node, reduction.reduction) != PriceLevelError::none) {
      std::terminate();
    }
    return;
  }

  if (level->erase(node) != PriceLevelError::none) {
    std::terminate();
  }
  if (index_.erase(node) != ActiveOrderIndexError::none) {
    std::terminate();
  }
  if (level->empty()) {
    const auto side_error =
        reduction.side == domain::Side::buy ? bids_.erase_level(*level) : asks_.erase_level(*level);
    if (side_error != BookSideError::none) {
      std::terminate();
    }
  }
  if (storage_.destroy(node) != StorageError::none) {
    std::terminate();
  }
}

OrderNode* InstrumentBook::commit_prepared_no_check(PreparedRest& prepared_rest) noexcept {
  auto* const node = prepared_rest.node_;
  if (node == nullptr || prepared_rest.staging_level_ == nullptr) {
    std::terminate();
  }

  PriceLevel* target = nullptr;
  if (node->side() == domain::Side::buy) {
    auto& detached = std::get<BidBookSide::DetachedLevel>(prepared_rest.prepared_level_);
    target = bids_.find_level(node->price());
    if (target == nullptr) {
      target = detached.level();
    }
  } else if (node->side() == domain::Side::sell) {
    auto& detached = std::get<AskBookSide::DetachedLevel>(prepared_rest.prepared_level_);
    target = asks_.find_level(node->price());
    if (target == nullptr) {
      target = detached.level();
    }
  } else {
    std::terminate();
  }

  if (target == nullptr || prepared_rest.staging_level_->erase(*node) != PriceLevelError::none ||
      target->append(*node) != PriceLevelError::none) {
    std::terminate();
  }

  BookSideError publish_error = BookSideError::none;
  if (node->side() == domain::Side::buy && bids_.find_level(node->price()) == nullptr) {
    publish_error = std::get<BidBookSide::DetachedLevel>(prepared_rest.prepared_level_).publish();
  } else if (node->side() == domain::Side::sell && asks_.find_level(node->price()) == nullptr) {
    publish_error = std::get<AskBookSide::DetachedLevel>(prepared_rest.prepared_level_).publish();
  }
  if (publish_error != BookSideError::none) {
    std::terminate();
  }

  prepared_rest.owner_ = nullptr;
  prepared_rest.node_ = nullptr;
  prepared_rest.staging_level_.reset();
  prepared_rest.prepared_level_ = std::monostate{};
  prepared_rest.replacement_old_ = nullptr;
  pending_node_ = nullptr;
  pending_level_ = nullptr;
  return node;
}

InstrumentBookInvariantResult InstrumentBook::validate_invariants() const noexcept {
  InstrumentBookInvariantResult result{};
  result.storage_size = storage_.size();
  result.index_size = index_.size();
  const auto fail = [&result](InstrumentBookInvariantError error, const OrderNode* node = nullptr,
                              domain::OrderId order_id = {}, domain::PriceTicks price = {},
                              BookSideInvariantResult side_result = {},
                              ActiveOrderIndexInvariantResult index_result = {}) noexcept {
    result.error = error;
    result.node = node;
    result.order_id = order_id;
    result.price = price;
    result.side_result = side_result;
    result.index_result = index_result;
    return result;
  };

  if (instrument_id_.value() == 0) {
    return fail(InstrumentBookInvariantError::invalid_instrument_id);
  }
  const bool has_pending_node = pending_node_ != nullptr;
  const bool has_pending_level = pending_level_ != nullptr;
  if (has_pending_node != has_pending_level) {
    return fail(InstrumentBookInvariantError::pending_state_mismatch, pending_node_);
  }

  const auto bid_result = bids_.validate_invariants();
  if (!bid_result) {
    return fail(InstrumentBookInvariantError::bid_side_invariant, nullptr, {}, bid_result.price,
                bid_result);
  }
  const auto ask_result = asks_.validate_invariants();
  if (!ask_result) {
    return fail(InstrumentBookInvariantError::ask_side_invariant, nullptr, {}, ask_result.price,
                ask_result);
  }
  const auto index_result = index_.validate_invariants();
  if (!index_result) {
    return fail(InstrumentBookInvariantError::index_invariant, index_result.node,
                index_result.order_id, {}, {}, index_result);
  }
  if (result.storage_size != result.index_size) {
    return fail(InstrumentBookInvariantError::storage_index_size_mismatch);
  }
  if (has_pending_node) {
    if (result.index_size == 0U || storage_.find(pending_node_->order_id()) != pending_node_ ||
        index_.find(pending_node_->order_id()) != pending_node_ ||
        pending_node_->instrument_id() != instrument_id_ ||
        pending_node_->client_id().value() == 0U || !domain::is_valid(pending_node_->side()) ||
        pending_node_->price_level() != pending_level_ ||
        bids_.find_level(pending_node_->price()) == pending_level_ ||
        asks_.find_level(pending_node_->price()) == pending_level_) {
      return fail(InstrumentBookInvariantError::pending_state_mismatch, pending_node_,
                  pending_node_->order_id(), pending_node_->price());
    }

    const auto pending_result = pending_level_->validate_invariants();
    if (!pending_result || pending_level_->price() != pending_node_->price() ||
        pending_level_->order_count() != 1U || pending_level_->head() != pending_node_ ||
        pending_level_->tail() != pending_node_) {
      return fail(InstrumentBookInvariantError::pending_node_invariant, pending_node_,
                  pending_node_->order_id(), pending_node_->price());
    }
  }

  const auto traverse_side = [this, &result, &fail](const auto& side,
                                                    domain::Side expected_side) noexcept {
    for (const PriceLevel& level : side) {
      const OrderNode* node = level.head();
      while (node != nullptr) {
        if (result.observed_order_count == std::numeric_limits<std::size_t>::max()) {
          static_cast<void>(fail(InstrumentBookInvariantError::traversal_count_overflow, node,
                                 node->order_id(), level.price()));
          return false;
        }
        ++result.observed_order_count;
        if (node == pending_node_) {
          static_cast<void>(fail(InstrumentBookInvariantError::pending_state_mismatch, node,
                                 node->order_id(), level.price()));
          return false;
        }
        if (node->side() != expected_side) {
          static_cast<void>(fail(InstrumentBookInvariantError::node_side_mismatch, node,
                                 node->order_id(), level.price()));
          return false;
        }
        if (node->instrument_id() != instrument_id_) {
          static_cast<void>(fail(InstrumentBookInvariantError::node_instrument_mismatch, node,
                                 node->order_id(), level.price()));
          return false;
        }
        if (node->client_id().value() == 0U) {
          static_cast<void>(fail(InstrumentBookInvariantError::node_invalid_client_id, node,
                                 node->order_id(), level.price()));
          return false;
        }
        if (node->price() != level.price()) {
          static_cast<void>(fail(InstrumentBookInvariantError::node_price_mismatch, node,
                                 node->order_id(), level.price()));
          return false;
        }
        if (node->price_level() != &level) {
          static_cast<void>(fail(InstrumentBookInvariantError::node_backlink_mismatch, node,
                                 node->order_id(), level.price()));
          return false;
        }
        if (index_.find(node->order_id()) != node) {
          static_cast<void>(fail(InstrumentBookInvariantError::node_missing_from_index, node,
                                 node->order_id(), level.price()));
          return false;
        }
        if (storage_.find(node->order_id()) != node) {
          static_cast<void>(fail(InstrumentBookInvariantError::node_missing_from_storage, node,
                                 node->order_id(), level.price()));
          return false;
        }
        node = node->next();
      }
    }
    return true;
  };

  if (!traverse_side(bids_, domain::Side::buy) || !traverse_side(asks_, domain::Side::sell)) {
    return result;
  }
  const auto expected_active_count = result.index_size - (has_pending_node ? 1U : 0U);
  if (result.observed_order_count != expected_active_count) {
    return fail(InstrumentBookInvariantError::traversal_index_size_mismatch);
  }

  if (const auto* bid = bids_.best_level(); bid != nullptr) {
    if (const auto* ask = asks_.best_level(); ask != nullptr && bid->price() >= ask->price()) {
      return fail(InstrumentBookInvariantError::crossed_book, nullptr, {}, bid->price());
    }
  }

  return result;
}

void InstrumentBook::enforce_postconditions() const noexcept {
#if defined(ATLAS_ENABLE_INVARIANTS) && ATLAS_ENABLE_INVARIANTS
  if (!validate_invariants()) {
    std::terminate();
  }
#endif
}

void InstrumentBook::drain() noexcept {
  if (pending_node_ != nullptr || pending_level_ != nullptr) {
    std::terminate();
  }
  while (!index_.empty()) {
    auto* const node = index_.any_order();
    if (node == nullptr || !remove(*node)) {
      std::terminate();
    }
  }
  if (!index_.empty() || storage_.size() != 0U || !bids_.empty() || !asks_.empty()) {
    std::terminate();
  }
}

}  // namespace atlaslob::core
