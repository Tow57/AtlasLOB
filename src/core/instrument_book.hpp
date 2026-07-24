#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <variant>

#include "active_order_index.hpp"
#include "book_side.hpp"
#include "order_storage.hpp"

namespace atlaslob::core {

enum class PreparationAllocationStage : std::uint8_t {
  detached_level = 1,
  staging_level = 2,
  storage_node = 3,
  active_index = 4,
};

using PreparationAllocationHook = void (*)(PreparationAllocationStage);

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
  preparation_in_progress = 17,
  replacement_transaction_required = 18,
  replacement_mismatch = 19,
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
    case InstrumentBookError::preparation_in_progress:
      return "preparation_in_progress";
    case InstrumentBookError::replacement_transaction_required:
      return "replacement_transaction_required";
    case InstrumentBookError::replacement_mismatch:
      return "replacement_mismatch";
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

// A value-plus-pointer binding created by the execution layer after match
// planning.  Every value is revalidated before InstrumentBook enters its
// no-fail mutation boundary.
struct PrevalidatedBookReduction final {
  OrderNode* node{};
  domain::OrderId order_id{};
  domain::ClientId client_id{};
  domain::Side side{};
  domain::PriceTicks price{};
  domain::Quantity remaining_before{};
  domain::Quantity reduction{};
  domain::Quantity remaining_after{};
  domain::Sequence priority_sequence{};

