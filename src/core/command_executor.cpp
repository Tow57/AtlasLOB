#include "command_executor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace atlaslob::core {
namespace {

struct BoundPlan final {
  std::vector<PrevalidatedBookReduction> reductions;
  PassiveBindingError error{PassiveBindingError::none};

  [[nodiscard]] bool has_value() const noexcept { return error == PassiveBindingError::none; }
};

template <domain::Side RestingSide>
[[nodiscard]] std::uint64_t maximum_priority(const BookSide<RestingSide>& side) noexcept {
  std::uint64_t maximum = 0U;
  for (const PriceLevel& level : side) {
    for (const OrderNode* node = level.head(); node != nullptr; node = node->next()) {
      maximum = std::max(maximum, node->priority_sequence().value());
    }
  }
  return maximum;
}

[[nodiscard]] std::uint64_t maximum_priority(const InstrumentBook& book) noexcept {
  return std::max(maximum_priority(book.bids()), maximum_priority(book.asks()));
}

[[nodiscard]] CommandExecutionResult fail(CommandExecutionError error) {
  CommandExecutionResult result;
  result.error = error;
  return result;
}

[[nodiscard]] bool crosses(const domain::NewOrder& order,
                           domain::PriceTicks resting_price) noexcept {
  if (order.order_type == domain::OrderType::market) {
    return true;
  }
  if (order.side == domain::Side::buy) {
    return order.limit_price.value() >= resting_price;
  }
  return order.limit_price.value() <= resting_price;
}

[[nodiscard]] ResidualDisposition expected_disposition(const domain::NewOrder& order,
                                                       domain::Quantity residual) noexcept {
  if (residual.value() == 0U) {
    return ResidualDisposition::filled;
  }
  if (order.order_type == domain::OrderType::market) {
    return ResidualDisposition::market_exhausted;
  }
  if (order.time_in_force == domain::TimeInForce::ioc) {
    return ResidualDisposition::ioc_canceled;
  }
  return ResidualDisposition::rest;
}

[[nodiscard]] bool planned_trade_matches(const PlannedTrade& trade, const OrderNode& node,
                                         std::uint64_t aggressor_before) noexcept {
  const auto expected_execution = std::min(aggressor_before, node.remaining_quantity().value());
  return trade.resting_order_id == node.order_id() && trade.resting_client_id == node.client_id() &&
         trade.execution_price == node.price() &&
         trade.execution_quantity.value() == expected_execution &&
         trade.aggressor_remaining.value() == aggressor_before - expected_execution &&
         trade.resting_remaining_before == node.remaining_quantity() &&
         trade.resting_remaining_after.value() ==
             node.remaining_quantity().value() - expected_execution &&
         trade.expected_resting_priority == node.priority_sequence();
}

template <domain::Side RestingSide>
[[nodiscard]] BoundPlan bind_side(const domain::NewOrder& order, const MatchPlan& plan,
                                  InstrumentBook& book, const BookSide<RestingSide>& opposite) {
  BoundPlan result;
  result.reductions.reserve(plan.trades.size());

  std::size_t trade_index = 0U;
  auto aggressor_remaining = order.quantity.value();
  for (const PriceLevel& level : opposite) {
    if (!crosses(order, level.price()) || aggressor_remaining == 0U) {
      break;
    }
    for (const OrderNode* node = level.head(); node != nullptr && aggressor_remaining != 0U;
         node = node->next()) {
      if (trade_index == plan.trades.size()) {
        result.error = PassiveBindingError::invalid_plan;
        return result;
      }

      const auto& trade = plan.trades[trade_index];
      auto* const mutable_node = book.find(trade.resting_order_id);
      if (mutable_node != node || !planned_trade_matches(trade, *node, aggressor_remaining)) {
        result.error = PassiveBindingError::book_mismatch;
        return result;
      }

      result.reductions.push_back({
          .node = mutable_node,
          .order_id = node->order_id(),
          .client_id = node->client_id(),
          .side = node->side(),
          .price = node->price(),
          .remaining_before = node->remaining_quantity(),
          .reduction = trade.execution_quantity,
          .remaining_after = trade.resting_remaining_after,
          .priority_sequence = node->priority_sequence(),
      });
      aggressor_remaining = trade.aggressor_remaining.value();
      ++trade_index;
    }
  }

  if (trade_index != plan.trades.size()) {
    result.error = PassiveBindingError::invalid_plan;
    return result;
  }
  if (aggressor_remaining != plan.residual_quantity.value()) {
    result.error = PassiveBindingError::quantity_chain_mismatch;
    return result;
  }
  if (plan.residual_disposition != expected_disposition(order, plan.residual_quantity)) {
    result.error = PassiveBindingError::invalid_plan;
    return result;
  }
  return result;
}

[[nodiscard]] BoundPlan bind_match_plan(const domain::NewOrder& order, const MatchPlan& plan,
                                        InstrumentBook& book) {
  if (!domain::is_valid(order.side) || order.quantity.value() == 0U) {
    return {.reductions = {}, .error = PassiveBindingError::invalid_plan};
  }
  if (order.side == domain::Side::buy) {
    return bind_side(order, plan, book, book.asks());
  }
  return bind_side(order, plan, book, book.bids());
}

[[nodiscard]] bool checked_new_event_count(std::size_t trade_count, bool book_changed,
                                           std::size_t& result) noexcept {
  const std::size_t fixed_count = 2U + (book_changed ? 1U : 0U);
  if (trade_count > std::numeric_limits<std::size_t>::max() - fixed_count) {
    return false;
  }
  result = trade_count + fixed_count;
  return result != 0U &&
         result - 1U <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
}

[[nodiscard]] bool checked_replace_event_count(std::size_t trade_count, bool book_changed,
                                               std::size_t& result) noexcept {
  const std::size_t fixed_count = 5U + (book_changed ? 1U : 0U);
  if (trade_count > std::numeric_limits<std::size_t>::max() - fixed_count) {
    return false;
  }
  result = trade_count + fixed_count;
  return result - 1U <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
}

[[nodiscard]] CommandExecutionResult finish_events(EventBatchBuilder& builder) noexcept {
  auto finished = std::move(builder).finish();
  if (!finished) {
    auto result = fail(CommandExecutionError::event_batch_failure);
    result.event_batch_error = finished.error;
    return result;
  }

  CommandExecutionResult result;
  result.batch.emplace(std::move(*finished.batch));
  return result;
}

[[nodiscard]] CommandExecutionResult make_rejection(domain::Sequence sequence,
                                                    domain::InstrumentId instrument_id,
                                                    domain::CommandType command_type,
                                                    domain::RejectReason reason,
                                                    std::optional<domain::OrderId> order_id) {
  EventBatchBuilder builder{sequence, instrument_id, 1U};
  const auto append_error = builder.append(domain::RejectedEvent{
      .command_type = command_type,
      .reason = reason,
      .order_id = order_id,
  });
  if (append_error != EventBatchBuilderError::none) {
    auto result = fail(CommandExecutionError::event_batch_failure);
    result.event_batch_error = append_error;
    return result;
  }
  return finish_events(builder);
}

[[nodiscard]] CommandExecutionResult admission_failure(const CommandAdmissionResult& admission) {
  auto result = fail(CommandExecutionError::admission_failure);
  result.admission_error = admission.internal_error;
  return result;
}

[[nodiscard]] bool is_representational_capacity(const InstrumentBookStatus& status) noexcept {
  return status.error == InstrumentBookError::price_level_failure &&
         (status.level_error == PriceLevelError::aggregate_overflow ||
          status.level_error == PriceLevelError::order_count_overflow);
}

[[nodiscard]] std::optional<ProjectedRestingResidual> projected_residual(
    const domain::NewOrder& order, const MatchPlan& plan) noexcept {
  if (plan.residual_disposition != ResidualDisposition::rest) {
    return std::nullopt;
  }
  return ProjectedRestingResidual{
      .price = order.limit_price.value(),
      .quantity = plan.residual_quantity,
  };
}

[[nodiscard]] domain::DoneReason done_reason(ResidualDisposition disposition) noexcept {
  switch (disposition) {
    case ResidualDisposition::filled:
      return domain::DoneReason::filled;
    case ResidualDisposition::ioc_canceled:
      return domain::DoneReason::ioc_residual_canceled;
    case ResidualDisposition::market_exhausted:
      return domain::DoneReason::market_exhausted;
    case ResidualDisposition::rest:
      break;
  }
  std::terminate();
}

[[nodiscard]] EventBatchBuilderError append_new_events(EventBatchBuilder& builder,
                                                       const domain::NewOrder& order,
                                                       const MatchPlan& plan,
                                                       const TopOfBookSnapshot& projected_top,
                                                       bool book_changed) noexcept {
  auto error = builder.append(domain::AcceptedEvent{
      .command_type = domain::CommandType::new_order,
  });
  if (error != EventBatchBuilderError::none) {
    return error;
  }

  for (const auto& trade : plan.trades) {
    error = builder.append(domain::TradeEvent{
        .aggressor_order_id = order.order_id,
        .resting_order_id = trade.resting_order_id,
        .aggressor_client_id = order.client_id,
        .resting_client_id = trade.resting_client_id,
        .aggressor_side = order.side,
        .execution_price = trade.execution_price,
        .execution_quantity = trade.execution_quantity,
        .aggressor_remaining = trade.aggressor_remaining,
        .resting_remaining = trade.resting_remaining_after,
    });
    if (error != EventBatchBuilderError::none) {
      return error;
    }
  }

  if (plan.residual_disposition == ResidualDisposition::rest) {
    error = builder.append(domain::RestedEvent{
        .order_id = order.order_id,
        .client_id = order.client_id,
        .side = order.side,
        .price = order.limit_price.value(),
        .remaining_quantity = plan.residual_quantity,
    });
  } else {
    error = builder.append(domain::DoneEvent{
        .order_id = order.order_id,
        .reason = done_reason(plan.residual_disposition),
        .remaining_quantity = plan.residual_quantity,
    });
  }
  if (error != EventBatchBuilderError::none) {
    return error;
  }

  if (book_changed) {
    error = builder.append(domain::BookChangedEvent{
        .best_bid = projected_top.best_bid,
        .best_ask = projected_top.best_ask,
    });
  }
  return error;
}

[[nodiscard]] EventBatchBuilderError append_replace_events(
    EventBatchBuilder& builder, const domain::ReplaceOrder& command,
    const domain::NewOrder& replacement, domain::Quantity old_remaining, const MatchPlan& plan,
    const TopOfBookSnapshot& projected_top, bool book_changed) noexcept {
  auto error = builder.append(domain::AcceptedEvent{
      .command_type = domain::CommandType::replace,
  });
  if (error == EventBatchBuilderError::none) {
    error = builder.append(domain::ReplacedEvent{
        .old_order_id = command.old_order_id,
        .new_order_id = command.new_order_id,
    });
  }
  if (error == EventBatchBuilderError::none) {
    error = builder.append(domain::CanceledEvent{
        .order_id = command.old_order_id,
        .canceled_quantity = old_remaining,
    });
  }
  if (error == EventBatchBuilderError::none) {
    error = builder.append(domain::DoneEvent{
        .order_id = command.old_order_id,
        .reason = domain::DoneReason::replaced,
        .remaining_quantity = old_remaining,
    });
  }
  if (error != EventBatchBuilderError::none) {
    return error;
  }

  for (const auto& trade : plan.trades) {
    error = builder.append(domain::TradeEvent{
        .aggressor_order_id = replacement.order_id,
        .resting_order_id = trade.resting_order_id,
        .aggressor_client_id = replacement.client_id,
        .resting_client_id = trade.resting_client_id,
        .aggressor_side = replacement.side,
        .execution_price = trade.execution_price,
        .execution_quantity = trade.execution_quantity,
        .aggressor_remaining = trade.aggressor_remaining,
        .resting_remaining = trade.resting_remaining_after,
    });
    if (error != EventBatchBuilderError::none) {
      return error;
    }
  }

  if (plan.residual_disposition == ResidualDisposition::rest) {
    error = builder.append(domain::RestedEvent{
        .order_id = replacement.order_id,
        .client_id = replacement.client_id,
        .side = replacement.side,
        .price = replacement.limit_price.value(),
        .remaining_quantity = plan.residual_quantity,
    });
  } else {
    error = builder.append(domain::DoneEvent{
        .order_id = replacement.order_id,
        .reason = done_reason(plan.residual_disposition),
        .remaining_quantity = plan.residual_quantity,
    });
  }
  if (error == EventBatchBuilderError::none && book_changed) {
    error = builder.append(domain::BookChangedEvent{
        .best_bid = projected_top.best_bid,
        .best_ask = projected_top.best_ask,
    });
  }
  return error;
}

}  // namespace

