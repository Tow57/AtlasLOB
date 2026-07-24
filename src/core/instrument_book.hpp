#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "active_order_index.hpp"
#include "book_side.hpp"
#include "order_storage.hpp"

namespace atlaslob::core {

enum class InstrumentBookError : std::uint8_t {
  none = 0,
  invalid_instrument_id = 1,
  instrument_mismatch = 2,
  invalid_side = 3,
  duplicate_order_id = 4,
  unknown_order_id = 5,
  node_not_owned = 6,
  node_not_indexed = 7,
  node_not_linked = 8,
  side_mismatch = 9,
  price_level_mismatch = 10,
  storage_failure = 11,
  index_failure = 12,
  book_side_failure = 13,
  price_level_failure = 14,
  book_invariant_violation = 15,
  would_cross_book = 16,
};

[[nodiscard]] constexpr std::string_view to_string(InstrumentBookError error) noexcept {
  switch (error) {
    case InstrumentBookError::none:
      return "none";
    case InstrumentBookError::invalid_instrument_id:
      return "invalid_instrument_id";
    case InstrumentBookError::instrument_mismatch:
      return "instrument_mismatch";
    case InstrumentBookError::invalid_side:
      return "invalid_side";
    case InstrumentBookError::duplicate_order_id:
      return "duplicate_order_id";
    case InstrumentBookError::unknown_order_id:
      return "unknown_order_id";
    case InstrumentBookError::node_not_owned:
      return "node_not_owned";
    case InstrumentBookError::node_not_indexed:
      return "node_not_indexed";
    case InstrumentBookError::node_not_linked:
      return "node_not_linked";
    case InstrumentBookError::side_mismatch:
      return "side_mismatch";
    case InstrumentBookError::price_level_mismatch:
      return "price_level_mismatch";
    case InstrumentBookError::storage_failure:
      return "storage_failure";
    case InstrumentBookError::index_failure:
      return "index_failure";
    case InstrumentBookError::book_side_failure:
      return "book_side_failure";
    case InstrumentBookError::price_level_failure:
      return "price_level_failure";
    case InstrumentBookError::book_invariant_violation:
      return "book_invariant_violation";
    case InstrumentBookError::would_cross_book:
      return "would_cross_book";
  }
  return "unknown";
}

struct InstrumentBookStatus final {
  InstrumentBookError error{InstrumentBookError::none};
  StorageError storage_error{StorageError::none};
  ActiveOrderIndexError index_error{ActiveOrderIndexError::none};
  BookSideError side_error{BookSideError::none};
  PriceLevelError level_error{PriceLevelError::none};

  [[nodiscard]] constexpr bool valid() const noexcept { return error == InstrumentBookError::none; }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

  bool operator==(const InstrumentBookStatus&) const = default;
};

struct RestOrderResult final {
  OrderNode* node{};
  InstrumentBookStatus status{};

  [[nodiscard]] constexpr bool has_value() const noexcept {
    return node != nullptr && status.valid();
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }
};

struct RemovedOrder final {
  domain::OrderId order_id{};
  domain::ClientId client_id{};
  domain::InstrumentId instrument_id{};
  domain::Side side{};
  domain::PriceTicks price{};
  domain::Quantity remaining_quantity{};
  domain::Sequence priority_sequence{};

  bool operator==(const RemovedOrder&) const = default;
};

struct RemoveOrderResult final {
  RemovedOrder order{};
  InstrumentBookStatus status{};

