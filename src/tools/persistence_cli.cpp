#include "persistence_cli.hpp"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include "atlaslob/persistence/inspection.hpp"
#include "atlaslob/persistence/replay.hpp"
#include "platform_cli.hpp"
#include "reports.hpp"

namespace atlaslob::persistence::detail {
namespace {

void print_inspect_usage(std::string_view program, std::ostream& error) {
  error << "Usage:\n"
        << "  " << program << " log <path> [--json] [--records]\n"
        << "  " << program << " repair-tail <input> <new-output> [--json]\n";
}

void print_replay_usage(std::string_view program, std::ostream& error) {
  error << "Usage:\n"
        << "  " << program
        << " <log> [--mode fast|verify|diagnostic]"
           " [--tail-policy strict|valid-prefix] [--json]\n";
}

[[nodiscard]] bool write_report(std::ostream& output, const std::string& report) {
  output << report;
  output.flush();
  return static_cast<bool>(output);
}

[[nodiscard]] int inspection_exit_code(const LogInspectionReport& report) noexcept {
  if (report.error.category == LogErrorCategory::io_failure) {
    return cli_io_failure_exit_code;
  }
  if (report.error || report.tail != LogTail::clean) {
    return cli_invalid_data_exit_code;
  }
  return EXIT_SUCCESS;
}

[[nodiscard]] bool parse_mode(std::string_view text, ReplayMode& mode) noexcept {
  if (text == "fast") {
    mode = ReplayMode::fast;
    return true;
  }
  if (text == "verify") {
    mode = ReplayMode::verify;
    return true;
  }
  if (text == "diagnostic") {
    mode = ReplayMode::diagnostic;
    return true;
  }
  return false;
}

[[nodiscard]] bool parse_tail_policy(std::string_view text, TailPolicy& policy) noexcept {
  if (text == "strict") {
    policy = TailPolicy::strict;
    return true;
  }
  if (text == "valid-prefix") {
    policy = TailPolicy::valid_prefix;
    return true;
  }
  return false;
}

[[nodiscard]] int replay_exit_code(const ReplayResult& replayed) noexcept {
  if (replayed.report.error.category == LogErrorCategory::io_failure) {
    return cli_io_failure_exit_code;
  }
  if (replayed.report.error || replayed.report.divergence.has_value() ||
      replayed.engine == nullptr) {
    return cli_invalid_data_exit_code;
  }
  return EXIT_SUCCESS;
}

}  // namespace

int run_atlas_inspect(std::span<const std::string_view> arguments, std::ostream& output,
                      std::ostream& error) {
  const auto program = arguments.empty() ? std::string_view{"atlas_inspect"} : arguments[0];
  if (arguments.size() >= 3U && arguments[1] == "log") {
    bool json{};
    bool records{};
    for (std::size_t index = 3U; index < arguments.size(); ++index) {
      const auto option = arguments[index];
      if (option == "--json" && !json) {
        json = true;
      } else if (option == "--records" && !records) {
        records = true;
      } else {
        print_inspect_usage(program, error);
        return cli_usage_exit_code;
      }
    }

    const auto report = inspect_log(path_from_utf8(arguments[2]), records);
    const auto rendered = json ? render_log_report_json(report, LogReportOperation::inspect_log)
                               : render_log_report_text(report, LogReportOperation::inspect_log);
    if (!write_report(output, rendered)) {
      return cli_io_failure_exit_code;
    }
    return inspection_exit_code(report);
  }

  if (arguments.size() >= 4U && arguments[1] == "repair-tail") {
    bool json{};
    for (std::size_t index = 4U; index < arguments.size(); ++index) {
      const auto option = arguments[index];
      if (option == "--json" && !json) {
        json = true;
      } else {
        print_inspect_usage(program, error);
        return cli_usage_exit_code;
      }
    }

    const auto repaired =
        repair_log_tail(path_from_utf8(arguments[2]), path_from_utf8(arguments[3]));
    const auto rendered =
        json ? render_log_report_json(repaired.inspection, LogReportOperation::repair_tail,
                                      repaired.output_bytes)
             : render_log_report_text(repaired.inspection, LogReportOperation::repair_tail,
                                      repaired.output_bytes);
    if (!write_report(output, rendered)) {
      return cli_io_failure_exit_code;
    }
    if (repaired.inspection.error.category == LogErrorCategory::io_failure) {
      return cli_io_failure_exit_code;
    }
    const bool repaired_tail =
        repaired.inspection.tail == LogTail::torn && repaired.inspection.warning();
    if (!repaired.output_created || !repaired_tail) {
      return cli_invalid_data_exit_code;
    }
    return EXIT_SUCCESS;
  }

  print_inspect_usage(program, error);
  return cli_usage_exit_code;
}

int run_atlas_replay(std::span<const std::string_view> arguments, std::ostream& output,
                     std::ostream& error) {
  const auto program = arguments.empty() ? std::string_view{"atlas_replay"} : arguments[0];
  if (arguments.size() < 2U) {
    print_replay_usage(program, error);
    return cli_usage_exit_code;
  }

  ReplayOptions options;
  bool json{};
  bool mode_seen{};
  bool tail_policy_seen{};
  for (std::size_t index = 2U; index < arguments.size(); ++index) {
    const auto option = arguments[index];
    if (option == "--json" && !json) {
      json = true;
      continue;
    }
    if (option == "--mode" && !mode_seen && index + 1U < arguments.size()) {
      mode_seen = true;
      ++index;
      if (!parse_mode(arguments[index], options.mode)) {
        print_replay_usage(program, error);
        return cli_usage_exit_code;
      }
      continue;
    }
    if (option == "--tail-policy" && !tail_policy_seen && index + 1U < arguments.size()) {
      tail_policy_seen = true;
      ++index;
      if (!parse_tail_policy(arguments[index], options.tail_policy)) {
        print_replay_usage(program, error);
        return cli_usage_exit_code;
      }
      continue;
    }
    print_replay_usage(program, error);
    return cli_usage_exit_code;
  }

  const auto replayed = replay_log(path_from_utf8(arguments[1]), options);
  const auto rendered = json ? render_replay_report_json(replayed.report)
                             : render_replay_report_text(replayed.report);
  if (!write_report(output, rendered)) {
    return cli_io_failure_exit_code;
  }
  return replay_exit_code(replayed);
}

}  // namespace atlaslob::persistence::detail