PreparedCommandExecution::PreparedCommandExecution(CommandExecutionResult result) noexcept
    : result_{std::move(result)} {}

PreparedCommandExecution::PreparedCommandExecution(
    InstrumentBook& book, CommandExecutionResult result, MutationKind mutation,
    std::vector<PrevalidatedBookReduction> passive_reductions,
    std::optional<PrevalidatedBookReduction> primary_reduction,
    std::optional<InstrumentBook::PreparedRest> prepared_rest,
    TopOfBookSnapshot projected_top) noexcept
    : book_{&book},
      result_{std::move(result)},
      passive_reductions_{std::move(passive_reductions)},
      primary_reduction_{std::move(primary_reduction)},
      prepared_rest_{std::move(prepared_rest)},
      projected_top_{std::move(projected_top)},
      mutation_{mutation} {}

PreparedCommandExecution::PreparedCommandExecution(PreparedCommandExecution&& other) noexcept
    : lease_owner_{std::exchange(other.lease_owner_, nullptr)},
      book_{std::exchange(other.book_, nullptr)},
      result_{std::move(other.result_)},
      passive_reductions_{std::move(other.passive_reductions_)},
      primary_reduction_{std::move(other.primary_reduction_)},
      prepared_rest_{std::move(other.prepared_rest_)},
      projected_top_{std::move(other.projected_top_)},
      mutation_{std::exchange(other.mutation_, MutationKind::none)},
      consumed_{std::exchange(other.consumed_, true)} {}

