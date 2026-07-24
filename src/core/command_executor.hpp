#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "atlaslob/domain/commands.hpp"
#include "atlaslob/domain/event_batch.hpp"
#include "command_admission.hpp"
#include "event_batch_builder.hpp"
#include "execution_policy.hpp"
#include "execution_projection.hpp"
#include "instrument_book.hpp"
#include "match_plan.hpp"

namespace atlaslob::core {

enum class CommandExecutionError : std::uint8_t {
  none = 0,
  admission_failure = 1,
  match_plan_failure = 2,
  active_order_projection_failure = 3,
  passive_binding_failure = 4,
  top_of_book_projection_failure = 5,
  residual_preparation_failure = 6,
  event_count_overflow = 7,
  event_batch_failure = 8,
  book_mutation_failure = 9,
};

[[nodiscard]] constexpr std::string_view to_string(CommandExecutionError error) noexcept {
  switch (error) {
    case CommandExecutionError::none:
      return "none";
    case CommandExecutionError::admission_failure:
      return "admission_failure";
    case CommandExecutionError::match_plan_failure:
      return "match_plan_failure";
    case CommandExecutionError::active_order_projection_failure:
      return "active_order_projection_failure";
    case CommandExecutionError::passive_binding_failure:
      return "passive_binding_failure";
    case CommandExecutionError::top_of_book_projection_failure:
      return "top_of_book_projection_failure";
    case CommandExecutionError::residual_preparation_failure:
      return "residual_preparation_failure";
    case CommandExecutionError::event_count_overflow:
      return "event_count_overflow";
    case CommandExecutionError::event_batch_failure:
      return "event_batch_failure";
    case CommandExecutionError::book_mutation_failure:
      return "book_mutation_failure";
  }
  return "unknown";
}

enum class PassiveBindingError : std::uint8_t {
  none = 0,
  invalid_plan = 1,
  book_mismatch = 2,
  quantity_chain_mismatch = 3,
};

[[nodiscard]] constexpr std::string_view to_string(PassiveBindingError error) noexcept {
  switch (error) {
    case PassiveBindingError::none:
      return "none";
    case PassiveBindingError::invalid_plan:
      return "invalid_plan";
    case PassiveBindingError::book_mismatch:
      return "book_mismatch";
    case PassiveBindingError::quantity_chain_mismatch:
      return "quantity_chain_mismatch";
  }
  return "unknown";
}

struct CommandExecutionResult final {
  std::optional<domain::EventBatch> batch{};
  CommandExecutionError error{CommandExecutionError::none};
  CommandAdmissionError admission_error{CommandAdmissionError::none};
  MatchPlanError match_plan_error{MatchPlanError::none};
  ActiveOrderProjectionError active_order_projection_error{ActiveOrderProjectionError::none};
  PassiveBindingError passive_binding_error{PassiveBindingError::none};
  ExecutionProjectionError top_of_book_projection_error{ExecutionProjectionError::none};
  InstrumentBookStatus residual_preparation_status{};
  PrevalidatedBatchError book_mutation_error{PrevalidatedBatchError::none};
  EventBatchBuilderError event_batch_error{EventBatchBuilderError::none};

  CommandExecutionResult() = default;
  CommandExecutionResult(const CommandExecutionResult&) = delete;
  CommandExecutionResult& operator=(const CommandExecutionResult&) = delete;
  CommandExecutionResult(CommandExecutionResult&&) noexcept = default;
  CommandExecutionResult& operator=(CommandExecutionResult&&) noexcept = default;
  ~CommandExecutionResult() = default;

  [[nodiscard]] bool has_value() const noexcept {
    return batch.has_value() && error == CommandExecutionError::none;
  }

  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
};

class CommandExecutor final {
 public:
  explicit CommandExecutor(InstrumentBook& book, ExecutionPolicy policy = {});
  CommandExecutor(InstrumentBook& book, ExecutionPolicy policy, domain::Sequence first_sequence);

  CommandExecutor(const CommandExecutor&) = delete;
  CommandExecutor& operator=(const CommandExecutor&) = delete;
  CommandExecutor(CommandExecutor&&) = delete;
  CommandExecutor& operator=(CommandExecutor&&) = delete;
  ~CommandExecutor() = default;

  [[nodiscard]] CommandExecutionResult execute(const domain::NewOrder& order);
  [[nodiscard]] CommandExecutionResult execute(const domain::CancelOrder& order);

  [[nodiscard]] const ExecutionPolicy& policy() const noexcept { return admission_.policy(); }
  [[nodiscard]] domain::Sequence next_sequence() const noexcept {
    return admission_.next_sequence();
  }
  [[nodiscard]] bool sequence_exhausted() const noexcept { return admission_.sequence_exhausted(); }

#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
  using BeforeEventAllocationHook = void (*)();
  void set_before_event_allocation_hook_for_testing(BeforeEventAllocationHook hook) noexcept {
    before_event_allocation_hook_ = hook;
  }
#endif

 private:
  InstrumentBook& book_;
  CommandAdmission admission_;
#if defined(ATLAS_ENABLE_TEST_ACCESS) && ATLAS_ENABLE_TEST_ACCESS
  BeforeEventAllocationHook before_event_allocation_hook_{};
#endif
};

}  // namespace atlaslob::core