  [[nodiscard]] constexpr bool has_value() const noexcept {
    return order.order_id.value() != 0 && status.valid();
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }
};

enum class InstrumentBookInvariantError : std::uint8_t {
  none = 0,
  invalid_instrument_id = 1,
  bid_side_invariant = 2,
  ask_side_invariant = 3,
  index_invariant = 4,
  storage_index_size_mismatch = 5,
  traversal_count_overflow = 6,
  node_side_mismatch = 7,
  node_instrument_mismatch = 8,
  node_price_mismatch = 9,
  node_backlink_mismatch = 10,
  node_missing_from_index = 11,
  node_missing_from_storage = 12,
  traversal_index_size_mismatch = 13,
  node_invalid_client_id = 14,
  crossed_book = 15,
};

[[nodiscard]] constexpr std::string_view to_string(InstrumentBookInvariantError error) noexcept {
  switch (error) {
    case InstrumentBookInvariantError::none:
      return "none";
    case InstrumentBookInvariantError::invalid_instrument_id:
      return "invalid_instrument_id";
    case InstrumentBookInvariantError::bid_side_invariant:
      return "bid_side_invariant";
    case InstrumentBookInvariantError::ask_side_invariant:
      return "ask_side_invariant";
    case InstrumentBookInvariantError::index_invariant:
      return "index_invariant";
    case InstrumentBookInvariantError::storage_index_size_mismatch:
      return "storage_index_size_mismatch";
    case InstrumentBookInvariantError::traversal_count_overflow:
      return "traversal_count_overflow";
    case InstrumentBookInvariantError::node_side_mismatch:
      return "node_side_mismatch";
    case InstrumentBookInvariantError::node_instrument_mismatch:
      return "node_instrument_mismatch";
    case InstrumentBookInvariantError::node_price_mismatch:
      return "node_price_mismatch";
    case InstrumentBookInvariantError::node_backlink_mismatch:
      return "node_backlink_mismatch";
    case InstrumentBookInvariantError::node_missing_from_index:
      return "node_missing_from_index";
    case InstrumentBookInvariantError::node_missing_from_storage:
      return "node_missing_from_storage";
    case InstrumentBookInvariantError::traversal_index_size_mismatch:
      return "traversal_index_size_mismatch";
    case InstrumentBookInvariantError::node_invalid_client_id:
      return "node_invalid_client_id";
    case InstrumentBookInvariantError::crossed_book:
      return "crossed_book";
  }
  return "unknown";
}

struct InstrumentBookInvariantResult final {
  InstrumentBookInvariantError error{InstrumentBookInvariantError::none};
  const OrderNode* node{};
  domain::OrderId order_id{};
  domain::PriceTicks price{};
  std::size_t observed_order_count{};
  std::size_t storage_size{};
  std::size_t index_size{};
  BookSideInvariantResult side_result{};
  ActiveOrderIndexInvariantResult index_result{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return error == InstrumentBookInvariantError::none;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

  bool operator==(const InstrumentBookInvariantResult&) const = default;
};

class InstrumentBook final {
 public:
  explicit InstrumentBook(domain::InstrumentId instrument_id);

  InstrumentBook(const InstrumentBook&) = delete;
  InstrumentBook& operator=(const InstrumentBook&) = delete;
  InstrumentBook(InstrumentBook&&) = delete;
  InstrumentBook& operator=(InstrumentBook&&) = delete;
  ~InstrumentBook() noexcept;

  [[nodiscard]] domain::InstrumentId instrument_id() const noexcept { return instrument_id_; }
  [[nodiscard]] std::size_t active_order_count() const noexcept { return index_.size(); }
  [[nodiscard]] bool empty() const noexcept { return index_.empty(); }

  [[nodiscard]] RestOrderResult rest(const OrderNodeSpec& spec);
  [[nodiscard]] OrderNode* find(domain::OrderId order_id) noexcept;
  [[nodiscard]] const OrderNode* find(domain::OrderId order_id) const noexcept;
  [[nodiscard]] InstrumentBookStatus reduce_remaining(OrderNode& node,
                                                      domain::Quantity reduction) noexcept;
  [[nodiscard]] RemoveOrderResult remove(OrderNode& node) noexcept;
  [[nodiscard]] RemoveOrderResult cancel(domain::OrderId order_id) noexcept;

  [[nodiscard]] PriceLevel* best_bid() noexcept { return bids_.best_level(); }
  [[nodiscard]] const PriceLevel* best_bid() const noexcept { return bids_.best_level(); }
  [[nodiscard]] PriceLevel* best_ask() noexcept { return asks_.best_level(); }
  [[nodiscard]] const PriceLevel* best_ask() const noexcept { return asks_.best_level(); }
  [[nodiscard]] const BidBookSide& bids() const noexcept { return bids_; }
  [[nodiscard]] const AskBookSide& asks() const noexcept { return asks_; }

  [[nodiscard]] InstrumentBookInvariantResult validate_invariants() const noexcept;

 private:
#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
  friend class test::CoreAccess;
#endif

  [[nodiscard]] InstrumentBookStatus preflight_node(const OrderNode& node) const noexcept;
  [[nodiscard]] RemoveOrderResult remove_prevalidated(OrderNode& node) noexcept;
  [[nodiscard]] InstrumentBookStatus rollback_prepared(OrderNode& node, bool indexed) noexcept;
  void enforce_postconditions() const noexcept;
  void drain() noexcept;

  domain::InstrumentId instrument_id_{};
  HeapOrderStorage storage_;
  BidBookSide bids_;
  AskBookSide asks_;
  ActiveOrderIndex index_;
};

}  // namespace atlaslob::core