PreparedCommandExecution& PreparedCommandExecution::operator=(
    PreparedCommandExecution&& other) noexcept {
  if (this != &other) {
    abandon();
    lease_owner_ = std::exchange(other.lease_owner_, nullptr);
    book_ = std::exchange(other.book_, nullptr);
    result_ = std::move(other.result_);
    passive_reductions_ = std::move(other.passive_reductions_);
    primary_reduction_ = std::move(other.primary_reduction_);
    prepared_rest_ = std::move(other.prepared_rest_);
    projected_top_ = std::move(other.projected_top_);
    mutation_ = std::exchange(other.mutation_, MutationKind::none);
    consumed_ = std::exchange(other.consumed_, true);
  }
  return *this;
}

PreparedCommandExecution::~PreparedCommandExecution() noexcept { abandon(); }

bool PreparedCommandExecution::has_value() const noexcept {
  return !consumed_ && result_.has_value();
}

const domain::EventBatch* PreparedCommandExecution::batch() const noexcept {
  return has_value() ? &*result_.batch : nullptr;
}

bool PreparedCommandExecution::rejected() const noexcept {
  const auto* const prepared_batch = batch();
  return prepared_batch != nullptr &&
         domain::event_type((*prepared_batch)[0]) == domain::EventType::rejected;
}

