#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "atlaslob/persistence/inspection.hpp"
#include "atlaslob/persistence/replay.hpp"

namespace atlaslob::persistence::detail {

enum class LogReportOperation : std::uint8_t {
  inspect_log = 1,
  repair_tail = 2,
};

[[nodiscard]] constexpr std::string_view to_string(LogReportOperation operation) noexcept {
  switch (operation) {
    case LogReportOperation::inspect_log:
      return "inspect_log";
    case LogReportOperation::repair_tail:
      return "repair_tail";
  }
  return "unknown";
}

[[nodiscard]] std::string render_log_report_json(
    const LogInspectionReport& report, LogReportOperation operation,
    std::optional<std::uint64_t> output_bytes = std::nullopt);

[[nodiscard]] std::string render_log_report_text(
    const LogInspectionReport& report, LogReportOperation operation,
    std::optional<std::uint64_t> output_bytes = std::nullopt);

[[nodiscard]] std::string render_replay_report_json(const ReplayReport& report);

[[nodiscard]] std::string render_replay_report_text(const ReplayReport& report);

}  // namespace atlaslob::persistence::detail