  bool operator==(const PrevalidatedBookReduction&) const = default;
};

enum class PrevalidatedBatchError : std::uint8_t {
  none = 0,
  book_invariant_violation = 1,
  invalid_binding = 2,
  duplicate_binding = 3,
  invalid_reduction = 4,
  preparation_mismatch = 5,
  residual_would_cross = 6,
  residual_append_failure = 7,
};

[[nodiscard]] constexpr std::string_view to_string(PrevalidatedBatchError error) noexcept {
  switch (error) {
    case PrevalidatedBatchError::none:
      return "none";
    case PrevalidatedBatchError::book_invariant_violation:
      return "book_invariant_violation";
    case PrevalidatedBatchError::invalid_binding:
      return "invalid_binding";
    case PrevalidatedBatchError::duplicate_binding:
      return "duplicate_binding";
    case PrevalidatedBatchError::invalid_reduction:
      return "invalid_reduction";
    case PrevalidatedBatchError::preparation_mismatch:
      return "preparation_mismatch";
    case PrevalidatedBatchError::residual_would_cross:
      return "residual_would_cross";
    case PrevalidatedBatchError::residual_append_failure:
      return "residual_append_failure";
  }
  return "unknown";
}

struct PrevalidatedBatchStatus final {
  PrevalidatedBatchError error{PrevalidatedBatchError::none};
  std::size_t failing_reduction{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return error == PrevalidatedBatchError::none;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

  bool operator==(const PrevalidatedBatchStatus&) const = default;
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
  pending_state_mismatch = 16,
  pending_node_invariant = 17,
  pending_replacement_invariant = 18,
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
    case InstrumentBookInvariantError::pending_state_mismatch:
      return "pending_state_mismatch";
    case InstrumentBookInvariantError::pending_node_invariant:
      return "pending_node_invariant";
    case InstrumentBookInvariantError::pending_replacement_invariant:
      return "pending_replacement_invariant";
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
  class PreparedRest final {
   public:
    PreparedRest(const PreparedRest&) = delete;
    PreparedRest& operator=(const PreparedRest&) = delete;
    PreparedRest(PreparedRest&& other) noexcept;
    PreparedRest& operator=(PreparedRest&& other) noexcept;
    ~PreparedRest() noexcept;

    [[nodiscard]] const OrderNode* node() const noexcept { return node_; }
    [[nodiscard]] const InstrumentBookStatus& status() const noexcept { return status_; }

    [[nodiscard]] bool has_value() const noexcept {
      return owner_ != nullptr && node_ != nullptr && staging_level_ != nullptr && status_.valid();
    }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    // Publishes the prepared residual without allocating. A changed book that
    // violates the planned residual's append or uncrossed-book preconditions is
    // an internal execution-contract failure and terminates.
    [[nodiscard]] RestOrderResult commit() noexcept;

   private:
    friend class InstrumentBook;

    using PreparedLevel =
        std::variant<std::monostate, BidBookSide::DetachedLevel, AskBookSide::DetachedLevel>;

    explicit PreparedRest(InstrumentBookStatus status) noexcept : status_{status} {}
    PreparedRest(InstrumentBook& owner, OrderNode& node, std::unique_ptr<PriceLevel> staging_level,
                 PreparedLevel prepared_level, domain::OrderId replacement_old_id) noexcept;

    void rollback() noexcept;

    InstrumentBook* owner_{};
    OrderNode* node_{};
    std::unique_ptr<PriceLevel> staging_level_;
    PreparedLevel prepared_level_;
    // Nonzero only for prepare_replace_rest(). The book pins this active ID
    // against direct mutation until the guard commits or rolls back. Keeping
    // identity rather than an owning object's address makes failed preflight
    // safe even if an internal contract violation removed the binding.
    domain::OrderId replacement_old_id_{};
    InstrumentBookStatus status_{};
  };

  explicit InstrumentBook(domain::InstrumentId instrument_id);

  InstrumentBook(const InstrumentBook&) = delete;
  InstrumentBook& operator=(const InstrumentBook&) = delete;
  InstrumentBook(InstrumentBook&&) = delete;
  InstrumentBook& operator=(InstrumentBook&&) = delete;
  ~InstrumentBook() noexcept;

  [[nodiscard]] domain::InstrumentId instrument_id() const noexcept { return instrument_id_; }
  [[nodiscard]] std::size_t active_order_count() const noexcept {
    return index_.size() -
           (pending_node_ != nullptr && !index_.empty() ? std::size_t{1U} : std::size_t{0U});
  }
  [[nodiscard]] bool empty() const noexcept { return active_order_count() == 0U; }
  [[nodiscard]] bool has_pending_preparation() const noexcept {
    return pending_node_ != nullptr || pending_level_ != nullptr ||
           pending_replacement_old_id_.value() != 0U;
  }

  [[nodiscard]] PreparedRest prepare_rest(const OrderNodeSpec& spec);
  [[nodiscard]] PreparedRest prepare_replace_rest(const OrderNodeSpec& spec, OrderNode& old_order);
  [[nodiscard]] RestOrderResult rest(const OrderNodeSpec& spec);
  [[nodiscard]] OrderNode* find(domain::OrderId order_id) noexcept;
  [[nodiscard]] const OrderNode* find(domain::OrderId order_id) const noexcept;
  [[nodiscard]] InstrumentBookStatus reduce_remaining(OrderNode& node,
                                                      domain::Quantity reduction) noexcept;
  [[nodiscard]] RemoveOrderResult remove(OrderNode& node) noexcept;
  [[nodiscard]] RemoveOrderResult cancel(domain::OrderId order_id) noexcept;

  // Applies an already-planned set of passive reductions. All pointer/value
  // bindings and an optional prepared residual are checked before mutation.
  // Once mutation begins, component failures are impossible by contract and
  // therefore terminate. No allocation occurs inside the mutation boundary.
  [[nodiscard]] PrevalidatedBatchStatus apply_prevalidated_batch(
      std::span<const PrevalidatedBookReduction> reductions,
      PreparedRest* prepared_rest = nullptr) noexcept;

  // Atomically removes the exact old order, applies the replacement's passive
  // fills, and publishes its optional prepared residual. The old binding is a
  // mandatory full reduction. All checks happen before the allocation-free,
  // no-fail mutation boundary.
  [[nodiscard]] PrevalidatedBatchStatus apply_prevalidated_replace_batch(
      const PrevalidatedBookReduction& old_reduction,
      std::span<const PrevalidatedBookReduction> passive_reductions,
      PreparedRest* prepared_rest = nullptr) noexcept;

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
  PreparationAllocationHook preparation_allocation_hook_{};
#endif

  [[nodiscard]] InstrumentBookStatus preflight_node(const OrderNode& node) const noexcept;
  [[nodiscard]] PreparedRest prepare_rest_impl(const OrderNodeSpec& spec,
                                               OrderNode* replacement_old);
  [[nodiscard]] RemoveOrderResult remove_prevalidated(OrderNode& node) noexcept;
  void apply_reduction_no_check(const PrevalidatedBookReduction& reduction) noexcept;
  [[nodiscard]] OrderNode* commit_prepared_no_check(PreparedRest& prepared_rest) noexcept;
  void enforce_postconditions() const noexcept;
  void drain() noexcept;

  domain::InstrumentId instrument_id_{};
  HeapOrderStorage storage_;
  BidBookSide bids_;
  AskBookSide asks_;
  ActiveOrderIndex index_;
  OrderNode* pending_node_{};
  PriceLevel* pending_level_{};
  // Nonzero only while a prepared replacement residual is outstanding.
  // Public direct mutation paths reject this exact active ID; unrelated
  // passive orders remain mutable.
  domain::OrderId pending_replacement_old_id_{};
};

}  // namespace atlaslob::core