bool PreparedCommandExecution::committed() const noexcept { return has_value() && !rejected(); }

void PreparedCommandExecution::abandon() noexcept {
  prepared_rest_.reset();
  book_ = nullptr;
  mutation_ = MutationKind::none;
  consumed_ = true;
  release_lease();
}

void PreparedCommandExecution::release_lease() noexcept {
  if (lease_owner_ == nullptr) {
    return;
  }
  if (!lease_owner_->preparation_active_) {
    std::terminate();
  }
  lease_owner_->preparation_active_ = false;
  lease_owner_ = nullptr;
}

CommandExecutionResult PreparedCommandExecution::commit() noexcept {
  if (consumed_) {
    auto consumed = fail(CommandExecutionError::book_mutation_failure);
    consumed.book_mutation_error = PrevalidatedBatchError::preparation_mismatch;
    return consumed;
  }

  if (mutation_ == MutationKind::none) {
    consumed_ = true;
    release_lease();
    return std::move(result_);
  }

  PrevalidatedBatchStatus mutation_status{
      .error = PrevalidatedBatchError::preparation_mismatch,
  };
  if (book_ != nullptr && result_.has_value()) {
    auto* const prepared_rest = prepared_rest_.has_value() ? &*prepared_rest_ : nullptr;
    if (mutation_ == MutationKind::replace && primary_reduction_.has_value()) {
      mutation_status = book_->apply_prevalidated_replace_batch(*primary_reduction_,
                                                                passive_reductions_, prepared_rest);
    } else if (mutation_ == MutationKind::batch) {
      const std::span<const PrevalidatedBookReduction> reductions =
          primary_reduction_.has_value()
              ? std::span<const PrevalidatedBookReduction>{&*primary_reduction_, 1U}
              : std::span<const PrevalidatedBookReduction>{passive_reductions_};
      mutation_status = book_->apply_prevalidated_batch(reductions, prepared_rest);
    }
  }

  if (!mutation_status) {
    result_.batch.reset();
    result_.error = CommandExecutionError::book_mutation_failure;
    result_.book_mutation_error = mutation_status.error;
  } else if (snapshot_top_of_book(*book_) != projected_top_) {
    std::terminate();
  }

  prepared_rest_.reset();
  book_ = nullptr;
  mutation_ = MutationKind::none;
  consumed_ = true;
  release_lease();
  return std::move(result_);
}

