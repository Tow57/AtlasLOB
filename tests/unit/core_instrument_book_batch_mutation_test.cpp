#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

#include "instrument_book.hpp"

namespace {

using namespace atlaslob;
using namespace atlaslob::core;

constexpr domain::InstrumentId instrument_id{1U};

OrderNodeSpec order_spec(std::uint64_t order_id, std::uint32_t client_id, domain::Side side,
                         std::int64_t price, std::uint64_t quantity, std::uint64_t priority) {
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

PrevalidatedBookReduction reduction(OrderNode& node, std::uint64_t quantity) {
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

TEST(InstrumentBookBatchMutation, AppliesMixedFullAndPartialReductionsAcrossLevels) {
  InstrumentBook book{instrument_id};
  const auto first = book.rest(order_spec(1U, 11U, domain::Side::sell, 100, 5U, 1U));
  const auto second = book.rest(order_spec(2U, 12U, domain::Side::sell, 100, 7U, 2U));
  const auto third = book.rest(order_spec(3U, 13U, domain::Side::sell, 101, 11U, 3U));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(third);

  const std::vector reductions{
      reduction(*first.node, 5U),
      reduction(*second.node, 3U),
      reduction(*third.node, 11U),
  };
  const auto status = book.apply_prevalidated_batch(reductions);

  ASSERT_TRUE(status);
  EXPECT_EQ(book.find(domain::OrderId{1U}), nullptr);
  EXPECT_EQ(book.find(domain::OrderId{3U}), nullptr);
  ASSERT_EQ(book.find(domain::OrderId{2U}), second.node);
  EXPECT_EQ(second.node->remaining_quantity(), domain::Quantity{4U});
  ASSERT_NE(book.best_ask(), nullptr);
  EXPECT_EQ(book.best_ask()->price(), domain::PriceTicks{100});
  EXPECT_EQ(book.best_ask()->aggregate_quantity(), domain::Quantity{4U});
  EXPECT_EQ(book.best_ask()->order_count(), 1U);
  EXPECT_EQ(book.asks().find_level(domain::PriceTicks{101}), nullptr);
  EXPECT_EQ(book.active_order_count(), 1U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookBatchMutation, CommitsPreparedResidualAfterPassiveFills) {
  InstrumentBook book{instrument_id};
  const auto first = book.rest(order_spec(1U, 11U, domain::Side::sell, 100, 5U, 1U));
  const auto second = book.rest(order_spec(2U, 12U, domain::Side::sell, 101, 7U, 2U));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);

  auto prepared = book.prepare_rest(order_spec(100U, 99U, domain::Side::buy, 100, 3U, 10U));
  ASSERT_TRUE(prepared);
  const std::vector reductions{reduction(*first.node, 5U)};

  const auto status = book.apply_prevalidated_batch(reductions, &prepared);

  ASSERT_TRUE(status);
  EXPECT_FALSE(prepared);
  EXPECT_EQ(book.find(domain::OrderId{1U}), nullptr);
  const auto* const residual = book.find(domain::OrderId{100U});
  ASSERT_NE(residual, nullptr);
  EXPECT_EQ(residual->remaining_quantity(), domain::Quantity{3U});
  EXPECT_EQ(residual->priority_sequence(), domain::Sequence{10U});
  ASSERT_NE(book.best_bid(), nullptr);
  EXPECT_EQ(book.best_bid()->head(), residual);
  EXPECT_EQ(book.best_bid()->aggregate_quantity(), domain::Quantity{3U});
  ASSERT_NE(book.best_ask(), nullptr);
  EXPECT_EQ(book.best_ask()->price(), domain::PriceTicks{101});
  EXPECT_EQ(book.active_order_count(), 2U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookBatchMutation, RejectsMismatchedBindingWithoutMutation) {
  InstrumentBook book{instrument_id};
  InstrumentBook foreign_book{instrument_id};
  const auto first = book.rest(order_spec(1U, 11U, domain::Side::buy, 100, 5U, 1U));
  const auto second = book.rest(order_spec(2U, 12U, domain::Side::buy, 100, 7U, 2U));
  const auto foreign = foreign_book.rest(order_spec(1U, 11U, domain::Side::buy, 100, 5U, 1U));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(foreign);
  auto wrong = reduction(*first.node, 5U);
  wrong.order_id = second.node->order_id();
  const std::vector reductions{wrong};

  const auto status = book.apply_prevalidated_batch(reductions);

  EXPECT_FALSE(status);
  EXPECT_EQ(status.error, PrevalidatedBatchError::invalid_binding);
  EXPECT_EQ(status.failing_reduction, 0U);
  EXPECT_EQ(book.find(domain::OrderId{1U}), first.node);
  EXPECT_EQ(book.find(domain::OrderId{2U}), second.node);
  EXPECT_EQ(first.node->remaining_quantity(), domain::Quantity{5U});
  EXPECT_EQ(second.node->remaining_quantity(), domain::Quantity{7U});
  EXPECT_EQ(book.best_bid()->aggregate_quantity(), domain::Quantity{12U});
  EXPECT_EQ(book.active_order_count(), 2U);

  auto wrong_pointer = reduction(*foreign.node, 5U);
  const std::vector foreign_reductions{wrong_pointer};
  const auto foreign_status = book.apply_prevalidated_batch(foreign_reductions);
  EXPECT_EQ(foreign_status.error, PrevalidatedBatchError::invalid_binding);
  EXPECT_EQ(book.find(domain::OrderId{1U}), first.node);
  EXPECT_EQ(foreign_book.find(domain::OrderId{1U}), foreign.node);
  EXPECT_TRUE(book.validate_invariants());
  EXPECT_TRUE(foreign_book.validate_invariants());
}

TEST(InstrumentBookBatchMutation, RejectsDuplicateAndInvalidQuantitiesAtomically) {
  InstrumentBook book{instrument_id};
  const auto rested = book.rest(order_spec(1U, 11U, domain::Side::sell, 100, 5U, 1U));
  ASSERT_TRUE(rested);
  const auto valid = reduction(*rested.node, 2U);
  const std::vector duplicate{valid, valid};

  const auto duplicate_status = book.apply_prevalidated_batch(duplicate);
  EXPECT_EQ(duplicate_status.error, PrevalidatedBatchError::duplicate_binding);
  EXPECT_EQ(duplicate_status.failing_reduction, 1U);

  auto invalid = valid;
  invalid.remaining_after = domain::Quantity{4U};
  const std::vector invalid_reductions{invalid};
  const auto invalid_status = book.apply_prevalidated_batch(invalid_reductions);
  EXPECT_EQ(invalid_status.error, PrevalidatedBatchError::invalid_reduction);
  EXPECT_EQ(rested.node->remaining_quantity(), domain::Quantity{5U});
  EXPECT_EQ(book.best_ask()->aggregate_quantity(), domain::Quantity{5U});
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookBatchMutation, RejectsResidualThatStillCrossesWithoutMutation) {
  InstrumentBook book{instrument_id};
  const auto first = book.rest(order_spec(1U, 11U, domain::Side::sell, 100, 5U, 1U));
  const auto second = book.rest(order_spec(2U, 12U, domain::Side::sell, 101, 7U, 2U));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  auto prepared = book.prepare_rest(order_spec(100U, 99U, domain::Side::buy, 101, 3U, 10U));
  ASSERT_TRUE(prepared);
  const std::vector reductions{reduction(*first.node, 5U)};

  const auto status = book.apply_prevalidated_batch(reductions, &prepared);

  EXPECT_FALSE(status);
  EXPECT_EQ(status.error, PrevalidatedBatchError::residual_would_cross);
  EXPECT_TRUE(prepared);
  EXPECT_EQ(book.find(domain::OrderId{1U}), first.node);
  EXPECT_EQ(book.find(domain::OrderId{2U}), second.node);
  EXPECT_EQ(first.node->remaining_quantity(), domain::Quantity{5U});
  EXPECT_EQ(second.node->remaining_quantity(), domain::Quantity{7U});
  EXPECT_EQ(book.active_order_count(), 2U);
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookBatchMutation, RejectsPreparedRestOwnedByAnotherBook) {
  InstrumentBook book{instrument_id};
  InstrumentBook other{instrument_id};
  const auto rested = book.rest(order_spec(1U, 11U, domain::Side::sell, 100, 5U, 1U));
  ASSERT_TRUE(rested);
  auto foreign = other.prepare_rest(order_spec(100U, 99U, domain::Side::buy, 99, 3U, 10U));
  ASSERT_TRUE(foreign);
  const std::vector reductions{reduction(*rested.node, 5U)};

  const auto status = book.apply_prevalidated_batch(reductions, &foreign);

  EXPECT_EQ(status.error, PrevalidatedBatchError::preparation_mismatch);
  EXPECT_TRUE(foreign);
  EXPECT_EQ(book.find(domain::OrderId{1U}), rested.node);
  EXPECT_EQ(rested.node->remaining_quantity(), domain::Quantity{5U});
  EXPECT_TRUE(book.validate_invariants());
  EXPECT_TRUE(other.validate_invariants());
}

TEST(InstrumentBookBatchMutation, AllowsPreparedOnlyPublicationAndEmptyNoOp) {
  InstrumentBook book{instrument_id};
  auto prepared = book.prepare_rest(order_spec(100U, 99U, domain::Side::buy, 99, 3U, 10U));
  ASSERT_TRUE(prepared);

  EXPECT_TRUE(
      book.apply_prevalidated_batch(std::span<const PrevalidatedBookReduction>{}, &prepared));
  EXPECT_FALSE(prepared);
  EXPECT_NE(book.find(domain::OrderId{100U}), nullptr);
  EXPECT_TRUE(book.validate_invariants());

  EXPECT_TRUE(book.apply_prevalidated_batch(std::span<const PrevalidatedBookReduction>{}));
  EXPECT_TRUE(book.validate_invariants());
}

TEST(InstrumentBookBatchMutationVocabulary, HasStableStrings) {
  EXPECT_EQ(to_string(PrevalidatedBatchError::duplicate_binding), "duplicate_binding");
  EXPECT_EQ(to_string(PrevalidatedBatchError::residual_append_failure), "residual_append_failure");
  EXPECT_EQ(to_string(static_cast<PrevalidatedBatchError>(255U)), "unknown");
}

}  // namespace
