#include "reports.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>

#include "atlaslob/domain/types.hpp"
#include "snapshot_codec.hpp"

namespace atlaslob::persistence::detail {
namespace {

void append_string(std::string& output, std::string_view value) {
  output.push_back('"');
  for (const char byte : value) {
    switch (byte) {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default: {
        const auto byte_value = static_cast<unsigned char>(byte);
        if (byte_value < 0x20U) {
          static constexpr std::string_view digits{"0123456789abcdef"};
          output += "\\u00";
          output.push_back(digits[(byte_value >> 4U) & 0x0fU]);
          output.push_back(digits[byte_value & 0x0fU]);
        } else {
          output.push_back(byte);
        }
        break;
      }
    }
  }
  output.push_back('"');
}

template <typename Integer>
void append_decimal_string(std::string& output, Integer value) {
  char buffer[32]{};
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (error != std::errc{}) {
    std::terminate();
  }
  append_string(output, {buffer, static_cast<std::size_t>(end - buffer)});
}

void append_key(std::string& output, std::string_view key) {
  append_string(output, key);
  output.push_back(':');
}

void append_error(std::string& output, const LogError& error) {
  if (!error) {
    output += "null";
    return;
  }
  output.push_back('{');
  append_key(output, "category");
  append_string(output, to_string(error.category));
  output.push_back(',');
  append_key(output, "offset");
  append_decimal_string(output, error.byte_offset);
  output.push_back('}');
}

void append_error(std::string& output, const SnapshotError& error) {
  if (!error) {
    output += "null";
    return;
  }
  output.push_back('{');
  append_key(output, "category");
  append_string(output, to_string(error.category));
  output.push_back(',');
  append_key(output, "offset");
  append_decimal_string(output, error.byte_offset);
  output.push_back('}');
}

[[nodiscard]] std::string_view log_status(
    const LogInspectionReport& report, LogReportOperation operation,
    const std::optional<std::uint64_t>& output_bytes) noexcept {
  if (report.error.category == LogErrorCategory::io_failure) {
    return "io_error";
  }
  if (report.tail == LogTail::torn && report.warning()) {
    return "warning";
  }
  if (report.error) {
    return "invalid";
  }
  if (operation == LogReportOperation::repair_tail && report.clean() && !output_bytes.has_value()) {
    return "invalid";
  }
  return "ok";
}

[[nodiscard]] std::string_view replay_status(const ReplayReport& report) noexcept {
  if (report.error.category == LogErrorCategory::io_failure) {
    return "io_error";
  }
  if (report.error) {
    return "invalid";
  }
  if (report.divergence.has_value()) {
    return "diverged";
  }
  if (report.warning) {
    return "warning";
  }
  return "ok";
}

[[nodiscard]] std::string_view snapshot_status(const SnapshotInspectionReport& report) noexcept {
  if (report.error.category == SnapshotErrorCategory::io_failure) {
    return "io_error";
  }
  return report.error ? "invalid" : "ok";
}

[[nodiscard]] std::string_view snapshot_replay_status(
    const SnapshotRecoveryReport& report) noexcept {
  if (report.snapshot_error.category == SnapshotErrorCategory::io_failure ||
      report.replay.error.category == LogErrorCategory::io_failure) {
    return "io_error";
  }
  if (report.snapshot_error || report.replay.error) {
    return "invalid";
  }
  if (report.replay.divergence.has_value()) {
    return "diverged";
  }
  if (report.replay.warning || !report.skipped_snapshots.empty() ||
      report.recovery_source == RecoverySource::full_log) {
    return "warning";
  }
  return "ok";
}

void append_optional_sequence(std::string& output,
                              const std::optional<domain::Sequence>& sequence) {
  if (!sequence.has_value()) {
    output += "null";
  } else {
    append_decimal_string(output, sequence->value());
  }
}

void append_evidence(std::string& output, const ReplayEvidenceSummary& summary) {
  output.push_back('{');
  append_key(output, "outcome");
  if (summary.outcome.has_value()) {
    append_string(output, to_string(*summary.outcome));
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "rejection_reason");
  if (summary.rejection_reason.has_value()) {
    append_string(output, domain::to_string(*summary.rejection_reason));
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "event_count");
  if (summary.event_count.has_value()) {
    append_decimal_string(output, *summary.event_count);
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "event_digest");
  if (summary.event_digest.has_value()) {
    append_string(output, summary.event_digest->hex());
  } else {
    output += "null";
  }
  output.push_back('}');
}

void append_command(std::string& output, const domain::Command& command) {
  output.push_back('{');
  std::visit(
      [&output](const auto& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        append_key(output, "type");
        append_string(output, domain::to_string(domain::command_type(domain::Command{value})));
        output.push_back(',');
        append_key(output, "client_id");
        append_decimal_string(output, value.client_id.value());
        output.push_back(',');
        if constexpr (std::is_same_v<Value, domain::NewOrder>) {
          append_key(output, "order_id");
          append_decimal_string(output, value.order_id.value());
          output.push_back(',');
          append_key(output, "instrument_id");
          append_decimal_string(output, value.instrument_id.value());
          output.push_back(',');
          append_key(output, "side");
          append_string(output, domain::to_string(value.side));
          output.push_back(',');
          append_key(output, "side_raw");
          append_decimal_string(output, static_cast<std::uint32_t>(value.side));
          output.push_back(',');
          append_key(output, "order_type");
          append_string(output, domain::to_string(value.order_type));
          output.push_back(',');
          append_key(output, "order_type_raw");
          append_decimal_string(output, static_cast<std::uint32_t>(value.order_type));
          output.push_back(',');
          append_key(output, "time_in_force");
          append_string(output, domain::to_string(value.time_in_force));
          output.push_back(',');
          append_key(output, "time_in_force_raw");
          append_decimal_string(output, static_cast<std::uint32_t>(value.time_in_force));
          output.push_back(',');
          append_key(output, "limit_price");
          if (value.limit_price.has_value()) {
            append_decimal_string(output, value.limit_price->value());
          } else {
            output += "null";
          }
          output.push_back(',');
          append_key(output, "quantity");
          append_decimal_string(output, value.quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::CancelOrder>) {
          append_key(output, "order_id");
          append_decimal_string(output, value.order_id.value());
          output.push_back(',');
          append_key(output, "instrument_id");
          append_decimal_string(output, value.instrument_id.value());
        } else {
          static_assert(std::is_same_v<Value, domain::ReplaceOrder>);
          append_key(output, "old_order_id");
          append_decimal_string(output, value.old_order_id.value());
          output.push_back(',');
          append_key(output, "new_order_id");
          append_decimal_string(output, value.new_order_id.value());
          output.push_back(',');
          append_key(output, "instrument_id");
          append_decimal_string(output, value.instrument_id.value());
          output.push_back(',');
          append_key(output, "new_limit_price");
          append_decimal_string(output, value.new_limit_price.value());
          output.push_back(',');
          append_key(output, "new_quantity");
          append_decimal_string(output, value.new_quantity.value());
        }
      },
      command);
  output.push_back('}');
}

void append_optional_command(std::string& output, const std::optional<domain::Command>& command) {
  if (!command.has_value()) {
    output += "null";
    return;
  }
  append_command(output, *command);
}

void append_top_of_book_level(std::string& output,
                              const std::optional<domain::TopOfBookLevel>& level) {
  if (!level.has_value()) {
    output += "null";
    return;
  }
  output.push_back('{');
  append_key(output, "price");
  append_decimal_string(output, level->price.value());
  output.push_back(',');
  append_key(output, "aggregate_quantity");
  append_decimal_string(output, level->aggregate_quantity.value());
  output.push_back('}');
}

void append_event(std::string& output, const domain::Event& event) {
  const auto& header = domain::event_header(event);
  output.push_back('{');
  append_key(output, "type");
  append_string(output, domain::to_string(domain::event_type(event)));
  output.push_back(',');
  append_key(output, "command_sequence");
  append_decimal_string(output, header.command_sequence.value());
  output.push_back(',');
  append_key(output, "event_index");
  append_decimal_string(output, header.event_index);
  output.push_back(',');
  append_key(output, "instrument_id");
  append_decimal_string(output, header.instrument_id.value());

  std::visit(
      [&output](const auto& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, domain::AcceptedEvent>) {
          output.push_back(',');
          append_key(output, "command_type");
          append_string(output, domain::to_string(value.command_type));
        } else if constexpr (std::is_same_v<Value, domain::RejectedEvent>) {
          output.push_back(',');
          append_key(output, "command_type");
          append_string(output, domain::to_string(value.command_type));
          output.push_back(',');
          append_key(output, "reason");
          append_string(output, domain::to_string(value.reason));
          output.push_back(',');
          append_key(output, "order_id");
          if (value.order_id.has_value()) {
            append_decimal_string(output, value.order_id->value());
          } else {
            output += "null";
          }
        } else if constexpr (std::is_same_v<Value, domain::TradeEvent>) {
          output.push_back(',');
          append_key(output, "aggressor_order_id");
          append_decimal_string(output, value.aggressor_order_id.value());
          output.push_back(',');
          append_key(output, "resting_order_id");
          append_decimal_string(output, value.resting_order_id.value());
          output.push_back(',');
          append_key(output, "aggressor_client_id");
          append_decimal_string(output, value.aggressor_client_id.value());
          output.push_back(',');
          append_key(output, "resting_client_id");
          append_decimal_string(output, value.resting_client_id.value());
          output.push_back(',');
          append_key(output, "aggressor_side");
          append_string(output, domain::to_string(value.aggressor_side));
          output.push_back(',');
          append_key(output, "execution_price");
          append_decimal_string(output, value.execution_price.value());
          output.push_back(',');
          append_key(output, "execution_quantity");
          append_decimal_string(output, value.execution_quantity.value());
          output.push_back(',');
          append_key(output, "aggressor_remaining");
          append_decimal_string(output, value.aggressor_remaining.value());
          output.push_back(',');
          append_key(output, "resting_remaining");
          append_decimal_string(output, value.resting_remaining.value());
        } else if constexpr (std::is_same_v<Value, domain::RestedEvent>) {
          output.push_back(',');
          append_key(output, "order_id");
          append_decimal_string(output, value.order_id.value());
          output.push_back(',');
          append_key(output, "client_id");
          append_decimal_string(output, value.client_id.value());
          output.push_back(',');
          append_key(output, "side");
          append_string(output, domain::to_string(value.side));
          output.push_back(',');
          append_key(output, "price");
          append_decimal_string(output, value.price.value());
          output.push_back(',');
          append_key(output, "remaining_quantity");
          append_decimal_string(output, value.remaining_quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::CanceledEvent>) {
          output.push_back(',');
          append_key(output, "order_id");
          append_decimal_string(output, value.order_id.value());
          output.push_back(',');
          append_key(output, "canceled_quantity");
          append_decimal_string(output, value.canceled_quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::ReplacedEvent>) {
          output.push_back(',');
          append_key(output, "old_order_id");
          append_decimal_string(output, value.old_order_id.value());
          output.push_back(',');
          append_key(output, "new_order_id");
          append_decimal_string(output, value.new_order_id.value());
        } else if constexpr (std::is_same_v<Value, domain::DoneEvent>) {
          output.push_back(',');
          append_key(output, "order_id");
          append_decimal_string(output, value.order_id.value());
          output.push_back(',');
          append_key(output, "reason");
          append_string(output, domain::to_string(value.reason));
          output.push_back(',');
          append_key(output, "remaining_quantity");
          append_decimal_string(output, value.remaining_quantity.value());
        } else {
          static_assert(std::is_same_v<Value, domain::BookChangedEvent>);
          output.push_back(',');
          append_key(output, "best_bid");
          append_top_of_book_level(output, value.best_bid);
          output.push_back(',');
          append_key(output, "best_ask");
          append_top_of_book_level(output, value.best_ask);
        }
      },
      event);
  output.push_back('}');
}

void append_events(std::string& output, const std::vector<domain::Event>& events) {
  output.push_back('[');
  for (std::size_t index = 0U; index < events.size(); ++index) {
    if (index != 0U) {
      output.push_back(',');
    }
    append_event(output, events[index]);
  }
  output.push_back(']');
}

void append_text_command(std::string& output, const std::optional<domain::Command>& command) {
  if (!command.has_value()) {
    output += "command=null\n";
    return;
  }

  output += "command type=";
  output += domain::to_string(domain::command_type(*command));
  std::visit(
      [&output](const auto& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        output += " client_id=" + std::to_string(value.client_id.value());
        if constexpr (std::is_same_v<Value, domain::NewOrder>) {
          output += " order_id=" + std::to_string(value.order_id.value());
          output += " instrument_id=" + std::to_string(value.instrument_id.value());
          output += " side=";
          output += domain::to_string(value.side);
          output += " side_raw=" + std::to_string(static_cast<std::uint32_t>(value.side));
          output += " order_type=";
          output += domain::to_string(value.order_type);
          output +=
              " order_type_raw=" + std::to_string(static_cast<std::uint32_t>(value.order_type));
          output += " time_in_force=";
          output += domain::to_string(value.time_in_force);
          output += " time_in_force_raw=" +
                    std::to_string(static_cast<std::uint32_t>(value.time_in_force));
          output += " limit_price=";
          output += value.limit_price.has_value() ? std::to_string(value.limit_price->value())
                                                  : std::string{"null"};
          output += " quantity=" + std::to_string(value.quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::CancelOrder>) {
          output += " order_id=" + std::to_string(value.order_id.value());
          output += " instrument_id=" + std::to_string(value.instrument_id.value());
        } else {
          static_assert(std::is_same_v<Value, domain::ReplaceOrder>);
          output += " old_order_id=" + std::to_string(value.old_order_id.value());
          output += " new_order_id=" + std::to_string(value.new_order_id.value());
          output += " instrument_id=" + std::to_string(value.instrument_id.value());
          output += " new_limit_price=" + std::to_string(value.new_limit_price.value());
          output += " new_quantity=" + std::to_string(value.new_quantity.value());
        }
      },
      *command);
  output.push_back('\n');
}

void append_text_top_of_book_level(std::string& output, std::string_view name,
                                   const std::optional<domain::TopOfBookLevel>& level) {
  output.push_back(' ');
  output += name;
  if (!level.has_value()) {
    output += "=null";
    return;
  }
  output += ".price=" + std::to_string(level->price.value());
  output.push_back(' ');
  output += name;
  output += ".aggregate_quantity=" + std::to_string(level->aggregate_quantity.value());
}

void append_text_event(std::string& output, const domain::Event& event) {
  const auto& header = domain::event_header(event);
  output += "actual_event type=";
  output += domain::to_string(domain::event_type(event));
  output += " command_sequence=" + std::to_string(header.command_sequence.value());
  output += " event_index=" + std::to_string(header.event_index);
  output += " instrument_id=" + std::to_string(header.instrument_id.value());

  std::visit(
      [&output](const auto& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, domain::AcceptedEvent>) {
          output += " command_type=";
          output += domain::to_string(value.command_type);
        } else if constexpr (std::is_same_v<Value, domain::RejectedEvent>) {
          output += " command_type=";
          output += domain::to_string(value.command_type);
          output += " reason=";
          output += domain::to_string(value.reason);
          output += " order_id=";
          output += value.order_id.has_value() ? std::to_string(value.order_id->value())
                                               : std::string{"null"};
        } else if constexpr (std::is_same_v<Value, domain::TradeEvent>) {
          output += " aggressor_order_id=" + std::to_string(value.aggressor_order_id.value());
          output += " resting_order_id=" + std::to_string(value.resting_order_id.value());
          output += " aggressor_client_id=" + std::to_string(value.aggressor_client_id.value());
          output += " resting_client_id=" + std::to_string(value.resting_client_id.value());
          output += " aggressor_side=";
          output += domain::to_string(value.aggressor_side);
          output += " execution_price=" + std::to_string(value.execution_price.value());
          output += " execution_quantity=" + std::to_string(value.execution_quantity.value());
          output += " aggressor_remaining=" + std::to_string(value.aggressor_remaining.value());
          output += " resting_remaining=" + std::to_string(value.resting_remaining.value());
        } else if constexpr (std::is_same_v<Value, domain::RestedEvent>) {
          output += " order_id=" + std::to_string(value.order_id.value());
          output += " client_id=" + std::to_string(value.client_id.value());
          output += " side=";
          output += domain::to_string(value.side);
          output += " price=" + std::to_string(value.price.value());
          output += " remaining_quantity=" + std::to_string(value.remaining_quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::CanceledEvent>) {
          output += " order_id=" + std::to_string(value.order_id.value());
          output += " canceled_quantity=" + std::to_string(value.canceled_quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::ReplacedEvent>) {
          output += " old_order_id=" + std::to_string(value.old_order_id.value());
          output += " new_order_id=" + std::to_string(value.new_order_id.value());
        } else if constexpr (std::is_same_v<Value, domain::DoneEvent>) {
          output += " order_id=" + std::to_string(value.order_id.value());
          output += " reason=";
          output += domain::to_string(value.reason);
          output += " remaining_quantity=" + std::to_string(value.remaining_quantity.value());
        } else {
          static_assert(std::is_same_v<Value, domain::BookChangedEvent>);
          append_text_top_of_book_level(output, "best_bid", value.best_bid);
          append_text_top_of_book_level(output, "best_ask", value.best_ask);
        }
      },
      event);
  output.push_back('\n');
}

void append_record(std::string& output, const RecordSummary& record) {
  output.push_back('{');
  append_key(output, "offset");
  append_decimal_string(output, record.offset);
  output.push_back(',');
  append_key(output, "total_length");
  append_decimal_string(output, record.total_length);
  output.push_back(',');
  append_key(output, "payload_length");
  append_decimal_string(output, record.payload_length);
  output.push_back(',');
  append_key(output, "record_version");
  append_decimal_string(output, record.record_version);
  output.push_back(',');
  append_key(output, "sequence");
  append_decimal_string(output, record.sequence.value());
  output.push_back(',');
  append_key(output, "command_type");
  append_string(output, domain::to_string(record.command_type));
  output.push_back(',');
  append_key(output, "outcome");
  append_string(output, to_string(record.outcome));
  output.push_back(',');
  append_key(output, "rejection_reason");
  append_string(output, domain::to_string(record.rejection_reason));
  output.push_back(',');
  append_key(output, "event_count");
  append_decimal_string(output, record.event_count);
  output.push_back(',');
  append_key(output, "event_digest");
  append_string(output, record.event_digest.hex());
  output.push_back('}');
}

void append_text_evidence(std::string& output, std::string_view prefix,
                          const ReplayEvidenceSummary& summary) {
  output.push_back(' ');
  output += prefix;
  output += ".outcome=";
  output += summary.outcome.has_value() ? std::string{to_string(*summary.outcome)} : "null";
  output.push_back(' ');
  output += prefix;
  output += ".rejection_reason=";
  output += summary.rejection_reason.has_value()
                ? std::string{domain::to_string(*summary.rejection_reason)}
                : "null";
  output.push_back(' ');
  output += prefix;
  output += ".event_count=";
  output += summary.event_count.has_value() ? std::to_string(*summary.event_count) : "null";
  output.push_back(' ');
  output += prefix;
  output += ".event_digest=";
  output += summary.event_digest.has_value() ? summary.event_digest->hex() : "null";
}

}  // namespace

std::string render_log_report_json(const LogInspectionReport& report, LogReportOperation operation,
                                   std::optional<std::uint64_t> output_bytes,
                                   bool unpublished_artifact_present) {
  std::string output;
  output.reserve(1024U);
  output.push_back('{');
  append_key(output, "schema");
  append_string(output, log_report_schema);
  output.push_back(',');
  append_key(output, "operation");
  append_string(output, to_string(operation));
  output.push_back(',');
  append_key(output, "status");
  append_string(output, log_status(report, operation, output_bytes));
  output.push_back(',');
  append_key(output, "format_version");
  if (report.header.has_value()) {
    append_decimal_string(output, report.header->format_version);
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "semantics_version");
  if (report.header.has_value()) {
    append_decimal_string(output, report.header->semantics_version);
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "log_id");
  if (report.header.has_value()) {
    append_string(output, report.header->log_id.hex());
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "first_sequence");
  if (report.header.has_value()) {
    append_decimal_string(output, report.header->first_sequence.value());
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "header_length");
  if (report.header.has_value()) {
    append_decimal_string(output, report.header_length);
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "catalog_length");
  if (report.header.has_value()) {
    append_decimal_string(output, report.catalog_length);
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "catalog_count");
  if (report.header.has_value()) {
    append_decimal_string(output, report.header->catalog.size());
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "configuration_digest");
  if (report.configuration_digest.has_value()) {
    append_string(output, report.configuration_digest->hex());
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "records_scanned");
  append_decimal_string(output, report.records_scanned);
  output.push_back(',');
  append_key(output, "last_sequence");
  append_optional_sequence(output, report.last_sequence);
  output.push_back(',');
  append_key(output, "input_bytes");
  append_decimal_string(output, report.input_bytes);
  output.push_back(',');
  append_key(output, "valid_prefix_bytes");
  append_decimal_string(output, report.valid_prefix_bytes);
  output.push_back(',');
  append_key(output, "output_bytes");
  if (output_bytes.has_value()) {
    append_decimal_string(output, *output_bytes);
  } else {
    output += "null";
  }
  if (unpublished_artifact_present) {
    output.push_back(',');
    append_key(output, "unpublished_artifact_present");
    output += "true";
  }
  output.push_back(',');
  append_key(output, "tail");
  append_string(output, to_string(report.tail));
  output.push_back(',');
  append_key(output, "warning");
  const bool torn_warning = report.tail == LogTail::torn && report.warning();
  if (torn_warning) {
    append_error(output, report.error);
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "error");
  if (torn_warning) {
    output += "null";
  } else {
    append_error(output, report.error);
  }
  output.push_back(',');
  append_key(output, "records");
  if (!report.records.has_value()) {
    output += "null";
  } else {
    output.push_back('[');
    for (std::size_t index = 0U; index < report.records->size(); ++index) {
      if (index != 0U) {
        output.push_back(',');
      }
      append_record(output, (*report.records)[index]);
    }
    output.push_back(']');
  }
  output += "}\n";
  return output;
}

std::string render_log_report_text(const LogInspectionReport& report, LogReportOperation operation,
                                   std::optional<std::uint64_t> output_bytes,
                                   bool unpublished_artifact_present) {
  std::string output;
  output += "schema=";
  output += log_report_schema;
  output += " operation=";
  output += to_string(operation);
  output += " status=";
  output += log_status(report, operation, output_bytes);
  output += " format_version=";
  output += report.header.has_value() ? std::to_string(report.header->format_version) : "null";
  output += " semantics_version=";
  output += report.header.has_value() ? std::to_string(report.header->semantics_version) : "null";
  output += " log_id=";
  output += report.header.has_value() ? report.header->log_id.hex() : "null";
  output += " first_sequence=";
  output +=
      report.header.has_value() ? std::to_string(report.header->first_sequence.value()) : "null";
  output += " header_length=";
  output += report.header.has_value() ? std::to_string(report.header_length) : "null";
  output += " catalog_length=";
  output += report.header.has_value() ? std::to_string(report.catalog_length) : "null";
  output += " catalog_count=";
  output += report.header.has_value() ? std::to_string(report.header->catalog.size()) : "null";
  output += " configuration_digest=";
  output += report.configuration_digest.has_value() ? report.configuration_digest->hex() : "null";
  output += " records_scanned=";
  output += std::to_string(report.records_scanned);
  output += " last_sequence=";
  output +=
      report.last_sequence.has_value() ? std::to_string(report.last_sequence->value()) : "null";
  output += " input_bytes=";
  output += std::to_string(report.input_bytes);
  output += " valid_prefix_bytes=";
  output += std::to_string(report.valid_prefix_bytes);
  output += " output_bytes=";
  output += output_bytes.has_value() ? std::to_string(*output_bytes) : "null";
  if (unpublished_artifact_present) {
    output += " unpublished_artifact_present=true";
  }
  output += " tail=";
  output += to_string(report.tail);
  const bool torn_warning = report.tail == LogTail::torn && report.warning();
  output += " warning=";
  if (torn_warning) {
    output += to_string(report.error.category);
    output.push_back('@');
    output += std::to_string(report.error.byte_offset);
  } else {
    output += "null";
  }
  output += " error=";
  if (report.error && !torn_warning) {
    output += to_string(report.error.category);
    output.push_back('@');
    output += std::to_string(report.error.byte_offset);
  } else {
    output += "null";
  }
  output += " records=";
  output += report.records.has_value() ? std::to_string(report.records->size()) : "null";
  output.push_back('\n');
  if (report.records.has_value()) {
    for (const auto& record : *report.records) {
      output += "record offset=" + std::to_string(record.offset);
      output += " total_length=" + std::to_string(record.total_length);
      output += " payload_length=" + std::to_string(record.payload_length);
      output += " record_version=" + std::to_string(record.record_version);
      output += " sequence=" + std::to_string(record.sequence.value());
      output += " command_type=";
      output += domain::to_string(record.command_type);
      output += " outcome=";
      output += to_string(record.outcome);
      output += " rejection_reason=";
      output += domain::to_string(record.rejection_reason);
      output += " event_count=" + std::to_string(record.event_count);
      output += " event_digest=" + record.event_digest.hex();
      output.push_back('\n');
    }
  }
  return output;
}

std::string render_replay_report_json(const ReplayReport& report) {
  std::string output;
  output.reserve(1024U);
  output.push_back('{');
  append_key(output, "schema");
  append_string(output, replay_report_schema);
  output.push_back(',');
  append_key(output, "status");
  append_string(output, replay_status(report));
  output.push_back(',');
  append_key(output, "mode");
  append_string(output, to_string(report.mode));
  output.push_back(',');
  append_key(output, "tail_policy");
  append_string(output, to_string(report.tail_policy));
  output.push_back(',');
  append_key(output, "semantics_version");
  if (report.header.has_value()) {
    append_decimal_string(output, report.header->semantics_version);
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "log_id");
  if (report.header.has_value()) {
    append_string(output, report.header->log_id.hex());
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "first_sequence");
  if (report.header.has_value()) {
    append_decimal_string(output, report.header->first_sequence.value());
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "last_sequence");
  append_optional_sequence(output, report.last_sequence);
  output.push_back(',');
  append_key(output, "records_available");
  append_decimal_string(output, report.records_scanned);
  output.push_back(',');
  append_key(output, "records_replayed");
  append_decimal_string(output, report.records_replayed);
  output.push_back(',');
  append_key(output, "committed");
  append_decimal_string(output, report.committed);
  output.push_back(',');
  append_key(output, "rejected");
  append_decimal_string(output, report.rejected);
  output.push_back(',');
  append_key(output, "final_state_digest");
  if (report.final_state_digest.has_value()) {
    append_string(output, report.final_state_digest->hex());
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "tail");
  append_string(output, to_string(report.tail));
  output.push_back(',');
  append_key(output, "warning");
  append_error(output, report.warning);
  output.push_back(',');
  append_key(output, "error");
  append_error(output, report.error);
  output.push_back(',');
  append_key(output, "divergence");
  if (!report.divergence.has_value()) {
    output += "null";
  } else {
    const auto& difference = *report.divergence;
    output.push_back('{');
    append_key(output, "sequence");
    append_decimal_string(output, difference.sequence.value());
    output.push_back(',');
    append_key(output, "record_offset");
    append_decimal_string(output, difference.record_offset);
    output.push_back(',');
    append_key(output, "category");
    append_string(output, to_string(difference.category));
    output.push_back(',');
    append_key(output, "command");
    append_optional_command(output, difference.command);
    output.push_back(',');
    append_key(output, "expected");
    append_evidence(output, difference.expected);
    output.push_back(',');
    append_key(output, "actual");
    append_evidence(output, difference.actual);
    output.push_back(',');
    append_key(output, "actual_engine_error");
    append_string(output, atlaslob::to_string(difference.actual_engine_error));
    output.push_back(',');
    append_key(output, "actual_events");
    append_events(output, difference.actual_events);
    output.push_back('}');
  }
  output += "}\n";
  return output;
}

std::string render_replay_report_text(const ReplayReport& report) {
  std::string output;
  output += "schema=";
  output += replay_report_schema;
  output += " status=";
  output += replay_status(report);
  output += " mode=";
  output += to_string(report.mode);
  output += " tail_policy=";
  output += to_string(report.tail_policy);
  output += " semantics_version=";
  output += report.header.has_value() ? std::to_string(report.header->semantics_version) : "null";
  output += " log_id=";
  output += report.header.has_value() ? report.header->log_id.hex() : "null";
  output += " first_sequence=";
  output +=
      report.header.has_value() ? std::to_string(report.header->first_sequence.value()) : "null";
  output += " last_sequence=";
  output +=
      report.last_sequence.has_value() ? std::to_string(report.last_sequence->value()) : "null";
  output += " records_available=";
  output += std::to_string(report.records_scanned);
  output += " records_replayed=";
  output += std::to_string(report.records_replayed);
  output += " committed=";
  output += std::to_string(report.committed);
  output += " rejected=";
  output += std::to_string(report.rejected);
  output += " final_state_digest=";
  output += report.final_state_digest.has_value() ? report.final_state_digest->hex() : "null";
  output += " tail=";
  output += to_string(report.tail);
  output += " warning=";
  if (report.warning) {
    output += to_string(report.warning.category);
    output.push_back('@');
    output += std::to_string(report.warning.byte_offset);
  } else {
    output += "null";
  }
  output += " error=";
  if (report.error) {
    output += to_string(report.error.category);
    output.push_back('@');
    output += std::to_string(report.error.byte_offset);
  } else {
    output += "null";
  }
  output += " divergence=";
  output +=
      report.divergence.has_value() ? std::string{to_string(report.divergence->category)} : "null";
  output.push_back('\n');
  if (report.divergence.has_value()) {
    output += "divergence sequence=";
    output += std::to_string(report.divergence->sequence.value());
    output += " record_offset=";
    output += std::to_string(report.divergence->record_offset);
    output += " category=";
    output += to_string(report.divergence->category);
    append_text_evidence(output, "expected", report.divergence->expected);
    append_text_evidence(output, "actual", report.divergence->actual);
    output += " actual_engine_error=";
    output += atlaslob::to_string(report.divergence->actual_engine_error);
    output += " actual_events=";
    output += std::to_string(report.divergence->actual_events.size());
    output.push_back('\n');
    append_text_command(output, report.divergence->command);
    for (const auto& event : report.divergence->actual_events) {
      append_text_event(output, event);
    }
  }
  return output;
}

std::string render_snapshot_report_json(const SnapshotInspectionReport& report) {
  std::string output;
  output.reserve(768U);
  output.push_back('{');
  append_key(output, "schema");
  append_string(output, snapshot_report_schema);
  output.push_back(',');
  append_key(output, "operation");
  append_string(output, "inspect_snapshot");
  output.push_back(',');
  append_key(output, "status");
  append_string(output, snapshot_status(report));

  const auto append_snapshot_number = [&output, &report](std::string_view key, auto accessor) {
    output.push_back(',');
    append_key(output, key);
    if (!report.snapshot.has_value()) {
      output += "null";
    } else {
      append_decimal_string(output, accessor(*report.snapshot));
    }
  };
  append_snapshot_number("format_version",
                         [](const SnapshotFile& value) { return value.format_version; });
  append_snapshot_number("semantics_version",
                         [](const SnapshotFile& value) { return value.semantics_version; });
  output.push_back(',');
  append_key(output, "log_id");
  if (report.snapshot.has_value()) {
    append_string(output, report.snapshot->log_id.hex());
  } else {
    output += "null";
  }
  append_snapshot_number("covered_sequence",
                         [](const SnapshotFile& value) { return value.covered_sequence.value(); });
  append_snapshot_number("covered_log_offset",
                         [](const SnapshotFile& value) { return value.covered_log_byte_offset; });
  append_snapshot_number("declared_snapshot_length",
                         [&report](const SnapshotFile&) { return report.input_bytes; });
  append_snapshot_number("header_length", [](const SnapshotFile& value) {
    return static_cast<std::uint64_t>(snapshot_fixed_bytes) +
           static_cast<std::uint64_t>(snapshot_catalog_entry_bytes) *
               static_cast<std::uint64_t>(value.catalog.size());
  });
  append_snapshot_number("catalog_length", [](const SnapshotFile& value) {
    return static_cast<std::uint64_t>(snapshot_catalog_entry_bytes) *
           static_cast<std::uint64_t>(value.catalog.size());
  });
  append_snapshot_number("instruments_length", [&report](const SnapshotFile& value) {
    const auto header_length = static_cast<std::uint64_t>(snapshot_fixed_bytes) +
                               static_cast<std::uint64_t>(snapshot_catalog_entry_bytes) *
                                   static_cast<std::uint64_t>(value.catalog.size());
    return report.input_bytes - header_length - sizeof(std::uint32_t);
  });
  append_snapshot_number("catalog_count",
                         [](const SnapshotFile& value) { return value.catalog.size(); });
  append_snapshot_number("instrument_count",
                         [](const SnapshotFile& value) { return value.instruments.size(); });
  append_snapshot_number("active_order_count",
                         [](const SnapshotFile& value) { return value.active_order_count; });
  output.push_back(',');
  append_key(output, "sequence_exhausted");
  if (report.snapshot.has_value()) {
    output += report.snapshot->sequence_exhausted ? "true" : "false";
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "configuration_digest");
  if (report.snapshot.has_value()) {
    append_string(output, report.snapshot->configuration_digest.hex());
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "state_digest");
  if (report.snapshot.has_value()) {
    append_string(output, report.snapshot->state_digest.hex());
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "input_bytes");
  append_decimal_string(output, report.input_bytes);
  output.push_back(',');
  append_key(output, "error");
  append_error(output, report.error);
  output += "}\n";
  return output;
}

std::string render_snapshot_report_text(const SnapshotInspectionReport& report) {
  std::string output;
  output += "schema=";
  output += snapshot_report_schema;
  output += " operation=inspect_snapshot status=";
  output += snapshot_status(report);
  output += " format_version=";
  output += report.snapshot.has_value() ? std::to_string(report.snapshot->format_version) : "null";
  output += " semantics_version=";
  output +=
      report.snapshot.has_value() ? std::to_string(report.snapshot->semantics_version) : "null";
  output += " log_id=";
  output += report.snapshot.has_value() ? report.snapshot->log_id.hex() : "null";
  output += " covered_sequence=";
  output += report.snapshot.has_value() ? std::to_string(report.snapshot->covered_sequence.value())
                                        : "null";
  output += " covered_log_offset=";
  output += report.snapshot.has_value() ? std::to_string(report.snapshot->covered_log_byte_offset)
                                        : "null";
  output += " active_order_count=";
  output +=
      report.snapshot.has_value() ? std::to_string(report.snapshot->active_order_count) : "null";
  output += " sequence_exhausted=";
  output += report.snapshot.has_value() ? (report.snapshot->sequence_exhausted ? "true" : "false")
                                        : "null";
  output += " configuration_digest=";
  output += report.snapshot.has_value() ? report.snapshot->configuration_digest.hex() : "null";
  output += " state_digest=";
  output += report.snapshot.has_value() ? report.snapshot->state_digest.hex() : "null";
  output += " input_bytes=" + std::to_string(report.input_bytes);
  output += " error=";
  if (report.error) {
    output += to_string(report.error.category);
    output.push_back('@');
    output += std::to_string(report.error.byte_offset);
  } else {
    output += "null";
  }
  output.push_back('\n');
  return output;
}

std::string render_snapshot_replay_report_json(const SnapshotRecoveryReport& report) {
  std::string output;
  output.reserve(1536U);
  output.push_back('{');
  append_key(output, "schema");
  append_string(output, snapshot_replay_report_schema);
  output.push_back(',');
  append_key(output, "status");
  append_string(output, snapshot_replay_status(report));
  output.push_back(',');
  append_key(output, "mode");
  append_string(output, to_string(report.replay.mode));
  output.push_back(',');
  append_key(output, "tail_policy");
  append_string(output, to_string(report.replay.tail_policy));
  output.push_back(',');
  append_key(output, "semantics_version");
  if (report.replay.header.has_value()) {
    append_decimal_string(output, report.replay.header->semantics_version);
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "log_id");
  if (report.replay.header.has_value()) {
    append_string(output, report.replay.header->log_id.hex());
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "first_sequence");
  if (report.replay.header.has_value()) {
    append_decimal_string(output, report.replay.header->first_sequence.value());
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "last_sequence");
  append_optional_sequence(output, report.replay.last_sequence);
  output.push_back(',');
  append_key(output, "records_available");
  append_decimal_string(output, report.replay.records_scanned);
  output.push_back(',');
  append_key(output, "records_covered_by_snapshot");
  append_decimal_string(
      output, report.covered_sequence.has_value() ? report.covered_sequence->value() : 0U);
  output.push_back(',');
  append_key(output, "records_replayed");
  append_decimal_string(output, report.replay.records_replayed);
  output.push_back(',');
  append_key(output, "committed");
  append_decimal_string(output, report.replay.committed);
  output.push_back(',');
  append_key(output, "rejected");
  append_decimal_string(output, report.replay.rejected);
  output.push_back(',');
  append_key(output, "final_state_digest");
  if (report.replay.final_state_digest.has_value()) {
    append_string(output, report.replay.final_state_digest->hex());
  } else {
    output += "null";
  }
  output.push_back(',');
  append_key(output, "tail");
  append_string(output, to_string(report.replay.tail));
  output.push_back(',');
  append_key(output, "recovery_source");
  append_string(output, to_string(report.recovery_source));
  output.push_back(',');
  append_key(output, "snapshot");
  if (!report.covered_sequence.has_value() || !report.covered_log_byte_offset.has_value() ||
      !report.snapshot_state_digest.has_value()) {
    output += "null";
  } else {
    output.push_back('{');
    append_key(output, "covered_sequence");
    append_decimal_string(output, report.covered_sequence->value());
    output.push_back(',');
    append_key(output, "covered_log_offset");
    append_decimal_string(output, *report.covered_log_byte_offset);
    output.push_back(',');
    append_key(output, "state_digest");
    append_string(output, report.snapshot_state_digest->hex());
    output.push_back('}');
  }
  output.push_back(',');
  append_key(output, "skipped_snapshots");
  output.push_back('[');
  for (std::size_t index = 0U; index < report.skipped_snapshots.size(); ++index) {
    if (index != 0U) {
      output.push_back(',');
    }
    const auto& skipped = report.skipped_snapshots[index];
    output.push_back('{');
    append_key(output, "candidate_sequence");
    if (skipped.filename_sequence.has_value()) {
      append_decimal_string(output, skipped.filename_sequence->value());
    } else {
      output += "null";
    }
    output.push_back(',');
    append_key(output, "category");
    append_string(output, to_string(skipped.error.category));
    output.push_back(',');
    append_key(output, "offset");
    append_decimal_string(output, skipped.error.byte_offset);
    output.push_back('}');
  }
  output.push_back(']');
  output.push_back(',');
  append_key(output, "warnings");
  output.push_back('[');
  bool has_warning{};
  const auto append_warning = [&output, &has_warning](std::string_view category,
                                                      std::optional<std::uint64_t> offset) {
    if (has_warning) {
      output.push_back(',');
    }
    has_warning = true;
    output.push_back('{');
    append_key(output, "category");
    append_string(output, category);
    output.push_back(',');
    append_key(output, "offset");
    if (offset.has_value()) {
      append_decimal_string(output, *offset);
    } else {
      output += "null";
    }
    output.push_back('}');
  };
  if (report.replay.warning) {
    append_warning("truncated_final_record", report.replay.warning.byte_offset);
  }
  if (!report.skipped_snapshots.empty()) {
    append_warning("snapshot_candidates_skipped", std::nullopt);
  }
  if (report.recovery_source == RecoverySource::full_log) {
    append_warning("snapshot_fallback_full_log", std::nullopt);
  }
  output.push_back(']');
  output.push_back(',');
  append_key(output, "error");
  if (report.snapshot_error) {
    append_error(output, report.snapshot_error);
  } else {
    append_error(output, report.replay.error);
  }
  output.push_back(',');
  append_key(output, "divergence");
  if (!report.replay.divergence.has_value()) {
    output += "null";
  } else {
    const auto& difference = *report.replay.divergence;
    output.push_back('{');
    append_key(output, "sequence");
    append_decimal_string(output, difference.sequence.value());
    output.push_back(',');
    append_key(output, "record_offset");
    append_decimal_string(output, difference.record_offset);
    output.push_back(',');
    append_key(output, "category");
    append_string(output, to_string(difference.category));
    output.push_back(',');
    append_key(output, "command");
    append_optional_command(output, difference.command);
    output.push_back(',');
    append_key(output, "expected");
    append_evidence(output, difference.expected);
    output.push_back(',');
    append_key(output, "actual");
    append_evidence(output, difference.actual);
    output.push_back(',');
    append_key(output, "actual_engine_error");
    append_string(output, atlaslob::to_string(difference.actual_engine_error));
    output.push_back(',');
    append_key(output, "actual_events");
    append_events(output, difference.actual_events);
    output.push_back('}');
  }
  output += "}\n";
  return output;
}

std::string render_snapshot_replay_report_text(const SnapshotRecoveryReport& report) {
  std::string output;
  output += "schema=";
  output += snapshot_replay_report_schema;
  output += " status=";
  output += snapshot_replay_status(report);
  output += " mode=";
  output += to_string(report.replay.mode);
  output += " tail_policy=";
  output += to_string(report.replay.tail_policy);
  output += " records_available=" + std::to_string(report.replay.records_scanned);
  output += " records_covered_by_snapshot=";
  output +=
      report.covered_sequence.has_value() ? std::to_string(report.covered_sequence->value()) : "0";
  output += " records_replayed=" + std::to_string(report.replay.records_replayed);
  output += " committed=" + std::to_string(report.replay.committed);
  output += " rejected=" + std::to_string(report.replay.rejected);
  output += " recovery_source=";
  output += to_string(report.recovery_source);
  output += " skipped_snapshots=" + std::to_string(report.skipped_snapshots.size());
  output += " final_state_digest=";
  output += report.replay.final_state_digest.has_value() ? report.replay.final_state_digest->hex()
                                                         : "null";
  output += " error=";
  if (report.snapshot_error) {
    output += to_string(report.snapshot_error.category);
    output.push_back('@');
    output += std::to_string(report.snapshot_error.byte_offset);
  } else if (report.replay.error) {
    output += to_string(report.replay.error.category);
    output.push_back('@');
    output += std::to_string(report.replay.error.byte_offset);
  } else {
    output += "null";
  }
  output += " divergence=";
  output += report.replay.divergence.has_value()
                ? std::string{to_string(report.replay.divergence->category)}
                : "null";
  output.push_back('\n');
  for (const auto& skipped : report.skipped_snapshots) {
    output += "skipped_snapshot candidate_sequence=";
    output += skipped.filename_sequence.has_value()
                  ? std::to_string(skipped.filename_sequence->value())
                  : "null";
    output += " category=";
    output += to_string(skipped.error.category);
    output += " offset=" + std::to_string(skipped.error.byte_offset);
    output.push_back('\n');
  }
  if (report.replay.divergence.has_value()) {
    const auto& difference = *report.replay.divergence;
    output += "divergence sequence=" + std::to_string(difference.sequence.value());
    output += " record_offset=" + std::to_string(difference.record_offset);
    output += " category=";
    output += to_string(difference.category);
    append_text_evidence(output, "expected", difference.expected);
    append_text_evidence(output, "actual", difference.actual);
    output += " actual_engine_error=";
    output += atlaslob::to_string(difference.actual_engine_error);
    output += " actual_events=" + std::to_string(difference.actual_events.size());
    output.push_back('\n');
    append_text_command(output, difference.command);
    for (const auto& event : difference.actual_events) {
      append_text_event(output, event);
    }
  }
  return output;
}

}  // namespace atlaslob::persistence::detail