CommandExecutor::CommandExecutor(InstrumentBook& book, ExecutionPolicy policy)
    : book_{book}, admission_{book, policy} {
  if (!book_.validate_invariants()) {
    throw std::invalid_argument{"CommandExecutor requires a valid initial book"};
  }
  if (book_.has_pending_preparation()) {
    throw std::invalid_argument{"CommandExecutor cannot attach during a pending book preparation"};
  }
  if (!book_.empty()) {
    throw std::invalid_argument{
        "default CommandExecutor sequencing requires an empty initial book"};
  }
}

CommandExecutor::CommandExecutor(InstrumentBook& book, ExecutionPolicy policy,
                                 domain::Sequence first_sequence)
    : book_{book}, admission_{book, policy, first_sequence} {
  if (!book_.validate_invariants()) {
    throw std::invalid_argument{"CommandExecutor requires a valid initial book"};
  }
  if (book_.has_pending_preparation()) {
    throw std::invalid_argument{"CommandExecutor cannot attach during a pending book preparation"};
  }
  if (first_sequence.value() <= maximum_priority(book_)) {
    throw std::invalid_argument{"CommandExecutor first sequence must exceed every active priority"};
  }
}

CommandExecutor::~CommandExecutor() noexcept {
  if (preparation_active_) {
    std::terminate();
  }
}

CommandExecutionResult CommandExecutor::execute(const domain::NewOrder& order) {
  if (preparation_active_) {
    return fail(CommandExecutionError::preparation_in_progress);
  }
  preparation_active_ = true;
  try {
    const auto admission = admission_.admit(order);
    auto prepared = prepare_admitted(order, admission);
    prepared.lease_owner_ = this;
    return prepared.commit();
  } catch (...) {
    preparation_active_ = false;
    throw;
  }
}

CommandExecutionResult CommandExecutor::execute_at(const domain::NewOrder& order,
                                                   domain::Sequence sequence) {
  return prepare_at(order, sequence).commit();
}

PreparedCommandExecution CommandExecutor::prepare_at(const domain::NewOrder& order,
                                                     domain::Sequence sequence) {
  if (preparation_active_) {
    return PreparedCommandExecution{fail(CommandExecutionError::preparation_in_progress)};
  }
  preparation_active_ = true;
  try {
    const auto admission = admission_.admit_at(order, sequence);
    auto prepared = prepare_admitted(order, admission);
    prepared.lease_owner_ = this;
    return prepared;
  } catch (...) {
    preparation_active_ = false;
    throw;
  }
}

