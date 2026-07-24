#include "instrument_book.hpp"

#include <exception>
#include <limits>
#include <stdexcept>

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

}  // namespace

InstrumentBook::InstrumentBook(domain::InstrumentId instrument_id) : instrument_id_{instrument_id} {
  if (instrument_id_.value() == 0) {
    throw std::invalid_argument{"InstrumentBook requires a nonzero instrument ID"};
  }
}

InstrumentBook::~InstrumentBook() noexcept { drain(); }

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
  if (storage_.find(spec.order_id) != nullptr || index_.find(spec.order_id) != nullptr) {
    return {
        .node = nullptr,
        .status =
            make_status(InstrumentBookError::duplicate_order_id, StorageError::duplicate_order_id,
                        ActiveOrderIndexError::duplicate_order_id),
    };
  }
  if (spec.side == domain::Side::buy) {
    const auto* const best_ask_level = asks_.best_level();
    if (best_ask_level != nullptr && spec.price >= best_ask_level->price()) {
      return {
          .node = nullptr,
          .status = make_status(InstrumentBookError::would_cross_book),
      };
    }
  } else {
    const auto* const best_bid_level = bids_.best_level();
    if (best_bid_level != nullptr && spec.price <= best_bid_level->price()) {
      return {
          .node = nullptr,
          .status = make_status(InstrumentBookError::would_cross_book),
      };
    }
  }

  const auto created = storage_.create(spec);
  if (!created) {
    const auto error = created.error == StorageError::duplicate_order_id
                           ? InstrumentBookError::duplicate_order_id
                           : InstrumentBookError::storage_failure;
    return {
        .node = nullptr,
        .status = make_status(error, created.error),
    };
  }

  auto* const node = created.node;
  bool indexed = false;
  try {
    const auto complete_rest = [this, node, &indexed](auto& side) -> RestOrderResult {
      auto prepared = side.prepare_level(node->price());

      if (!prepared) {
        const auto rollback = rollback_prepared(*node, indexed);
        if (!rollback) {
          std::terminate();
        }
        return {
            .node = nullptr,
            .status = make_status(InstrumentBookError::book_side_failure, StorageError::none,
                                  ActiveOrderIndexError::none, prepared.error()),
        };
      }

      const auto index_error = index_.insert(*node);
      if (index_error != ActiveOrderIndexError::none) {
        const auto rollback = rollback_prepared(*node, indexed);
        if (!rollback) {
          std::terminate();
        }
        return {
            .node = nullptr,
            .status =
                make_status(InstrumentBookError::index_failure, StorageError::none, index_error),
        };
      }
      indexed = true;

      auto* const level = prepared.level();
      const auto append_error = level->append(*node);
      if (append_error != PriceLevelError::none) {
        const auto rollback = rollback_prepared(*node, indexed);
        if (!rollback) {
          std::terminate();
        }
        return {
            .node = nullptr,
            .status = make_status(InstrumentBookError::price_level_failure, StorageError::none,
                                  ActiveOrderIndexError::none, BookSideError::none, append_error),
        };
      }

      const auto commit_error = prepared.commit();
      if (commit_error != BookSideError::none) {
        const auto erase_error = level->erase(*node);
        if (erase_error != PriceLevelError::none) {
          std::terminate();
        }
        const auto rollback = rollback_prepared(*node, indexed);
        if (!rollback) {
          std::terminate();
        }
        return {
            .node = nullptr,
            .status = make_status(InstrumentBookError::book_side_failure, StorageError::none,
                                  ActiveOrderIndexError::none, commit_error),
        };
      }

      return {.node = node, .status = {}};
    };

    auto result = spec.side == domain::Side::buy ? complete_rest(bids_) : complete_rest(asks_);
    if (result) {
      enforce_postconditions();
    }
    return result;
  } catch (...) {
    const auto rollback = rollback_prepared(*node, indexed);
    if (!rollback) {
      std::terminate();
    }
    throw;
  }
}

OrderNode* InstrumentBook::find(domain::OrderId order_id) noexcept { return index_.find(order_id); }

const OrderNode* InstrumentBook::find(domain::OrderId order_id) const noexcept {
  return index_.find(order_id);
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
  auto* const node = index_.find(order_id);
  if (node == nullptr) {
    return {
        .order = {},
        .status = make_status(InstrumentBookError::unknown_order_id),
    };
  }
  return remove_prevalidated(*node);
}

InstrumentBookStatus InstrumentBook::rollback_prepared(OrderNode& node, bool indexed) noexcept {
  if (node.is_linked()) {
    return make_status(InstrumentBookError::book_invariant_violation);
  }
  if (indexed) {
    const auto index_error = index_.erase(node);
    if (index_error != ActiveOrderIndexError::none) {
      return make_status(InstrumentBookError::index_failure, StorageError::none, index_error);
    }
  }
  const auto storage_error = storage_.destroy(node);
  if (storage_error != StorageError::none) {
    return make_status(InstrumentBookError::storage_failure, storage_error);
  }
  return {};
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
  if (result.observed_order_count != result.index_size) {
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
