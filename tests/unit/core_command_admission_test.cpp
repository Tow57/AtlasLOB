#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include "command_admission.hpp"
#include "command_sequencer.hpp"
#include "core_test_access.hpp"
#include "state_validation.hpp"

namespace {

namespace core = atlaslob::core;
namespace domain = atlaslob::domain;

constexpr domain::InstrumentId instrument_id{7U};
constexpr domain::ClientId client_id{17U};
constexpr core::ExecutionPolicy policy{
    .max_order_quantity = domain::Quantity{100U},
    .tick_increment = domain::PriceTicks{5},
    .max_active_orders = 2U,
};

static_assert(policy.valid());
static_assert(!std::is_copy_constructible_v<core::CommandSequencer>);
static_assert(!std::is_move_constructible_v<core::CommandSequencer>);
static_assert(!std::is_copy_constructible_v<core::CommandAdmission>);
static_assert(!std::is_move_constructible_v<core::CommandAdmission>);

[[nodiscard]] domain::NewOrder new_order(std::uint64_t order_id = 101U) {
  return {
      .client_id = client_id,
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
      .side = domain::Side::buy,
      .order_type = domain::OrderType::limit,
      .time_in_force = domain::TimeInForce::gtc,
      .limit_price = domain::PriceTicks{10'000},
      .quantity = domain::Quantity{10U},
  };
}

[[nodiscard]] domain::CancelOrder cancel_order(std::uint64_t order_id = 101U) {
  return {
      .client_id = client_id,
      .order_id = domain::OrderId{order_id},
      .instrument_id = instrument_id,
  };
}

[[nodiscard]] domain::ReplaceOrder replace_order(std::uint64_t old_order_id = 101U,
                                                 std::uint64_t new_order_id = 102U) {
  return {
      .client_id = client_id,
      .old_order_id = domain::OrderId{old_order_id},
      .new_order_id = domain::OrderId{new_order_id},
      .instrument_id = instrument_id,
      .new_limit_price = domain::PriceTicks{10'005},
      .new_quantity = domain::Quantity{11U},
  };
}

[[nodiscard]] atlaslob::core::OrderNodeSpec node_spec(std::uint64_t order_id,
                                                      std::uint64_t priority_sequence,
                                                      domain::ClientId owner = client_id,
                                                      domain::Side side = domain::Side::buy,
                                                      std::int64_t price = 10'000) {
  return {
      .order_id = domain::OrderId{order_id},
      .client_id = owner,
      .instrument_id = instrument_id,
      .side = side,
      .price = domain::PriceTicks{price},
      .remaining_quantity = domain::Quantity{10U},
      .priority_sequence = domain::Sequence{priority_sequence},
  };
}

TEST(CommandSequencer, IssuesStrictlyMonotonicNonzeroSequences) {
  core::CommandSequencer sequencer;

  const auto first = sequencer.issue();
  const auto second = sequencer.issue();
  const auto third = sequencer.issue();

  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(third);
  EXPECT_EQ(first.sequence, domain::Sequence{1U});
  EXPECT_EQ(second.sequence, domain::Sequence{2U});
  EXPECT_EQ(third.sequence, domain::Sequence{3U});
  EXPECT_EQ(sequencer.next_sequence(), domain::Sequence{4U});
  EXPECT_FALSE(sequencer.exhausted());
}

TEST(CommandSequencer, IssuesMaximumOnceThenReturnsStickyInternalExhaustion) {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  core::CommandSequencer sequencer{domain::Sequence{maximum}};

  const auto maximum_issue = sequencer.issue();
  const auto first_exhaustion = sequencer.issue();
  const auto second_exhaustion = sequencer.issue();

  ASSERT_TRUE(maximum_issue);
  EXPECT_EQ(maximum_issue.sequence, domain::Sequence{maximum});
  EXPECT_TRUE(sequencer.exhausted());
  EXPECT_EQ(sequencer.next_sequence(), domain::Sequence{});

  EXPECT_FALSE(first_exhaustion);
  EXPECT_EQ(first_exhaustion.sequence, domain::Sequence{});
  EXPECT_EQ(first_exhaustion.error, core::CommandSequencerError::exhausted);
  EXPECT_EQ(second_exhaustion, first_exhaustion);
}

TEST(CommandSequencer, RejectsZeroAsAConfiguredFirstSequence) {
  EXPECT_THROW(static_cast<void>(core::CommandSequencer{domain::Sequence{}}),
               std::invalid_argument);
}

TEST(CommandAdmission, AllocatesSequenceBeforePureAndStateValidation) {
  core::InstrumentBook book{instrument_id};
  core::CommandAdmission admission{book, policy};
  auto pure_invalid = new_order();
  pure_invalid.client_id = {};
  auto wrong_route = cancel_order();
  wrong_route.instrument_id = domain::InstrumentId{99U};

  const auto pure_reject = admission.admit(pure_invalid);
  const auto state_reject = admission.admit(wrong_route);
  const auto accepted = admission.admit(new_order());

  EXPECT_TRUE(pure_reject.rejected());
  EXPECT_EQ(pure_reject.command_sequence, domain::Sequence{1U});
  EXPECT_EQ(pure_reject.reject_reason, domain::RejectReason::invalid_client_id);
  EXPECT_TRUE(state_reject.rejected());
  EXPECT_EQ(state_reject.command_sequence, domain::Sequence{2U});
  EXPECT_EQ(state_reject.reject_reason, domain::RejectReason::unknown_instrument);
  EXPECT_TRUE(accepted.accepted());
  EXPECT_EQ(accepted.command_sequence, domain::Sequence{3U});
}

TEST(CommandAdmission, PreservesCommandTypeAndRelevantOrderIdThroughVariantAdmission) {
  core::InstrumentBook book{instrument_id};
  core::CommandAdmission admission{book, policy};
  const domain::Command command{cancel_order(919U)};

  const auto result = admission.admit(command);

  EXPECT_EQ(result.command_type, domain::CommandType::cancel);
  EXPECT_EQ(result.reject_reason, domain::RejectReason::unknown_order_id);
  EXPECT_EQ(result.relevant_order_id, domain::OrderId{919U});
}

TEST(CommandAdmission, ReportsSequenceExhaustionAsInternalNotDomainRejection) {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  core::InstrumentBook book{instrument_id};
  core::CommandAdmission admission{book, policy, domain::Sequence{maximum}};
  auto invalid = new_order();
  invalid.client_id = {};

  const auto maximum_reject = admission.admit(invalid);
  const auto exhaustion = admission.admit(new_order());
  const auto sticky_exhaustion = admission.admit(cancel_order());

  EXPECT_TRUE(maximum_reject.rejected());
  EXPECT_EQ(maximum_reject.command_sequence, domain::Sequence{maximum});
  EXPECT_EQ(maximum_reject.reject_reason, domain::RejectReason::invalid_client_id);

  EXPECT_FALSE(exhaustion.processed());
  EXPECT_FALSE(exhaustion.accepted());
  EXPECT_FALSE(exhaustion.rejected());
  EXPECT_EQ(exhaustion.command_sequence, domain::Sequence{});
  EXPECT_EQ(exhaustion.reject_reason, domain::RejectReason::none);
  EXPECT_EQ(exhaustion.internal_error, core::CommandAdmissionError::sequence_exhausted);
  EXPECT_EQ(sticky_exhaustion.internal_error, core::CommandAdmissionError::sequence_exhausted);
  EXPECT_EQ(sticky_exhaustion.reject_reason, domain::RejectReason::none);
}

TEST(CommandAdmission, RejectsInvalidExecutionPolicyAtConstruction) {
  core::InstrumentBook book{instrument_id};
  auto zero_quantity = policy;
  zero_quantity.max_order_quantity = {};
  auto zero_tick = policy;
  zero_tick.tick_increment = {};
  auto negative_tick = policy;
  negative_tick.tick_increment = domain::PriceTicks{-5};

  EXPECT_THROW(static_cast<void>(core::CommandAdmission{book, zero_quantity}),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(core::CommandAdmission{book, zero_tick}), std::invalid_argument);
  EXPECT_THROW(static_cast<void>(core::CommandAdmission{book, negative_tick}),
               std::invalid_argument);
}

TEST(StateValidation, ReportsInvalidPolicyAsInternalWithoutARejectReason) {
  core::InstrumentBook book{instrument_id};
  auto invalid_policy = policy;
  invalid_policy.tick_increment = {};

  const auto new_result = core::validate_state(new_order(), book, invalid_policy);
  const auto cancel_result = core::validate_state(cancel_order(), book, invalid_policy);
  const auto replace_result = core::validate_state(replace_order(), book, invalid_policy);

  for (const auto& result : {new_result, cancel_result, replace_result}) {
    EXPECT_FALSE(result.accepted());
    EXPECT_FALSE(result.rejected());
    EXPECT_EQ(result.reason, domain::RejectReason::none);
    EXPECT_EQ(result.internal_error, core::StateValidationError::invalid_policy);
    EXPECT_EQ(result.relevant_order_id, std::nullopt);
  }
}

TEST(CommandAdmission, ConsumesSequenceAndReportsCorruptBookAsInternal) {
  core::InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(node_spec(101U, 1U)));
  auto& index = core::test::CoreAccess::active_index(book);
  ASSERT_TRUE(core::test::CoreAccess::insert_index_entry(index, domain::OrderId{999U}, nullptr));
  core::CommandAdmission admission{book, policy};
  auto malformed = new_order(103U);
  malformed.client_id = {};

  const auto corrupt = admission.admit(malformed);

  EXPECT_EQ(corrupt.command_sequence, domain::Sequence{1U});
  EXPECT_EQ(corrupt.reject_reason, domain::RejectReason::none);
  EXPECT_EQ(corrupt.relevant_order_id, std::nullopt);
  EXPECT_EQ(corrupt.internal_error, core::CommandAdmissionError::book_invariant_violation);
  EXPECT_FALSE(corrupt.accepted());
  EXPECT_FALSE(corrupt.rejected());

  ASSERT_TRUE(core::test::CoreAccess::erase_index_entry(index, domain::OrderId{999U}));
  const auto recovered = admission.admit(new_order(103U));
  EXPECT_TRUE(recovered.accepted());
  EXPECT_EQ(recovered.command_sequence, domain::Sequence{2U});
}

TEST(NewStateValidation, AppliesPureRouteQuantityTickAndDuplicatePrecedence) {
  core::InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(node_spec(101U, 1U)));
  ASSERT_TRUE(book.rest(node_spec(102U, 2U, client_id, domain::Side::buy, 9'995)));
  auto order = new_order(101U);
  order.client_id = {};
  order.instrument_id = domain::InstrumentId{99U};
  order.quantity = domain::Quantity{101U};
  order.limit_price = domain::PriceTicks{10'002};

  EXPECT_EQ(core::validate_state(order, book, policy).reason,
            domain::RejectReason::invalid_client_id);
  order.client_id = client_id;
  EXPECT_EQ(core::validate_state(order, book, policy).reason,
            domain::RejectReason::unknown_instrument);
  order.instrument_id = instrument_id;
  EXPECT_EQ(core::validate_state(order, book, policy).reason,
            domain::RejectReason::quantity_out_of_range);
  order.quantity = domain::Quantity{10U};
  EXPECT_EQ(core::validate_state(order, book, policy).reason, domain::RejectReason::invalid_tick);
  order.limit_price = domain::PriceTicks{10'000};
  EXPECT_EQ(core::validate_state(order, book, policy).reason,
            domain::RejectReason::duplicate_order_id);
  order.order_id = domain::OrderId{103U};
  EXPECT_TRUE(core::validate_state(order, book, policy));
}

TEST(NewStateValidation, DefersCapacityAndDoesNotApplyTickAlignmentToMarketOrders) {
  core::InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(node_spec(101U, 1U)));
  ASSERT_TRUE(book.rest(node_spec(102U, 2U, client_id, domain::Side::buy, 9'995)));
  ASSERT_EQ(book.active_order_count(), policy.max_active_orders);
  auto order = new_order(103U);
  order.order_type = domain::OrderType::market;
  order.time_in_force = domain::TimeInForce::ioc;
  order.limit_price.reset();

  EXPECT_TRUE(core::validate_state(order, book, policy));
}

TEST(NewStateValidation, AllowsAnInactiveOrderIdToBeReused) {
  core::InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(node_spec(101U, 1U)));
  EXPECT_EQ(core::validate_state(new_order(101U), book, policy).reason,
            domain::RejectReason::duplicate_order_id);
  ASSERT_TRUE(book.cancel(domain::OrderId{101U}));

  EXPECT_TRUE(core::validate_state(new_order(101U), book, policy));
}

TEST(CancelStateValidation, AppliesPureRouteUnknownAndOwnershipPrecedence) {
  core::InstrumentBook book{instrument_id};
  const auto rested = book.rest(node_spec(101U, 1U));
  ASSERT_TRUE(rested);
  auto order = cancel_order(999U);
  order.client_id = {};
  order.instrument_id = domain::InstrumentId{99U};

  EXPECT_EQ(core::validate_state(order, book, policy).reason,
            domain::RejectReason::invalid_client_id);
  order.client_id = client_id;
  EXPECT_EQ(core::validate_state(order, book, policy).reason,
            domain::RejectReason::unknown_instrument);
  order.instrument_id = instrument_id;
  EXPECT_EQ(core::validate_state(order, book, policy).reason,
            domain::RejectReason::unknown_order_id);
  order.order_id = domain::OrderId{101U};
  order.client_id = domain::ClientId{18U};
  EXPECT_EQ(core::validate_state(order, book, policy).reason,
            domain::RejectReason::ownership_mismatch);

  order.client_id = client_id;
  core::test::CoreAccess::set_instrument_id(*rested.node, domain::InstrumentId{8U});
  const auto corrupt_book = core::validate_state(order, book, policy);
  EXPECT_EQ(corrupt_book.reason, domain::RejectReason::none);
  EXPECT_EQ(corrupt_book.internal_error, core::StateValidationError::book_invariant_violation);
  EXPECT_EQ(corrupt_book.relevant_order_id, std::nullopt);
  core::test::CoreAccess::set_instrument_id(*rested.node, instrument_id);
  EXPECT_TRUE(core::validate_state(order, book, policy));
}

TEST(ReplaceStateValidation, AppliesSpecifiedDeterministicPrecedence) {
  core::InstrumentBook book{instrument_id};
  const auto original = book.rest(node_spec(101U, 1U));
  ASSERT_TRUE(original);
  ASSERT_TRUE(book.rest(node_spec(102U, 2U, client_id, domain::Side::buy, 9'995)));
  auto order = replace_order(999U, 102U);
  order.client_id = {};
  order.instrument_id = domain::InstrumentId{99U};
  order.new_quantity = domain::Quantity{101U};
  order.new_limit_price = domain::PriceTicks{10'002};

  EXPECT_EQ(core::validate_state(order, book, policy).reason,
            domain::RejectReason::invalid_client_id);
  order.client_id = client_id;
  EXPECT_EQ(core::validate_state(order, book, policy).reason,
            domain::RejectReason::unknown_instrument);
  order.instrument_id = instrument_id;
  EXPECT_EQ(core::validate_state(order, book, policy).reason,
            domain::RejectReason::quantity_out_of_range);
  order.new_quantity = domain::Quantity{11U};
  EXPECT_EQ(core::validate_state(order, book, policy).reason, domain::RejectReason::invalid_tick);
  order.new_limit_price = domain::PriceTicks{10'005};
  EXPECT_EQ(core::validate_state(order, book, policy).reason,
            domain::RejectReason::unknown_order_id);
  order.old_order_id = domain::OrderId{101U};
  order.client_id = domain::ClientId{18U};
  EXPECT_EQ(core::validate_state(order, book, policy).reason,
            domain::RejectReason::ownership_mismatch);

  order.client_id = client_id;
  core::test::CoreAccess::set_instrument_id(*original.node, domain::InstrumentId{8U});
  const auto corrupt_book = core::validate_state(order, book, policy);
  EXPECT_EQ(corrupt_book.reason, domain::RejectReason::none);
  EXPECT_EQ(corrupt_book.internal_error, core::StateValidationError::book_invariant_violation);
  EXPECT_EQ(corrupt_book.relevant_order_id, std::nullopt);
  core::test::CoreAccess::set_instrument_id(*original.node, instrument_id);
  EXPECT_EQ(core::validate_state(order, book, policy).reason,
            domain::RejectReason::invalid_replacement_id);
}

TEST(ReplaceStateValidation, DefersFinalCapacityAndAllowsAdmissionAtConfiguredCapacity) {
  core::InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(node_spec(101U, 1U)));
  ASSERT_TRUE(book.rest(node_spec(102U, 2U, client_id, domain::Side::buy, 9'995)));

  EXPECT_TRUE(core::validate_state(replace_order(101U, 103U), book, policy));
  EXPECT_EQ(book.active_order_count(), policy.max_active_orders);
}

TEST(StateValidation, AttributesReplacementIdFailuresToTheRelevantId) {
  core::InstrumentBook book{instrument_id};
  ASSERT_TRUE(book.rest(node_spec(101U, 1U)));
  ASSERT_TRUE(book.rest(node_spec(102U, 2U, client_id, domain::Side::buy, 9'995)));

  const auto unknown_old = core::validate_state(replace_order(999U, 103U), book, policy);
  EXPECT_EQ(unknown_old.relevant_order_id, domain::OrderId{999U});

  const auto active_new = core::validate_state(replace_order(101U, 102U), book, policy);
  EXPECT_EQ(active_new.reason, domain::RejectReason::invalid_replacement_id);
  EXPECT_EQ(active_new.relevant_order_id, domain::OrderId{102U});

  auto invalid_same_id = replace_order(101U, 101U);
  const auto same_id = core::validate_state(invalid_same_id, book, policy);
  EXPECT_EQ(same_id.reason, domain::RejectReason::invalid_replacement_id);
  EXPECT_EQ(same_id.relevant_order_id, domain::OrderId{101U});

  auto missing_new_id = replace_order(101U, 102U);
  missing_new_id.new_order_id = {};
  const auto invalid_new_id = core::validate_state(missing_new_id, book, policy);
  EXPECT_EQ(invalid_new_id.reason, domain::RejectReason::invalid_order_id);
  EXPECT_EQ(invalid_new_id.relevant_order_id, std::nullopt);

  auto invalid_new_quantity = replace_order(101U, 103U);
  invalid_new_quantity.new_quantity = {};
  const auto zero_quantity = core::validate_state(invalid_new_quantity, book, policy);
  EXPECT_EQ(zero_quantity.reason, domain::RejectReason::invalid_quantity);
  EXPECT_EQ(zero_quantity.relevant_order_id, domain::OrderId{103U});
}

TEST(CommandAdmissionVocabulary, HasStableStringRepresentations) {
  EXPECT_EQ(core::to_string(core::CommandSequencerError::exhausted), "exhausted");
  EXPECT_EQ(core::to_string(core::CommandAdmissionError::sequence_exhausted), "sequence_exhausted");
  EXPECT_EQ(core::to_string(core::CommandAdmissionError::book_invariant_violation),
            "book_invariant_violation");
  EXPECT_EQ(core::to_string(core::StateValidationError::invalid_policy), "invalid_policy");
  EXPECT_EQ(core::to_string(core::StateValidationError::book_invariant_violation),
            "book_invariant_violation");
  EXPECT_EQ(core::to_string(static_cast<core::CommandSequencerError>(255U)), "unknown");
  EXPECT_EQ(core::to_string(static_cast<core::CommandAdmissionError>(255U)), "unknown");
  EXPECT_EQ(core::to_string(static_cast<core::StateValidationError>(255U)), "unknown");
}

}  // namespace