PreparedCommandExecution CommandExecutor::prepare_admitted(
    const domain::NewOrder& order, const CommandAdmissionResult& admission) {
  if (!admission.processed()) {
    return PreparedCommandExecution{admission_failure(admission)};
  }
  if (admission.rejected()) {
    return PreparedCommandExecution{make_rejection(
        admission.command_sequence, order.instrument_id, domain::CommandType::new_order,
        admission.reject_reason, admission.relevant_order_id)};
  }

  const auto before = snapshot_top_of_book(book_);
  auto planned = plan_matches(order, book_);
  if (!planned) {
    auto result = fail(CommandExecutionError::match_plan_failure);
    result.match_plan_error = planned.error;
    return PreparedCommandExecution{std::move(result)};
  }

  const auto active_projection =
      project_active_order_count(planned.plan, book_.active_order_count(), false);
  if (!active_projection) {
    auto result = fail(CommandExecutionError::active_order_projection_failure);
    result.active_order_projection_error = active_projection.error;
    return PreparedCommandExecution{std::move(result)};
  }
  if (!active_projection.within_limit(admission_.policy().max_active_orders)) {
    return PreparedCommandExecution{make_rejection(
        admission.command_sequence, order.instrument_id, domain::CommandType::new_order,
        domain::RejectReason::capacity_exceeded, order.order_id)};
  }

  auto bound = bind_match_plan(order, planned.plan, book_);
  if (!bound.has_value()) {
    auto result = fail(CommandExecutionError::passive_binding_failure);
    result.passive_binding_error = bound.error;
    return PreparedCommandExecution{std::move(result)};
  }

  const auto residual = projected_residual(order, planned.plan);
  const auto top_projection = project_new_top_of_book(book_, order.side, planned.plan, residual);
  if (!top_projection) {
    if (top_projection.error == ExecutionProjectionError::aggregate_overflow) {
      return PreparedCommandExecution{make_rejection(
          admission.command_sequence, order.instrument_id, domain::CommandType::new_order,
          domain::RejectReason::capacity_exceeded, order.order_id)};
    }
    auto result = fail(CommandExecutionError::top_of_book_projection_failure);
    result.top_of_book_projection_error = top_projection.error;
    return PreparedCommandExecution{std::move(result)};
  }

  std::optional<InstrumentBook::PreparedRest> prepared_rest;
  if (residual.has_value()) {
    auto prepared = book_.prepare_rest({
        .order_id = order.order_id,
        .client_id = order.client_id,
        .instrument_id = order.instrument_id,
        .side = order.side,
        .price = residual->price,
        .remaining_quantity = residual->quantity,
        .priority_sequence = admission.command_sequence,
    });
    if (!prepared) {
      if (is_representational_capacity(prepared.status())) {
        return PreparedCommandExecution{make_rejection(
            admission.command_sequence, order.instrument_id, domain::CommandType::new_order,
            domain::RejectReason::capacity_exceeded, order.order_id)};
      }
      auto result = fail(CommandExecutionError::residual_preparation_failure);
      result.residual_preparation_status = prepared.status();
      return PreparedCommandExecution{std::move(result)};
    }
    prepared_rest.emplace(std::move(prepared));
  }

#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
  if (before_event_allocation_hook_ != nullptr) {
    before_event_allocation_hook_();
  }
#endif

  const bool book_changed = before != top_projection.snapshot;
  std::size_t event_count{};
  if (!checked_new_event_count(planned.plan.trades.size(), book_changed, event_count)) {
    return PreparedCommandExecution{fail(CommandExecutionError::event_count_overflow)};
  }

  EventBatchBuilder builder{admission.command_sequence, order.instrument_id, event_count};
  const auto append_error =
      append_new_events(builder, order, planned.plan, top_projection.snapshot, book_changed);
  if (append_error != EventBatchBuilderError::none) {
    auto result = fail(CommandExecutionError::event_batch_failure);
    result.event_batch_error = append_error;
    return PreparedCommandExecution{std::move(result)};
  }
  auto result = finish_events(builder);
  if (!result) {
    return PreparedCommandExecution{std::move(result)};
  }

  return PreparedCommandExecution{
      book_,
      std::move(result),
      PreparedCommandExecution::MutationKind::batch,
      std::move(bound.reductions),
      std::nullopt,
      std::move(prepared_rest),
      top_projection.snapshot,
  };
}

CommandExecutionResult CommandExecutor::execute(const domain::CancelOrder& order) {
  if (preparation_active_) {
    return fail(CommandExecutionError::preparation_in_progress);
  }
  preparation_active_ = true;
  try {
    const auto admission = admission_.admit(order);
    auto prepared = prepare_admitted(order, admission);
    prepared.lease_owner_ = this;
    return prepared.commit();
  } catch (...) {
    preparation_active_ = false;
    throw;
  }
}

CommandExecutionResult CommandExecutor::execute_at(const domain::CancelOrder& order,
                                                   domain::Sequence sequence) {
  return prepare_at(order, sequence).commit();
}

PreparedCommandExecution CommandExecutor::prepare_at(const domain::CancelOrder& order,
                                                     domain::Sequence sequence) {
  if (preparation_active_) {
    return PreparedCommandExecution{fail(CommandExecutionError::preparation_in_progress)};
  }
  preparation_active_ = true;
  try {
    const auto admission = admission_.admit_at(order, sequence);
    auto prepared = prepare_admitted(order, admission);
    prepared.lease_owner_ = this;
    return prepared;
  } catch (...) {
    preparation_active_ = false;
    throw;
  }
}

