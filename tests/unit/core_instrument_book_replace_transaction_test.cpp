#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "core_test_access.hpp"
#include "instrument_book.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::core;

constexpr domain::InstrumentId instrument_id{41U};

[[nodiscard]] OrderNodeSpec order_spec(std::uint64_t order_id, std::uint32_t client_id,
                                       domain::Side side, std::int64_t price,
                                       std::uint64_t quantity, std::uint64_t priority) {
  return {
      .order_id = domain::OrderId{order_id},
      .client_id = domain::ClientId{client_id},
      .instrument_id = instrument_id,
      .side = side,
      .price = domain::PriceTicks{price},
      .remaining_quantity = domain::Quantity{quantity},
      .priority_sequence = domain::Sequence{priority},
  };
}

[[nodiscard]] PrevalidatedBookReduction reduction(OrderNode& node, std::uint64_t quantity) {
  return {
      .node = &node,
      .order_id = node.order_id(),
      .client_id = node.client_id(),
      .side = node.side(),
      .price = node.price(),
      .remaining_before = node.remaining_quantity(),
      .reduction = domain::Quantity{quantity},
      .remaining_after = domain::Quantity{node.remaining_quantity().value() - quantity},
      .priority_sequence = node.priority_sequence(),
  };
}

TEST(InstrumentBookReplaceTransaction, RecreatesSingletonLevelAtTheSamePrice) {
  InstrumentBook book{instrument_id};
  const auto old = book.rest(order_spec(1U, 7U, domain::Side::buy, 100, 5U, 1U));
  ASSERT_TRUE(old);

  auto prepared =
      book.prepare_replace_rest(order_spec(2U, 7U, domain::Side::buy, 100, 7U, 10U), *old.node);
  ASSERT_TRUE(prepared);
  const auto old_reduction = reduction(*old.node, 5U);

  const auto status = book.apply_prevalidated_replace_batch(
      old_reduction, std::span<const PrevalidatedBookReduction>{}, &prepared);

  ASSERT_TRUE(status);
  EXPECT_FALSE(prepared);
  EXPECT_EQ(book.find(domain::OrderId{1U}), nullptr);
  const auto* const replacement = book.find(domain::OrderId{2U});
  ASSERT_NE(replacement, nullptr);
  ASSERT_NE(book.best_bid(), nullptr);
  EXPECT_EQ(book.best_bid()->head(), replacement);
  EXPECT_EQ(book.best_bid()->tail(), replacement);
  EXPECT_EQ(book.best_bid()->order_count(), 1U);
  EXPECT_EQ(book.best_bid()->aggregate_quantity(), domain::Quantity{7U});
  EXPECT_EQ(replacement->priority_sequence(), domain::Sequence{10U});
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookReplaceTransaction, ReplacedTailReceivesNewPriorityAtTheSameLevel) {
  InstrumentBook book{instrument_id};
  const auto first = book.rest(order_spec(1U, 7U, domain::Side::sell, 100, 3U, 1U));
  const auto old = book.rest(order_spec(2U, 7U, domain::Side::sell, 100, 5U, 2U));
  ASSERT_TRUE(first);
  ASSERT_TRUE(old);
  auto* const level = book.best_ask();

  auto prepared =
      book.prepare_replace_rest(order_spec(3U, 7U, domain::Side::sell, 100, 7U, 10U), *old.node);
  ASSERT_TRUE(prepared);
  const auto status = book.apply_prevalidated_replace_batch(
      reduction(*old.node, 5U), std::span<const PrevalidatedBookReduction>{}, &prepared);

  ASSERT_TRUE(status);
  const auto* const replacement = book.find(domain::OrderId{3U});
  ASSERT_NE(replacement, nullptr);
  EXPECT_EQ(book.best_ask(), level);
  EXPECT_EQ(level->head(), first.node);
  EXPECT_EQ(level->tail(), replacement);
  EXPECT_EQ(first.node->next(), replacement);
  EXPECT_EQ(replacement->previous(), first.node);
  EXPECT_EQ(replacement->priority_sequence(), domain::Sequence{10U});
  EXPECT_EQ(level->order_count(), 2U);
  EXPECT_EQ(level->aggregate_quantity(), domain::Quantity{10U});
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookReplaceTransaction, OldQuantityRelievesSameLevelAggregateOverflow) {
  InstrumentBook book{instrument_id};
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto first = book.rest(order_spec(1U, 7U, domain::Side::buy, 100, 1U, 1U));
  const auto old = book.rest(order_spec(2U, 7U, domain::Side::buy, 100, maximum - 1U, 2U));
  ASSERT_TRUE(first);
  ASSERT_TRUE(old);
  ASSERT_EQ(book.best_bid()->aggregate_quantity(), domain::Quantity{maximum});

  const auto ordinary =
      book.prepare_rest(order_spec(3U, 7U, domain::Side::buy, 100, maximum - 1U, 10U));
  EXPECT_FALSE(ordinary);
  EXPECT_EQ(ordinary.status().level_error, PriceLevelError::aggregate_overflow);

  auto prepared = book.prepare_replace_rest(
      order_spec(3U, 7U, domain::Side::buy, 100, maximum - 1U, 10U), *old.node);
  ASSERT_TRUE(prepared);
  const auto status = book.apply_prevalidated_replace_batch(
      reduction(*old.node, maximum - 1U), std::span<const PrevalidatedBookReduction>{}, &prepared);

  ASSERT_TRUE(status);
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  ASSERT_NE(book.find(domain::OrderId{3U}), nullptr);
  EXPECT_EQ(book.best_bid()->head(), first.node);
  EXPECT_EQ(book.best_bid()->tail(), book.find(domain::OrderId{3U}));
  EXPECT_EQ(book.best_bid()->order_count(), 2U);
  EXPECT_EQ(book.best_bid()->aggregate_quantity(), domain::Quantity{maximum});
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookReplaceTransaction, SupportsFullyExecutedReplacementWithoutResidual) {
  InstrumentBook book{instrument_id};
  const auto old = book.rest(order_spec(1U, 7U, domain::Side::buy, 90, 4U, 1U));
  const auto passive = book.rest(order_spec(2U, 8U, domain::Side::sell, 100, 5U, 2U));
  ASSERT_TRUE(old);
  ASSERT_TRUE(passive);
  const std::vector passive_reductions{reduction(*passive.node, 5U)};

  const auto status =
      book.apply_prevalidated_replace_batch(reduction(*old.node, 4U), passive_reductions);

  ASSERT_TRUE(status);
  EXPECT_EQ(book.find(domain::OrderId{1U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_TRUE(book.empty());
  EXPECT_EQ(book.best_bid(), nullptr);
  EXPECT_EQ(book.best_ask(), nullptr);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookReplaceTransaction, RejectsWrongOldAndBindingBeforeMutation) {
  InstrumentBook book{instrument_id};
  const auto expected_old = book.rest(order_spec(1U, 7U, domain::Side::buy, 90, 4U, 1U));
  const auto other = book.rest(order_spec(2U, 7U, domain::Side::buy, 89, 6U, 2U));
  ASSERT_TRUE(expected_old);
  ASSERT_TRUE(other);
  auto prepared = book.prepare_replace_rest(order_spec(3U, 7U, domain::Side::buy, 91, 5U, 10U),
                                            *expected_old.node);
  ASSERT_TRUE(prepared);

  const auto wrong_old = book.apply_prevalidated_replace_batch(
      reduction(*other.node, 6U), std::span<const PrevalidatedBookReduction>{}, &prepared);
  EXPECT_EQ(wrong_old.error, PrevalidatedBatchError::preparation_mismatch);
  EXPECT_TRUE(prepared);
  EXPECT_EQ(book.find(domain::OrderId{1U}), expected_old.node);
  EXPECT_EQ(book.find(domain::OrderId{2U}), other.node);

  auto bad_binding = reduction(*expected_old.node, 4U);
  bad_binding.client_id = domain::ClientId{999U};
  const auto bad_binding_status = book.apply_prevalidated_replace_batch(
      bad_binding, std::span<const PrevalidatedBookReduction>{}, &prepared);
  EXPECT_EQ(bad_binding_status.error, PrevalidatedBatchError::invalid_binding);
  EXPECT_TRUE(prepared);
  EXPECT_EQ(expected_old.node->remaining_quantity(), domain::Quantity{4U});
  EXPECT_EQ(other.node->remaining_quantity(), domain::Quantity{6U});
  EXPECT_EQ(book.active_order_count(), 2U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookReplaceTransaction, RejectsForeignOldAndOrdinaryGuardAtomically) {
  InstrumentBook book{instrument_id};
  InstrumentBook foreign{instrument_id};
  const auto old = book.rest(order_spec(1U, 7U, domain::Side::sell, 100, 4U, 1U));
  const auto foreign_old = foreign.rest(order_spec(1U, 7U, domain::Side::sell, 100, 4U, 1U));
  ASSERT_TRUE(old);
  ASSERT_TRUE(foreign_old);

  const auto mismatched = book.prepare_replace_rest(
      order_spec(2U, 7U, domain::Side::sell, 101, 5U, 10U), *foreign_old.node);
  EXPECT_FALSE(mismatched);
  EXPECT_EQ(mismatched.status().error, InstrumentBookError::node_not_owned);

  const auto changed_identity =
      book.prepare_replace_rest(order_spec(2U, 99U, domain::Side::sell, 101, 5U, 10U), *old.node);
  EXPECT_FALSE(changed_identity);
  EXPECT_EQ(changed_identity.status().error, InstrumentBookError::replacement_mismatch);

  auto ordinary = book.prepare_rest(order_spec(2U, 7U, domain::Side::sell, 101, 5U, 10U));
  ASSERT_TRUE(ordinary);
  const auto wrong_guard = book.apply_prevalidated_replace_batch(
      reduction(*old.node, 4U), std::span<const PrevalidatedBookReduction>{}, &ordinary);
  EXPECT_EQ(wrong_guard.error, PrevalidatedBatchError::preparation_mismatch);
  EXPECT_TRUE(ordinary);
  EXPECT_EQ(book.find(domain::OrderId{1U}), old.node);
  EXPECT_EQ(old.node->remaining_quantity(), domain::Quantity{4U});
  EXPECT_TRUE(book.validate_invariants());
  EXPECT_TRUE(foreign.validate_invariants());
}

TEST(InstrumentBookReplaceTransaction, ReplacementGuardCannotUseOrdinaryCommit) {
  InstrumentBook book{instrument_id};
  const auto old = book.rest(order_spec(1U, 7U, domain::Side::buy, 100, 4U, 1U));
  ASSERT_TRUE(old);
  auto prepared =
      book.prepare_replace_rest(order_spec(2U, 7U, domain::Side::buy, 100, 5U, 10U), *old.node);
  ASSERT_TRUE(prepared);

  const auto direct = prepared.commit();

  EXPECT_FALSE(direct);
  EXPECT_EQ(direct.status.error, InstrumentBookError::replacement_transaction_required);
  EXPECT_TRUE(prepared);
  EXPECT_EQ(book.find(domain::OrderId{1U}), old.node);
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookReplaceTransaction, AbandonmentRollsBackOnlyThePreparedReplacement) {
  InstrumentBook book{instrument_id};
  const auto first = book.rest(order_spec(1U, 7U, domain::Side::sell, 100, 3U, 1U));
  const auto old = book.rest(order_spec(2U, 7U, domain::Side::sell, 100, 5U, 2U));
  ASSERT_TRUE(first);
  ASSERT_TRUE(old);
  auto* const level = book.best_ask();

  {
    auto prepared =
        book.prepare_replace_rest(order_spec(3U, 7U, domain::Side::sell, 100, 7U, 10U), *old.node);
    ASSERT_TRUE(prepared);
    EXPECT_EQ(book.find(domain::OrderId{3U}), nullptr);
    EXPECT_EQ(book.active_order_count(), 2U);
    EXPECT_EQ(level->head(), first.node);
    EXPECT_EQ(level->tail(), old.node);
    EXPECT_EQ(level->aggregate_quantity(), domain::Quantity{8U});
    EXPECT_TRUE(book.validate_invariants());
  }

  EXPECT_EQ(book.find(domain::OrderId{1U}), first.node);
  EXPECT_EQ(book.find(domain::OrderId{2U}), old.node);
  EXPECT_EQ(book.find(domain::OrderId{3U}), nullptr);
  EXPECT_EQ(book.best_ask(), level);
  EXPECT_EQ(level->head(), first.node);
  EXPECT_EQ(level->tail(), old.node);
  EXPECT_EQ(level->order_count(), 2U);
  EXPECT_EQ(level->aggregate_quantity(), domain::Quantity{8U});
  EXPECT_EQ(book.active_order_count(), 2U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookReplaceTransaction,
     PinsExactOldAgainstDirectMutationWhileUnrelatedOrdersRemainMutable) {
  InstrumentBook book{instrument_id};
  const auto old = book.rest(order_spec(1U, 7U, domain::Side::buy, 90, 5U, 1U));
  const auto same_level = book.rest(order_spec(2U, 8U, domain::Side::buy, 90, 7U, 2U));
  const auto passive = book.rest(order_spec(3U, 9U, domain::Side::sell, 100, 9U, 3U));
  ASSERT_TRUE(old);
  ASSERT_TRUE(same_level);
  ASSERT_TRUE(passive);

  auto prepared =
      book.prepare_replace_rest(order_spec(4U, 7U, domain::Side::buy, 90, 6U, 10U), *old.node);
  ASSERT_TRUE(prepared);
  ASSERT_TRUE(book.validate_invariants());

  const auto exact_reduce = book.reduce_remaining(*old.node, domain::Quantity{1U});
  EXPECT_EQ(exact_reduce.error, InstrumentBookError::replacement_transaction_required);
  const auto exact_remove = book.remove(*old.node);
  EXPECT_EQ(exact_remove.status.error, InstrumentBookError::replacement_transaction_required);
  const auto exact_cancel = book.cancel(old.node->order_id());
  EXPECT_EQ(exact_cancel.status.error, InstrumentBookError::replacement_transaction_required);
  EXPECT_EQ(old.node->remaining_quantity(), domain::Quantity{5U});

  EXPECT_TRUE(book.reduce_remaining(*same_level.node, domain::Quantity{2U}));
  EXPECT_TRUE(book.reduce_remaining(*passive.node, domain::Quantity{3U}));
  EXPECT_EQ(same_level.node->remaining_quantity(), domain::Quantity{5U});
  EXPECT_EQ(passive.node->remaining_quantity(), domain::Quantity{6U});
  EXPECT_TRUE(book.validate_invariants());

  const auto status = book.apply_prevalidated_replace_batch(
      reduction(*old.node, 5U), std::span<const PrevalidatedBookReduction>{}, &prepared);
  ASSERT_TRUE(status);
  EXPECT_FALSE(prepared);
  EXPECT_EQ(book.find(domain::OrderId{1U}), nullptr);
  const auto* const replacement = book.find(domain::OrderId{4U});
  ASSERT_NE(replacement, nullptr);
  ASSERT_NE(book.best_bid(), nullptr);
  EXPECT_EQ(book.best_bid()->head(), same_level.node);
  EXPECT_EQ(book.best_bid()->tail(), replacement);
  EXPECT_EQ(book.best_bid()->aggregate_quantity(), domain::Quantity{11U});
  EXPECT_EQ(book.best_ask()->aggregate_quantity(), domain::Quantity{6U});
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookReplaceTransaction, UnrelatedTargetRemovalUsesPreparedFallbackLevel) {
  InstrumentBook book{instrument_id};
  const auto old = book.rest(order_spec(1U, 7U, domain::Side::buy, 90, 5U, 1U));
  const auto prior_target = book.rest(order_spec(2U, 8U, domain::Side::buy, 91, 7U, 2U));
  ASSERT_TRUE(old);
  ASSERT_TRUE(prior_target);

  auto prepared =
      book.prepare_replace_rest(order_spec(3U, 7U, domain::Side::buy, 91, 6U, 10U), *old.node);
  ASSERT_TRUE(prepared);

  const auto unrelated_cancel = book.cancel(domain::OrderId{2U});
  ASSERT_TRUE(unrelated_cancel);
  EXPECT_TRUE(prepared);
  EXPECT_TRUE(book.validate_invariants());

  const auto status = book.apply_prevalidated_replace_batch(
      reduction(*old.node, 5U), std::span<const PrevalidatedBookReduction>{}, &prepared);
  ASSERT_TRUE(status);
  EXPECT_FALSE(prepared);
  const auto* const replacement = book.find(domain::OrderId{3U});
  ASSERT_NE(replacement, nullptr);
  ASSERT_NE(book.best_bid(), nullptr);
  EXPECT_EQ(book.best_bid()->price(), domain::PriceTicks{91});
  EXPECT_EQ(book.best_bid()->head(), replacement);
  EXPECT_EQ(book.best_bid()->tail(), replacement);
  EXPECT_EQ(book.best_bid()->aggregate_quantity(), domain::Quantity{6U});
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookReplaceTransaction, MovingAndAbandoningGuardTransfersAndReleasesOldPin) {
  InstrumentBook book{instrument_id};
  const auto old = book.rest(order_spec(1U, 7U, domain::Side::sell, 100, 5U, 1U));
  ASSERT_TRUE(old);

  {
    auto original =
        book.prepare_replace_rest(order_spec(2U, 7U, domain::Side::sell, 101, 6U, 10U), *old.node);
    ASSERT_TRUE(original);
    auto moved = std::move(original);
    EXPECT_FALSE(original);
    ASSERT_TRUE(moved);
    EXPECT_EQ(book.cancel(old.node->order_id()).status.error,
              InstrumentBookError::replacement_transaction_required);
    EXPECT_TRUE(book.validate_invariants());
  }

  EXPECT_FALSE(book.has_pending_preparation());
  EXPECT_EQ(book.find(domain::OrderId{2U}), nullptr);
  EXPECT_TRUE(book.reduce_remaining(*old.node, domain::Quantity{1U}));
  EXPECT_EQ(old.node->remaining_quantity(), domain::Quantity{4U});
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookReplaceTransaction, MoveAssignmentReleasesPriorPinAndTransfersSourcePin) {
  InstrumentBook first_book{instrument_id};
  InstrumentBook second_book{instrument_id};
  const auto first_old = first_book.rest(order_spec(1U, 7U, domain::Side::buy, 90, 5U, 1U));
  const auto second_old = second_book.rest(order_spec(11U, 8U, domain::Side::sell, 100, 6U, 2U));
  ASSERT_TRUE(first_old);
  ASSERT_TRUE(second_old);

  {
    auto target = first_book.prepare_replace_rest(
        order_spec(2U, 7U, domain::Side::buy, 91, 7U, 10U), *first_old.node);
    auto source = second_book.prepare_replace_rest(
        order_spec(12U, 8U, domain::Side::sell, 101, 8U, 11U), *second_old.node);
    ASSERT_TRUE(target);
    ASSERT_TRUE(source);

    target = std::move(source);

    EXPECT_FALSE(source);
    ASSERT_TRUE(target);
    EXPECT_FALSE(first_book.has_pending_preparation());
    EXPECT_TRUE(second_book.has_pending_preparation());
    EXPECT_TRUE(first_book.cancel(first_old.node->order_id()));
    EXPECT_EQ(second_book.cancel(second_old.node->order_id()).status.error,
              InstrumentBookError::replacement_transaction_required);
    EXPECT_TRUE(first_book.validate_invariants());
    EXPECT_TRUE(second_book.validate_invariants());
  }

  EXPECT_FALSE(second_book.has_pending_preparation());
  EXPECT_TRUE(second_book.reduce_remaining(*second_old.node, domain::Quantity{1U}));
  EXPECT_EQ(second_old.node->remaining_quantity(), domain::Quantity{5U});
  EXPECT_TRUE(second_book.validate_invariants());
}

TEST(InstrumentBookReplaceTransaction, InvariantsRejectUnknownPinnedReplacementIdentity) {
  InstrumentBook book{instrument_id};
  const auto old = book.rest(order_spec(1U, 7U, domain::Side::buy, 90, 5U, 1U));
  ASSERT_TRUE(old);
  auto prepared =
      book.prepare_replace_rest(order_spec(2U, 7U, domain::Side::buy, 91, 6U, 10U), *old.node);
  ASSERT_TRUE(prepared);
  ASSERT_TRUE(book.validate_invariants());

  core::test::CoreAccess::set_pending_replacement_old_id(book, domain::OrderId{999U});
  const auto corrupted = book.validate_invariants();
  EXPECT_EQ(corrupted.error, InstrumentBookInvariantError::pending_replacement_invariant);
  EXPECT_EQ(corrupted.order_id, domain::OrderId{999U});

  core::test::CoreAccess::set_pending_replacement_old_id(book, old.node->order_id());
  EXPECT_TRUE(book.validate_invariants());
}

}  // namespace
