#include "command_admission.hpp"

#include <stdexcept>
#include <variant>

#include "state_validation.hpp"

namespace atlaslob::core {
namespace {

[[nodiscard]] CommandAdmissionError admission_error(StateValidationError error) noexcept {
  switch (error) {
    case StateValidationError::none:
      return CommandAdmissionError::none;
    case StateValidationError::invalid_policy:
      return CommandAdmissionError::invalid_policy;
    case StateValidationError::book_invariant_violation:
      return CommandAdmissionError::book_invariant_violation;
  }
  return CommandAdmissionError::book_invariant_violation;
}

[[nodiscard]] CommandAdmissionResult complete_admission(
    domain::Sequence sequence, domain::CommandType command_type,
    const StateValidationResult& validation) noexcept {
  return {
      .command_sequence = sequence,
      .command_type = command_type,
      .reject_reason = validation.reason,
      .relevant_order_id = validation.relevant_order_id,
      .internal_error = admission_error(validation.internal_error),
  };
}

}  // namespace

CommandAdmission::CommandAdmission(const InstrumentBook& book, ExecutionPolicy policy)
    : book_{book}, policy_{policy} {
  if (!policy_.valid()) {
    throw std::invalid_argument{"CommandAdmission requires a valid execution policy"};
  }
}

CommandAdmission::CommandAdmission(const InstrumentBook& book, ExecutionPolicy policy,
                                   domain::Sequence first_sequence)
    : book_{book}, policy_{policy}, sequencer_{first_sequence} {
  if (!policy_.valid()) {
    throw std::invalid_argument{"CommandAdmission requires a valid execution policy"};
  }
}

CommandAdmissionResult CommandAdmission::admit(const domain::NewOrder& order) noexcept {
  // Issue before either pure or state validation so every domain submission has an audit position.
  const auto issued = sequencer_.issue();
  if (!issued) {
    return {
        .command_sequence = {},
        .command_type = domain::CommandType::new_order,
        .reject_reason = domain::RejectReason::none,
        .relevant_order_id = std::nullopt,
        .internal_error = CommandAdmissionError::sequence_exhausted,
    };
  }
  const auto validation = validate_state(order, book_, policy_);
  return complete_admission(issued.sequence, domain::CommandType::new_order, validation);
}

CommandAdmissionResult CommandAdmission::admit(const domain::CancelOrder& order) noexcept {
  const auto issued = sequencer_.issue();
  if (!issued) {
    return {
        .command_sequence = {},
        .command_type = domain::CommandType::cancel,
        .reject_reason = domain::RejectReason::none,
        .relevant_order_id = std::nullopt,
        .internal_error = CommandAdmissionError::sequence_exhausted,
    };
  }
  const auto validation = validate_state(order, book_, policy_);
  return complete_admission(issued.sequence, domain::CommandType::cancel, validation);
}

CommandAdmissionResult CommandAdmission::admit(const domain::ReplaceOrder& order) noexcept {
  const auto issued = sequencer_.issue();
  if (!issued) {
    return {
        .command_sequence = {},
        .command_type = domain::CommandType::replace,
        .reject_reason = domain::RejectReason::none,
        .relevant_order_id = std::nullopt,
        .internal_error = CommandAdmissionError::sequence_exhausted,
    };
  }
  const auto validation = validate_state(order, book_, policy_);
  return complete_admission(issued.sequence, domain::CommandType::replace, validation);
}

CommandAdmissionResult CommandAdmission::admit(const domain::Command& command) noexcept {
  return std::visit([this](const auto& value) noexcept { return admit(value); }, command);
}

}  // namespace atlaslob::core