PreparedCommandExecution CommandExecutor::prepare_admitted(
    const domain::CancelOrder& order, const CommandAdmissionResult& admission) {
  if (!admission.processed()) {
    return PreparedCommandExecution{admission_failure(admission)};
  }
  if (admission.rejected()) {
    return PreparedCommandExecution{
        make_rejection(admission.command_sequence, order.instrument_id, domain::CommandType::cancel,
                       admission.reject_reason, admission.relevant_order_id)};
  }

  const auto before = snapshot_top_of_book(book_);
  auto* const node = book_.find(order.order_id);
  if (node == nullptr || node->client_id() != order.client_id ||
      node->instrument_id() != order.instrument_id) {
    auto result = fail(CommandExecutionError::passive_binding_failure);
    result.passive_binding_error = PassiveBindingError::book_mismatch;
    return PreparedCommandExecution{std::move(result)};
  }

  const auto target = make_cancel_projection_target(*node);
  const auto projected = project_cancel_top_of_book(book_, target);
  if (!projected) {
    auto result = fail(CommandExecutionError::top_of_book_projection_failure);
    result.top_of_book_projection_error = projected.error;
    return PreparedCommandExecution{std::move(result)};
  }

  const auto canceled_quantity = node->remaining_quantity();
  const PrevalidatedBookReduction reduction{
      .node = node,
      .order_id = node->order_id(),
      .client_id = node->client_id(),
      .side = node->side(),
      .price = node->price(),
      .remaining_before = canceled_quantity,
      .reduction = canceled_quantity,
      .remaining_after = {},
      .priority_sequence = node->priority_sequence(),
  };

  const bool book_changed = before != projected.snapshot;
  EventBatchBuilder builder{admission.command_sequence, order.instrument_id,
                            3U + (book_changed ? 1U : 0U)};
  auto append_error = builder.append(domain::AcceptedEvent{
      .command_type = domain::CommandType::cancel,
  });
  if (append_error == EventBatchBuilderError::none) {
    append_error = builder.append(domain::CanceledEvent{
        .order_id = order.order_id,
        .canceled_quantity = canceled_quantity,
    });
  }
  if (append_error == EventBatchBuilderError::none) {
    append_error = builder.append(domain::DoneEvent{
        .order_id = order.order_id,
        .reason = domain::DoneReason::canceled,
        .remaining_quantity = canceled_quantity,
    });
  }
  if (append_error == EventBatchBuilderError::none && book_changed) {
    append_error = builder.append(domain::BookChangedEvent{
        .best_bid = projected.snapshot.best_bid,
        .best_ask = projected.snapshot.best_ask,
    });
  }
  if (append_error != EventBatchBuilderError::none) {
    auto result = fail(CommandExecutionError::event_batch_failure);
    result.event_batch_error = append_error;
    return PreparedCommandExecution{std::move(result)};
  }
  auto result = finish_events(builder);
  if (!result) {
    return PreparedCommandExecution{std::move(result)};
  }

  return PreparedCommandExecution{
      book_,
      std::move(result),
      PreparedCommandExecution::MutationKind::batch,
      {},
      reduction,
      std::nullopt,
      projected.snapshot,
  };
}

CommandExecutionResult CommandExecutor::execute(const domain::ReplaceOrder& order) {
  if (preparation_active_) {
    return fail(CommandExecutionError::preparation_in_progress);
  }
  preparation_active_ = true;
  try {
    const auto admission = admission_.admit(order);
    auto prepared = prepare_admitted(order, admission);
    prepared.lease_owner_ = this;
    return prepared.commit();
  } catch (...) {
    preparation_active_ = false;
    throw;
  }
}

CommandExecutionResult CommandExecutor::execute_at(const domain::ReplaceOrder& order,
                                                   domain::Sequence sequence) {
  return prepare_at(order, sequence).commit();
}

PreparedCommandExecution CommandExecutor::prepare_at(const domain::ReplaceOrder& order,
                                                     domain::Sequence sequence) {
  if (preparation_active_) {
    return PreparedCommandExecution{fail(CommandExecutionError::preparation_in_progress)};
  }
  preparation_active_ = true;
  try {
    const auto admission = admission_.admit_at(order, sequence);
    auto prepared = prepare_admitted(order, admission);
    prepared.lease_owner_ = this;
    return prepared;
  } catch (...) {
    preparation_active_ = false;
    throw;
  }
}

PreparedCommandExecution CommandExecutor::prepare_admitted(
    const domain::ReplaceOrder& order, const CommandAdmissionResult& admission) {
  if (!admission.processed()) {
    return PreparedCommandExecution{admission_failure(admission)};
  }
  if (admission.rejected()) {
    return PreparedCommandExecution{make_rejection(
        admission.command_sequence, order.instrument_id, domain::CommandType::replace,
        admission.reject_reason, admission.relevant_order_id)};
  }

  const auto before = snapshot_top_of_book(book_);
  auto* const old_node = book_.find(order.old_order_id);
  if (old_node == nullptr || old_node->client_id() != order.client_id ||
      old_node->instrument_id() != order.instrument_id ||
      book_.find(order.new_order_id) != nullptr) {
    auto result = fail(CommandExecutionError::passive_binding_failure);
    result.passive_binding_error = PassiveBindingError::book_mismatch;
    return PreparedCommandExecution{std::move(result)};
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
  const auto old_target = make_cancel_projection_target(*old_node);
  const auto old_remaining = old_node->remaining_quantity();
  const PrevalidatedBookReduction old_reduction{
      .node = old_node,
      .order_id = old_node->order_id(),
      .client_id = old_node->client_id(),
      .side = old_node->side(),
      .price = old_node->price(),
      .remaining_before = old_remaining,
      .reduction = old_remaining,
      .remaining_after = {},
      .priority_sequence = old_node->priority_sequence(),
  };

  auto planned = plan_matches(replacement, book_);
  if (!planned) {
    auto result = fail(CommandExecutionError::match_plan_failure);
    result.match_plan_error = planned.error;
    return PreparedCommandExecution{std::move(result)};
  }

  const auto active_projection =
      project_active_order_count(planned.plan, book_.active_order_count(), true);
  if (!active_projection) {
    auto result = fail(CommandExecutionError::active_order_projection_failure);
    result.active_order_projection_error = active_projection.error;
    return PreparedCommandExecution{std::move(result)};
  }
  if (!active_projection.within_limit(admission_.policy().max_active_orders)) {
    return PreparedCommandExecution{make_rejection(
        admission.command_sequence, order.instrument_id, domain::CommandType::replace,
        domain::RejectReason::capacity_exceeded, order.new_order_id)};
  }

  auto bound = bind_match_plan(replacement, planned.plan, book_);
  if (!bound.has_value()) {
    auto result = fail(CommandExecutionError::passive_binding_failure);
    result.passive_binding_error = bound.error;
    return PreparedCommandExecution{std::move(result)};
  }

  const auto residual = projected_residual(replacement, planned.plan);
  const auto top_projection =
      project_replace_top_of_book(book_, old_target, replacement, planned.plan, residual);
  if (!top_projection) {
    if (top_projection.error == ExecutionProjectionError::aggregate_overflow) {
      return PreparedCommandExecution{make_rejection(
          admission.command_sequence, order.instrument_id, domain::CommandType::replace,
          domain::RejectReason::capacity_exceeded, order.new_order_id)};
    }
    auto result = fail(CommandExecutionError::top_of_book_projection_failure);
    result.top_of_book_projection_error = top_projection.error;
    return PreparedCommandExecution{std::move(result)};
  }

  std::optional<InstrumentBook::PreparedRest> prepared_rest;
  if (residual.has_value()) {
    auto prepared = book_.prepare_replace_rest(
        {
            .order_id = replacement.order_id,
            .client_id = replacement.client_id,
            .instrument_id = replacement.instrument_id,
            .side = replacement.side,
            .price = residual->price,
            .remaining_quantity = residual->quantity,
            .priority_sequence = admission.command_sequence,
        },
        *old_node);
    if (!prepared) {
      if (is_representational_capacity(prepared.status())) {
        return PreparedCommandExecution{make_rejection(
            admission.command_sequence, order.instrument_id, domain::CommandType::replace,
            domain::RejectReason::capacity_exceeded, order.new_order_id)};
      }
      auto result = fail(CommandExecutionError::residual_preparation_failure);
      result.residual_preparation_status = prepared.status();
      return PreparedCommandExecution{std::move(result)};
    }
    prepared_rest.emplace(std::move(prepared));
  }

#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
  if (before_event_allocation_hook_ != nullptr) {
    before_event_allocation_hook_();
  }
#endif

  const bool book_changed = before != top_projection.snapshot;
  std::size_t event_count{};
  if (!checked_replace_event_count(planned.plan.trades.size(), book_changed, event_count)) {
    return PreparedCommandExecution{fail(CommandExecutionError::event_count_overflow)};
  }

  EventBatchBuilder builder{admission.command_sequence, order.instrument_id, event_count};
  const auto append_error =
      append_replace_events(builder, order, replacement, old_remaining, planned.plan,
                            top_projection.snapshot, book_changed);
  if (append_error != EventBatchBuilderError::none) {
    auto result = fail(CommandExecutionError::event_batch_failure);
    result.event_batch_error = append_error;
    return PreparedCommandExecution{std::move(result)};
  }
  auto result = finish_events(builder);
  if (!result) {
    return PreparedCommandExecution{std::move(result)};
  }

  return PreparedCommandExecution{
      book_,
      std::move(result),
      PreparedCommandExecution::MutationKind::replace,
      std::move(bound.reductions),
      old_reduction,
      std::move(prepared_rest),
      top_projection.snapshot,
  };
}

}  // namespace atlaslob::core
