#include "benchmark_runner.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(__linux__)
#include <sys/resource.h>
#endif

#include "atlaslob/digest.hpp"
#include "atlaslob/multi_instrument_engine.hpp"
#include "benchmark_observation.hpp"
#include "benchmark_replay.hpp"
#include "benchmark_workload.hpp"
#include "platform_cli.hpp"
#include "sha256.hpp"

namespace atlaslob::benchmark {
namespace {

constexpr std::size_t latency_stride{32U};
constexpr std::size_t maximum_latency_samples{200'000U};

enum class ObservationMode : std::uint8_t {
  throughput,
  latency,
  allocation,
  replay_fast,
  replay_verify,
  construction,
  preload,
  setup_allocation,
};

struct CliOptions final {
  std::string workload_path;
  std::string workload_id;
  std::string workload_manifest_sha256;
  std::string workload_sha256;
  std::string replay_log_path;
  std::string replay_log_sha256;
  std::string binary_sha256;
  std::string environment_sha256;
  std::string host_context_sha256;
  std::string run_label;
  std::string suite_label;
  std::string variant;
  std::string expected_event_digest;
  std::string expected_final_digest;
  std::string expected_empty_state_digest;
  std::string expected_preload_event_digest;
  std::string expected_preload_state_digest;
  MeasurementParameters measurement_parameters;
  std::uint64_t block_index{};
  std::uint64_t block_position{};
  std::uint64_t preload_count{};
  std::uint64_t warmup_count{};
  std::uint64_t measured_count{};
  std::uint64_t expected_events{};
  std::uint64_t expected_committed{};
  std::uint64_t expected_rejected{};
  std::uint64_t expected_engine_errors{};
  std::uint64_t expected_preload_events{};
  std::uint64_t expected_preload_committed{};
  std::uint64_t expected_preload_rejected{};
  std::uint64_t expected_preload_engine_errors{};
  std::uint64_t expected_preload_active_orders{};
  ObservationMode mode{ObservationMode::throughput};
  bool diagnostic_phases{};
};

struct ParseState final {
  bool workload{};
  bool workload_id{};
  bool workload_manifest_sha256{};
  bool workload_sha256{};
  bool replay_log{};
  bool replay_log_sha256{};
  bool binary_sha256{};
  bool environment_sha256{};
  bool host_context_sha256{};
  bool run_label{};
  bool suite_label{};
  bool variant{};
  bool expected_event_digest{};
  bool expected_final_digest{};
  bool expected_empty_state_digest{};
  bool expected_preload_events{};
  bool expected_preload_committed{};
  bool expected_preload_rejected{};
  bool expected_preload_engine_errors{};
  bool expected_preload_event_digest{};
  bool expected_preload_state_digest{};
  bool expected_preload_active_orders{};
  bool block_index{};
  bool block_position{};
  bool preload_count{};
  bool warmup_count{};
  bool measured_count{};
  bool expected_events{};
  bool expected_committed{};
  bool expected_rejected{};
  bool expected_engine_errors{};
  bool mode{};
  bool diagnostic_phases{};
};

struct RegionStatistics final {
  std::uint64_t commands{};
  std::uint64_t events{};
  std::uint64_t committed{};
  std::uint64_t rejected{};
  std::uint64_t engine_errors{};
};

struct ValidationEvidence final {
  RegionStatistics measured;
  std::string event_digest;
  std::string final_digest;
  bool complete{};
};

struct PrefixEvidence final {
  RegionStatistics statistics;
  std::string event_digest;
  std::string final_digest;
  std::uint64_t active_orders{};
  bool complete{};
};

[[nodiscard]] bool canonical_unsigned_token(std::string_view token) noexcept {
  if (token.empty() || (token.size() > 1U && token.front() == '0')) {
    return false;
  }
  return std::all_of(token.begin(), token.end(),
                     [](char value) { return value >= '0' && value <= '9'; });
}

[[nodiscard]] bool parse_u64(std::string_view token, std::uint64_t& destination) noexcept {
  if (!canonical_unsigned_token(token)) {
    return false;
  }
  std::uint64_t value{};
  const auto* const begin = token.data();
  const auto* const end = begin + token.size();
  const auto [parsed_end, error] = std::from_chars(begin, end, value, 10);
  if (error != std::errc{} || parsed_end != end) {
    return false;
  }
  destination = value;
  return true;
}

[[nodiscard]] bool is_sha256(std::string_view value) noexcept {
  return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] bool is_workload_id(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U) {
    return false;
  }
  const auto first = value.front();
  if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
        (first >= '0' && first <= '9'))) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '-' || character == '_' ||
           character == '.';
  });
}

[[nodiscard]] bool is_suite_label(std::string_view value) noexcept {
  return value.size() <= 32U && is_workload_id(value);
}

[[nodiscard]] bool set_string(bool& seen, std::string& output, std::string_view value) {
  if (seen) {
    return false;
  }
  seen = true;
  output.assign(value);
  return true;
}

[[nodiscard]] bool set_count(bool& seen, std::uint64_t& output, std::string_view value) noexcept {
  if (seen || !parse_u64(value, output)) {
    return false;
  }
  seen = true;
  return true;
}

[[nodiscard]] bool insert_measurement_parameter(MeasurementParameters& parameters,
                                                std::string_view encoded) {
  const auto separator = encoded.find('=');
  if (separator == std::string_view::npos ||
      encoded.find('=', separator + 1U) != std::string_view::npos) {
    return false;
  }
  const auto name = encoded.substr(0U, separator);
  const auto value = encoded.substr(separator + 1U);
  if (!is_measurement_parameter_name(name) || !is_measurement_parameter_value(value)) {
    return false;
  }

  const auto position = std::lower_bound(parameters.begin(), parameters.end(), name,
                                         [](const auto& parameter, std::string_view candidate) {
                                           return parameter.first < candidate;
                                         });
  if (position != parameters.end() && position->first == name) {
    return false;
  }
  parameters.insert(position, std::pair{std::string{name}, std::string{value}});
  return true;
}

[[nodiscard]] constexpr bool is_replay_mode(ObservationMode mode) noexcept {
  return mode == ObservationMode::replay_fast || mode == ObservationMode::replay_verify;
}

[[nodiscard]] constexpr bool is_setup_mode(ObservationMode mode) noexcept {
  return mode == ObservationMode::construction || mode == ObservationMode::preload ||
         mode == ObservationMode::setup_allocation;
}

void emit_diagnostic_phase(const CliOptions& options, std::string_view phase) {
  if (!options.diagnostic_phases) {
    return;
  }
  std::cerr << "ATLAS_DIAGNOSTIC_PHASE " << phase << '\n';
  std::cerr.flush();
}

[[nodiscard]] std::optional<std::string_view> measurement_parameter(
    const MeasurementParameters& parameters, std::string_view name) noexcept {
  const auto found = std::lower_bound(parameters.begin(), parameters.end(), name,
                                      [](const auto& parameter, std::string_view candidate) {
                                        return parameter.first < candidate;
                                      });
  if (found == parameters.end() || found->first != name) {
    return std::nullopt;
  }
  return found->second;
}

[[nodiscard]] bool valid_replay_parameters(const CliOptions& options) noexcept {
  if (options.measurement_parameters.size() != 7U) {
    return false;
  }
  const auto instrument_count =
      measurement_parameter(options.measurement_parameters, "instrument_count");
  const auto measured_start =
      measurement_parameter(options.measurement_parameters, "measured_start_active_order_count");
  const auto record_count = measurement_parameter(options.measurement_parameters, "record_count");
  const auto cache_policy = measurement_parameter(options.measurement_parameters, "cache_policy");
  const auto replay_mode = measurement_parameter(options.measurement_parameters, "replay_mode");
  const auto sweep_depth = measurement_parameter(options.measurement_parameters, "sweep_depth");
  const auto timed_input =
      measurement_parameter(options.measurement_parameters, "timed_input_sha256");
  std::uint64_t parsed_instrument_count{};
  std::uint64_t parsed_measured_start{};
  std::uint64_t parsed_records{};
  std::uint64_t parsed_sweep_depth{};
  const auto expected_mode = options.mode == ObservationMode::replay_fast ? "fast" : "verify";
  return instrument_count.has_value() && parse_u64(*instrument_count, parsed_instrument_count) &&
         parsed_instrument_count != 0U && measured_start.has_value() &&
         parse_u64(*measured_start, parsed_measured_start) && record_count.has_value() &&
         parse_u64(*record_count, parsed_records) && parsed_records == options.measured_count &&
         cache_policy == "warm_page_cache" && replay_mode == expected_mode &&
         sweep_depth.has_value() && parse_u64(*sweep_depth, parsed_sweep_depth) &&
         timed_input == options.replay_log_sha256;
}

[[nodiscard]] std::optional<CliOptions> parse_cli(int argc, char** argv, RunnerFlavor flavor) {
  CliOptions options;
  ParseState seen;
  if (flavor == RunnerFlavor::allocation) {
    options.mode = ObservationMode::allocation;
  }

  for (int index = 1; index < argc; index += 2) {
    if (index + 1 >= argc) {
      return std::nullopt;
    }
    const std::string_view name{argv[index]};
    const std::string_view value{argv[index + 1]};
    if (name == "--workload") {
      if (!set_string(seen.workload, options.workload_path, value)) {
        return std::nullopt;
      }
    } else if (name == "--workload-id") {
      if (!set_string(seen.workload_id, options.workload_id, value)) {
        return std::nullopt;
      }
    } else if (name == "--measurement-parameter") {
      if (!insert_measurement_parameter(options.measurement_parameters, value)) {
        return std::nullopt;
      }
    } else if (name == "--workload-manifest-sha256") {
      if (!set_string(seen.workload_manifest_sha256, options.workload_manifest_sha256, value)) {
        return std::nullopt;
      }
    } else if (name == "--workload-sha256") {
      if (!set_string(seen.workload_sha256, options.workload_sha256, value)) {
        return std::nullopt;
      }
    } else if (name == "--replay-log") {
      if (!set_string(seen.replay_log, options.replay_log_path, value)) {
        return std::nullopt;
      }
    } else if (name == "--replay-log-sha256") {
      if (!set_string(seen.replay_log_sha256, options.replay_log_sha256, value)) {
        return std::nullopt;
      }
    } else if (name == "--binary-sha256") {
      if (!set_string(seen.binary_sha256, options.binary_sha256, value)) {
        return std::nullopt;
      }
    } else if (name == "--environment-sha256") {
      if (!set_string(seen.environment_sha256, options.environment_sha256, value)) {
        return std::nullopt;
      }
    } else if (name == "--host-context-sha256") {
      if (!set_string(seen.host_context_sha256, options.host_context_sha256, value)) {
        return std::nullopt;
      }
    } else if (name == "--run-label") {
      if (!set_string(seen.run_label, options.run_label, value)) {
        return std::nullopt;
      }
    } else if (name == "--suite-label") {
      if (!set_string(seen.suite_label, options.suite_label, value)) {
        return std::nullopt;
      }
    } else if (name == "--variant") {
      if (!set_string(seen.variant, options.variant, value)) {
        return std::nullopt;
      }
    } else if (name == "--block-index") {
      if (!set_count(seen.block_index, options.block_index, value)) {
        return std::nullopt;
      }
    } else if (name == "--block-position") {
      if (!set_count(seen.block_position, options.block_position, value)) {
        return std::nullopt;
      }
    } else if (name == "--preload-count") {
      if (!set_count(seen.preload_count, options.preload_count, value)) {
        return std::nullopt;
      }
    } else if (name == "--warmup-count") {
      if (!set_count(seen.warmup_count, options.warmup_count, value)) {
        return std::nullopt;
      }
    } else if (name == "--measured-count") {
      if (!set_count(seen.measured_count, options.measured_count, value)) {
        return std::nullopt;
      }
    } else if (name == "--expected-events") {
      if (!set_count(seen.expected_events, options.expected_events, value)) {
        return std::nullopt;
      }
    } else if (name == "--expected-committed") {
      if (!set_count(seen.expected_committed, options.expected_committed, value)) {
        return std::nullopt;
      }
    } else if (name == "--expected-rejected") {
      if (!set_count(seen.expected_rejected, options.expected_rejected, value)) {
        return std::nullopt;
      }
    } else if (name == "--expected-engine-errors") {
      if (!set_count(seen.expected_engine_errors, options.expected_engine_errors, value)) {
        return std::nullopt;
      }
    } else if (name == "--expected-event-digest") {
      if (!set_string(seen.expected_event_digest, options.expected_event_digest, value)) {
        return std::nullopt;
      }
    } else if (name == "--expected-final-digest") {
      if (!set_string(seen.expected_final_digest, options.expected_final_digest, value)) {
        return std::nullopt;
      }
    } else if (name == "--expected-empty-state-digest") {
      if (!set_string(seen.expected_empty_state_digest, options.expected_empty_state_digest,
                      value)) {
        return std::nullopt;
      }
    } else if (name == "--expected-preload-events") {
      if (!set_count(seen.expected_preload_events, options.expected_preload_events, value)) {
        return std::nullopt;
      }
    } else if (name == "--expected-preload-committed") {
      if (!set_count(seen.expected_preload_committed, options.expected_preload_committed, value)) {
        return std::nullopt;
      }
    } else if (name == "--expected-preload-rejected") {
      if (!set_count(seen.expected_preload_rejected, options.expected_preload_rejected, value)) {
        return std::nullopt;
      }
    } else if (name == "--expected-preload-engine-errors") {
      if (!set_count(seen.expected_preload_engine_errors, options.expected_preload_engine_errors,
                     value)) {
        return std::nullopt;
      }
    } else if (name == "--expected-preload-event-digest") {
      if (!set_string(seen.expected_preload_event_digest, options.expected_preload_event_digest,
                      value)) {
        return std::nullopt;
      }
    } else if (name == "--expected-preload-state-digest") {
      if (!set_string(seen.expected_preload_state_digest, options.expected_preload_state_digest,
                      value)) {
        return std::nullopt;
      }
    } else if (name == "--expected-preload-active-orders") {
      if (!set_count(seen.expected_preload_active_orders, options.expected_preload_active_orders,
                     value)) {
        return std::nullopt;
      }
    } else if (name == "--diagnostic-phases") {
      if (seen.diagnostic_phases || value != "yes") {
        return std::nullopt;
      }
      seen.diagnostic_phases = true;
      options.diagnostic_phases = true;
    } else if (name == "--mode") {
      if (seen.mode) {
        return std::nullopt;
      }
      seen.mode = true;
      if (flavor == RunnerFlavor::timed) {
        if (value == "throughput") {
          options.mode = ObservationMode::throughput;
        } else if (value == "latency") {
          options.mode = ObservationMode::latency;
        } else if (value == "replay-fast") {
          options.mode = ObservationMode::replay_fast;
        } else if (value == "replay-verify") {
          options.mode = ObservationMode::replay_verify;
        } else if (value == "construction") {
          options.mode = ObservationMode::construction;
        } else if (value == "preload") {
          options.mode = ObservationMode::preload;
        } else {
          return std::nullopt;
        }
      } else if (value == "setup-allocation") {
        options.mode = ObservationMode::setup_allocation;
      } else {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
  }

  const bool required =
      seen.workload && seen.workload_id && seen.workload_manifest_sha256 && seen.workload_sha256 &&
      seen.binary_sha256 && seen.environment_sha256 && seen.host_context_sha256 && seen.run_label &&
      seen.suite_label && seen.variant && seen.block_index && seen.block_position &&
      seen.expected_event_digest && seen.expected_final_digest && seen.preload_count &&
      seen.warmup_count && seen.measured_count && seen.expected_events && seen.expected_committed &&
      seen.expected_rejected && seen.expected_engine_errors &&
      (flavor == RunnerFlavor::allocation || seen.mode);
  if (!required || options.measured_count == 0U || !is_workload_id(options.workload_id) ||
      !is_sha256(options.workload_manifest_sha256) || !is_sha256(options.workload_sha256) ||
      !is_sha256(options.binary_sha256) || !is_sha256(options.environment_sha256) ||
      !is_sha256(options.host_context_sha256) || !is_workload_id(options.run_label) ||
      !is_suite_label(options.suite_label) || !is_sha256(options.expected_event_digest) ||
      !is_sha256(options.expected_final_digest)) {
    return std::nullopt;
  }
  const bool replay_mode = is_replay_mode(options.mode);
  const bool any_replay_argument = seen.replay_log || seen.replay_log_sha256;
  if (replay_mode) {
    if (!seen.replay_log || options.replay_log_path.empty() || !seen.replay_log_sha256 ||
        !is_sha256(options.replay_log_sha256) || options.preload_count != 0U ||
        options.warmup_count != 0U || options.expected_engine_errors != 0U ||
        !valid_replay_parameters(options)) {
      return std::nullopt;
    }
  } else if (any_replay_argument) {
    return std::nullopt;
  }
  const bool setup_mode = is_setup_mode(options.mode);
  const bool any_setup_argument =
      seen.expected_empty_state_digest || seen.expected_preload_events ||
      seen.expected_preload_committed || seen.expected_preload_rejected ||
      seen.expected_preload_engine_errors || seen.expected_preload_event_digest ||
      seen.expected_preload_state_digest || seen.expected_preload_active_orders;
  if (setup_mode) {
    const bool all_setup_arguments =
        seen.expected_empty_state_digest && seen.expected_preload_events &&
        seen.expected_preload_committed && seen.expected_preload_rejected &&
        seen.expected_preload_engine_errors && seen.expected_preload_event_digest &&
        seen.expected_preload_state_digest && seen.expected_preload_active_orders;
    if (!all_setup_arguments || !is_sha256(options.expected_empty_state_digest) ||
        !is_sha256(options.expected_preload_event_digest) ||
        !is_sha256(options.expected_preload_state_digest)) {
      return std::nullopt;
    }
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (options.expected_preload_committed > maximum - options.expected_preload_rejected ||
        options.expected_preload_committed + options.expected_preload_rejected >
            maximum - options.expected_preload_engine_errors ||
        options.expected_preload_committed + options.expected_preload_rejected +
                options.expected_preload_engine_errors !=
            options.preload_count) {
      return std::nullopt;
    }
  } else if (any_setup_argument) {
    return std::nullopt;
  }
  const bool standalone =
      options.variant == "standalone" && options.block_index == 0U && options.block_position == 0U;
  const bool comparison = (options.variant == "baseline" || options.variant == "candidate") &&
                          options.block_index != 0U && options.block_position >= 1U &&
                          options.block_position <= 4U;
  if (!standalone && !comparison) {
    return std::nullopt;
  }
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (options.expected_committed > maximum - options.expected_rejected ||
      options.expected_committed + options.expected_rejected >
          maximum - options.expected_engine_errors ||
      options.expected_committed + options.expected_rejected + options.expected_engine_errors !=
          options.measured_count) {
    return std::nullopt;
  }
  return options;
}

void print_usage(std::ostream& output, RunnerFlavor flavor) {
  output << "usage: "
         << (flavor == RunnerFlavor::timed ? "atlas_bench_runner" : "atlas_bench_alloc_runner")
         << " --workload <ATLAS_DIFF_V2> --workload-id <id>"
            " [--measurement-parameter <key=value>]..."
            " --workload-manifest-sha256 <sha256> --workload-sha256 <sha256>"
            " --binary-sha256 <sha256>"
            " --environment-sha256 <sha256> --host-context-sha256 <sha256>"
            " --run-label <id> --suite-label <id>"
            " --variant <standalone|baseline|candidate>"
            " --block-index <n> --block-position <n> --preload-count <n>"
            " --warmup-count <n> --measured-count <n> --expected-events <n>"
            " --expected-committed <n> --expected-rejected <n>"
            " --expected-engine-errors <n>"
            " --expected-event-digest <sha256> --expected-final-digest <sha256>";
  if (flavor == RunnerFlavor::timed) {
    output << " [--replay-log <ATLSLG01> --replay-log-sha256 <sha256>]"
              " --mode <throughput|latency|replay-fast|replay-verify|construction|preload>";
  } else {
    output << " [--mode setup-allocation]";
  }
  output << " [--expected-empty-state-digest <sha256>"
            " --expected-preload-events <n> --expected-preload-committed <n>"
            " --expected-preload-rejected <n> --expected-preload-engine-errors <n>"
            " --expected-preload-event-digest <sha256>"
            " --expected-preload-state-digest <sha256>"
            " --expected-preload-active-orders <n>]";
  output << " [--diagnostic-phases yes]";
  output << '\n';
}

[[nodiscard]] std::optional<std::filesystem::path> running_executable_path(
    [[maybe_unused]] std::string_view fallback) {
  try {
#if defined(__linux__)
    return std::filesystem::read_symlink("/proc/self/exe");
#elif defined(_WIN32)
    std::wstring buffer(32'768U, L'\0');
    const auto count =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count == 0U || count >= buffer.size()) {
      return std::nullopt;
    }
    buffer.resize(static_cast<std::size_t>(count));
    return std::filesystem::path{buffer};
#else
    return std::filesystem::canonical(std::filesystem::path{fallback});
#endif
  } catch (...) {
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::string> hash_file(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::nullopt;
  }
  utility::Sha256 hash;
  std::array<std::uint8_t, 64U * 1024U> buffer{};
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      hash.update(std::span<const std::uint8_t>{buffer.data(), static_cast<std::size_t>(count)});
    }
  }
  if (!input.eof()) {
    return std::nullopt;
  }
  return hash.finish().hex();
}

[[nodiscard]] bool checked_region_total(const CliOptions& options, std::uint64_t& total) noexcept {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (options.preload_count > maximum - options.warmup_count) {
    return false;
  }
  const auto prefix = options.preload_count + options.warmup_count;
  if (prefix > maximum - options.measured_count) {
    return false;
  }
  total = prefix + options.measured_count;
  return true;
}

[[nodiscard]] bool execute_one(MultiInstrumentEngine& engine, const domain::Command& command,
                               RegionStatistics& statistics) {
  auto result = engine.execute(command);
  ++statistics.commands;
  if (const auto* batch = result.batch(); batch != nullptr) {
    statistics.events += static_cast<std::uint64_t>(batch->size());
    if (result.committed()) {
      ++statistics.committed;
    } else {
      ++statistics.rejected;
    }
    return true;
  }
  ++statistics.engine_errors;
  return false;
}

// The call returns only after the owned EngineResult (including its EventBatch)
// has been destroyed. Timed callers therefore include the public execute
// boundary and owned-result lifetime, but not event inspection or evidence
// aggregation.
[[nodiscard]] bool execute_measured_command(MultiInstrumentEngine& engine,
                                            const domain::Command& command) {
  auto result = engine.execute(command);
  return result.has_value();
}

[[nodiscard]] RegionStatistics execute_untimed(MultiInstrumentEngine& engine,
                                               const std::vector<BenchmarkCommand>& commands,
                                               std::size_t begin, std::size_t count) {
  RegionStatistics statistics;
  const auto end = begin + count;
  for (auto index = begin; index < end; ++index) {
    if (!execute_one(engine, commands[index].command, statistics)) {
      break;
    }
  }
  return statistics;
}

[[nodiscard]] std::uint64_t elapsed_nanoseconds(
    std::chrono::steady_clock::time_point begin,
    std::chrono::steady_clock::time_point end) noexcept {
  const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
  return value <= 0 ? 0U : static_cast<std::uint64_t>(value);
}

[[nodiscard]] RegionStatistics execute_throughput(MultiInstrumentEngine& engine,
                                                  const std::vector<BenchmarkCommand>& commands,
                                                  std::size_t begin, std::size_t count,
                                                  std::uint64_t& elapsed_ns) {
  RegionStatistics statistics;
  const auto end = begin + count;
  std::size_t processed{};
  bool engine_error{};
  const auto started = std::chrono::steady_clock::now();
  for (auto index = begin; index < end; ++index) {
    ++processed;
    if (!execute_measured_command(engine, commands[index].command)) {
      engine_error = true;
      break;
    }
  }
  const auto finished = std::chrono::steady_clock::now();
  elapsed_ns = elapsed_nanoseconds(started, finished);
  statistics.commands = static_cast<std::uint64_t>(processed);
  statistics.engine_errors = engine_error ? 1U : 0U;
  return statistics;
}

[[nodiscard]] RegionStatistics execute_latency(MultiInstrumentEngine& engine,
                                               const std::vector<BenchmarkCommand>& commands,
                                               std::size_t begin, std::size_t count,
                                               std::vector<std::uint64_t>& samples,
                                               std::uint64_t& elapsed_ns) {
  RegionStatistics statistics;
  const auto end = begin + count;
  std::size_t processed{};
  bool engine_error{};
  const auto started = std::chrono::steady_clock::now();
  for (auto index = begin; index < end; ++index) {
    const auto measured_index = index - begin;
    const bool sample = measured_index % latency_stride == latency_stride - 1U &&
                        samples.size() < maximum_latency_samples;
    bool complete{};
    if (!sample) {
      complete = execute_measured_command(engine, commands[index].command);
    } else {
      const auto command_started = std::chrono::steady_clock::now();
      complete = execute_measured_command(engine, commands[index].command);
      const auto command_finished = std::chrono::steady_clock::now();
      samples.push_back(elapsed_nanoseconds(command_started, command_finished));
    }
    ++processed;
    if (!complete) {
      engine_error = true;
      break;
    }
  }
  const auto finished = std::chrono::steady_clock::now();
  elapsed_ns = elapsed_nanoseconds(started, finished);
  statistics.commands = static_cast<std::uint64_t>(processed);
  statistics.engine_errors = engine_error ? 1U : 0U;
  return statistics;
}

[[nodiscard]] RegionStatistics execute_allocation(MultiInstrumentEngine& engine,
                                                  const std::vector<BenchmarkCommand>& commands,
                                                  std::size_t begin, std::size_t count) {
  RegionStatistics statistics;
  const auto end = begin + count;
  std::size_t processed{};
  bool engine_error{};
  for (auto index = begin; index < end; ++index) {
    ++processed;
    if (!execute_measured_command(engine, commands[index].command)) {
      engine_error = true;
      break;
    }
  }
  statistics.commands = static_cast<std::uint64_t>(processed);
  statistics.engine_errors = engine_error ? 1U : 0U;
  return statistics;
}

[[nodiscard]] std::optional<std::uint64_t> peak_rss_bytes() noexcept {
#if defined(__linux__)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
    return std::nullopt;
  }
  const auto kibibytes = static_cast<std::uint64_t>(usage.ru_maxrss);
  if (kibibytes > std::numeric_limits<std::uint64_t>::max() / 1024U) {
    return std::nullopt;
  }
  return kibibytes * 1024U;
#else
  return 0U;
#endif
}

[[nodiscard]] std::optional<std::uint64_t> current_rss_bytes() noexcept {
#if defined(__linux__)
  try {
    std::ifstream input{"/proc/self/status"};
    if (!input) {
      return std::nullopt;
    }
    std::string line;
    while (std::getline(input, line)) {
      constexpr std::string_view prefix{"VmRSS:"};
      if (!std::string_view{line}.starts_with(prefix)) {
        continue;
      }
      auto value = std::string_view{line}.substr(prefix.size());
      const auto first = value.find_first_not_of(" \t");
      if (first == std::string_view::npos) {
        return std::nullopt;
      }
      value.remove_prefix(first);
      const auto whitespace = value.find_first_of(" \t");
      const auto token = value.substr(0U, whitespace);
      std::uint64_t kibibytes{};
      if (!parse_u64(token, kibibytes) ||
          kibibytes > std::numeric_limits<std::uint64_t>::max() / 1024U) {
        return std::nullopt;
      }
      return kibibytes * 1024U;
    }
  } catch (...) {
    return std::nullopt;
  }
  return std::nullopt;
#else
  return 0U;
#endif
}

[[nodiscard]] ValidationEvidence derive_validation_evidence(const BenchmarkWorkload& workload,
                                                            std::size_t measured_begin,
                                                            std::size_t measured_count) {
  ValidationEvidence evidence;
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{workload.catalog},
                               workload.engine_config};
  utility::Sha256 event_hash;

  for (std::size_t index = 0U; index < workload.commands.size(); ++index) {
    auto result = engine.execute(workload.commands[index].command);
    const auto* batch = result.batch();
    if (batch == nullptr) {
      if (index >= measured_begin && index < measured_begin + measured_count) {
        ++evidence.measured.commands;
        ++evidence.measured.engine_errors;
      }
      evidence.event_digest = event_hash.finish().hex();
      evidence.final_digest = engine.state_digest().hex();
      return evidence;
    }
    if (index < measured_begin || index >= measured_begin + measured_count) {
      continue;
    }
    ++evidence.measured.commands;
    evidence.measured.events += static_cast<std::uint64_t>(batch->size());
    if (result.committed()) {
      ++evidence.measured.committed;
    } else {
      ++evidence.measured.rejected;
    }
    const auto digest = atlaslob::event_digest(*batch);
    event_hash.update(digest.bytes);
  }
  evidence.event_digest = event_hash.finish().hex();
  evidence.final_digest = engine.state_digest().hex();
  evidence.complete = true;
  return evidence;
}

[[nodiscard]] PrefixEvidence derive_prefix_evidence(const BenchmarkWorkload& workload,
                                                    std::size_t command_count) {
  PrefixEvidence evidence;
  MultiInstrumentEngine engine{std::span<const InstrumentConfig>{workload.catalog},
                               workload.engine_config};
  utility::Sha256 event_hash;

  for (std::size_t index = 0U; index < command_count; ++index) {
    auto result = engine.execute(workload.commands[index].command);
    ++evidence.statistics.commands;
    const auto* batch = result.batch();
    if (batch == nullptr) {
      ++evidence.statistics.engine_errors;
      evidence.event_digest = event_hash.finish().hex();
      evidence.final_digest = engine.state_digest().hex();
      evidence.active_orders = static_cast<std::uint64_t>(engine.active_order_count());
      return evidence;
    }
    evidence.statistics.events += static_cast<std::uint64_t>(batch->size());
    if (result.committed()) {
      ++evidence.statistics.committed;
    } else {
      ++evidence.statistics.rejected;
    }
    event_hash.update(atlaslob::event_digest(*batch).bytes);
  }

  evidence.event_digest = event_hash.finish().hex();
  evidence.final_digest = engine.state_digest().hex();
  evidence.active_orders = static_cast<std::uint64_t>(engine.active_order_count());
  evidence.complete = true;
  return evidence;
}

[[nodiscard]] std::string boundary_name(ObservationMode mode) {
  switch (mode) {
    case ObservationMode::throughput:
      return "core_throughput";
    case ObservationMode::latency:
      return "core_latency";
    case ObservationMode::allocation:
      return "core_allocation";
    case ObservationMode::replay_fast:
      return "replay_fast";
    case ObservationMode::replay_verify:
      return "replay_verify";
    case ObservationMode::construction:
      return "core_construction";
    case ObservationMode::preload:
      return "core_preload";
    case ObservationMode::setup_allocation:
      return "core_setup_allocation";
  }
  return "unknown";
}

[[nodiscard]] BenchmarkObservation initial_observation(const CliOptions& options) {
  return BenchmarkObservation{
      .boundary = boundary_name(options.mode),
      .measurement_parameters = options.measurement_parameters,
      .workload_id = options.workload_id,
      .workload_manifest_sha256 = options.workload_manifest_sha256,
      .workload_sha256 = options.workload_sha256,
      .binary_sha256 = options.binary_sha256,
      .environment_sha256 = options.environment_sha256,
      .host_context_sha256 = options.host_context_sha256,
      .run_label = options.run_label,
      .suite_label = options.suite_label,
      .variant = options.variant,
      .timed_input_kind = is_replay_mode(options.mode) ? "atlslg01" : "none",
      .timed_input_sha256 = is_replay_mode(options.mode)
                                ? std::optional<std::string>{options.replay_log_sha256}
                                : std::nullopt,
      .block_index = options.block_index,
      .block_position = options.block_position,
      .preload_commands = options.preload_count,
      .warmup_commands = options.warmup_count,
      .commands = 0U,
      .events = 0U,
      .committed = 0U,
      .rejected = 0U,
      .engine_errors = 0U,
      .elapsed_ns = 0U,
      .peak_rss_bytes = 0U,
      .rss_before_bytes = 0U,
      .rss_after_bytes = 0U,
      .latency_ns = {},
      .allocations = (options.mode == ObservationMode::allocation ||
                      options.mode == ObservationMode::setup_allocation)
                         ? std::optional<AllocationStatistics>{AllocationStatistics{}}
                         : std::nullopt,
      .event_digest = std::string(64U, '0'),
      .final_digest = std::string(64U, '0'),
      .valid = false,
      .failure_reason = std::nullopt,
  };
}

[[nodiscard]] int emit_observation(BenchmarkObservation& observation, int exit_code) {
  write_observation_json(std::cout, observation);
  std::cout.flush();
  return std::cout ? exit_code : benchmark_operational_error_exit_code;
}

[[nodiscard]] int fail(BenchmarkObservation& observation, std::string reason, int exit_code) {
  observation.valid = false;
  observation.failure_reason = std::move(reason);
  return emit_observation(observation, exit_code);
}

[[nodiscard]] std::optional<std::string> validate_prefix_evidence(const CliOptions& options,
                                                                  const PrefixEvidence& evidence) {
  if (evidence.statistics.commands != options.preload_count) {
    return "preload_engine_error";
  }
  if (evidence.statistics.events != options.expected_preload_events) {
    return "preload_event_count_mismatch";
  }
  if (evidence.statistics.committed != options.expected_preload_committed ||
      evidence.statistics.rejected != options.expected_preload_rejected ||
      evidence.statistics.engine_errors != options.expected_preload_engine_errors) {
    return "preload_outcome_count_mismatch";
  }
  if (evidence.event_digest != options.expected_preload_event_digest) {
    return "preload_event_digest_mismatch";
  }
  if (evidence.final_digest != options.expected_preload_state_digest) {
    return "preload_state_digest_mismatch";
  }
  if (evidence.active_orders != options.expected_preload_active_orders) {
    return "preload_active_order_count_mismatch";
  }
  return std::nullopt;
}

void populate_prefix_observation(BenchmarkObservation& observation,
                                 const PrefixEvidence& evidence) {
  observation.commands = evidence.statistics.commands;
  observation.events = evidence.statistics.events;
  observation.committed = evidence.statistics.committed;
  observation.rejected = evidence.statistics.rejected;
  observation.engine_errors = evidence.statistics.engine_errors;
  observation.event_digest = evidence.event_digest;
  observation.final_digest = evidence.final_digest;
}

[[nodiscard]] bool capture_post_region_rss(BenchmarkObservation& observation) {
  const auto rss_after = current_rss_bytes();
  const auto peak_rss = peak_rss_bytes();
  if (rss_after.has_value()) {
    observation.rss_after_bytes = *rss_after;
  }
  if (peak_rss.has_value()) {
    observation.peak_rss_bytes =
        std::max(*peak_rss,
                 std::max(observation.rss_before_bytes, observation.rss_after_bytes));
  }
  return rss_after.has_value() && peak_rss.has_value();
}

[[nodiscard]] int run_setup(const CliOptions& options, const BenchmarkWorkload& workload,
                            RunnerFlavor flavor, AllocationHooks allocation_hooks,
                            BenchmarkObservation& observation) {
  const auto preload_count = static_cast<std::size_t>(options.preload_count);

  if (options.mode == ObservationMode::construction) {
    const auto rss_before = current_rss_bytes();
    if (!rss_before.has_value()) {
      return fail(observation, "rss_capture_unavailable", benchmark_invalid_observation_exit_code);
    }
    observation.rss_before_bytes = *rss_before;

    std::unique_ptr<MultiInstrumentEngine> engine;
    const auto started = std::chrono::steady_clock::now();
    try {
      engine = std::make_unique<MultiInstrumentEngine>(
          std::span<const InstrumentConfig>{workload.catalog}, workload.engine_config);
    } catch (const std::invalid_argument&) {
      return fail(observation, "invalid_engine_config", benchmark_invalid_observation_exit_code);
    }
    const auto finished = std::chrono::steady_clock::now();
    observation.elapsed_ns = elapsed_nanoseconds(started, finished);
    const auto rss_available = capture_post_region_rss(observation);

    utility::Sha256 empty_event_hash;
    observation.event_digest = empty_event_hash.finish().hex();
    observation.final_digest = engine->state_digest().hex();
    if (observation.final_digest != options.expected_empty_state_digest) {
      return fail(observation, "empty_state_digest_mismatch",
                  benchmark_invalid_observation_exit_code);
    }
    if (engine->active_order_count() != 0U) {
      return fail(observation, "nonempty_constructed_engine",
                  benchmark_invalid_observation_exit_code);
    }

    const auto prefix = derive_prefix_evidence(workload, preload_count);
    if (const auto mismatch = validate_prefix_evidence(options, prefix); mismatch.has_value()) {
      return fail(observation, *mismatch, benchmark_invalid_observation_exit_code);
    }
    if (!rss_available) {
      return fail(observation, "rss_capture_unavailable", benchmark_invalid_observation_exit_code);
    }
    if (observation.elapsed_ns == 0U) {
      return fail(observation, "zero_elapsed_time", benchmark_invalid_observation_exit_code);
    }
  } else if (options.mode == ObservationMode::preload) {
    std::unique_ptr<MultiInstrumentEngine> engine;
    try {
      engine = std::make_unique<MultiInstrumentEngine>(
          std::span<const InstrumentConfig>{workload.catalog}, workload.engine_config);
    } catch (const std::invalid_argument&) {
      return fail(observation, "invalid_engine_config", benchmark_invalid_observation_exit_code);
    }
    const auto rss_before = current_rss_bytes();
    if (!rss_before.has_value()) {
      return fail(observation, "rss_capture_unavailable", benchmark_invalid_observation_exit_code);
    }
    observation.rss_before_bytes = *rss_before;

    const auto measured =
        execute_throughput(*engine, workload.commands, 0U, preload_count, observation.elapsed_ns);
    const auto rss_available = capture_post_region_rss(observation);
    const auto timed_final_digest = engine->state_digest().hex();
    const auto timed_active_orders = static_cast<std::uint64_t>(engine->active_order_count());

    const auto prefix = derive_prefix_evidence(workload, preload_count);
    populate_prefix_observation(observation, prefix);
    if (measured.commands != prefix.statistics.commands ||
        measured.engine_errors != prefix.statistics.engine_errors ||
        timed_final_digest != prefix.final_digest || timed_active_orders != prefix.active_orders) {
      return fail(observation, "preload_validation_divergence",
                  benchmark_invalid_observation_exit_code);
    }
    if (const auto mismatch = validate_prefix_evidence(options, prefix); mismatch.has_value()) {
      return fail(observation, *mismatch, benchmark_invalid_observation_exit_code);
    }
    if (!rss_available) {
      return fail(observation, "rss_capture_unavailable", benchmark_invalid_observation_exit_code);
    }
    if (observation.elapsed_ns == 0U) {
      return fail(observation, "zero_elapsed_time", benchmark_invalid_observation_exit_code);
    }
  } else {
    if (flavor != RunnerFlavor::allocation || allocation_hooks.begin == nullptr ||
        allocation_hooks.end == nullptr) {
      return fail(observation, "allocation_tracker_unavailable",
                  benchmark_operational_error_exit_code);
    }
    const auto rss_before = current_rss_bytes();
    if (!rss_before.has_value()) {
      return fail(observation, "rss_capture_unavailable", benchmark_invalid_observation_exit_code);
    }
    observation.rss_before_bytes = *rss_before;

    std::unique_ptr<MultiInstrumentEngine> engine;
    RegionStatistics measured;
    allocation_hooks.begin();
    try {
      engine = std::make_unique<MultiInstrumentEngine>(
          std::span<const InstrumentConfig>{workload.catalog}, workload.engine_config);
      measured = execute_allocation(*engine, workload.commands, 0U, preload_count);
    } catch (const std::invalid_argument&) {
      observation.allocations = allocation_hooks.end();
      return fail(observation, "invalid_engine_config", benchmark_invalid_observation_exit_code);
    } catch (...) {
      observation.allocations = allocation_hooks.end();
      throw;
    }
    observation.allocations = allocation_hooks.end();
    observation.elapsed_ns = 0U;
    const auto rss_available = capture_post_region_rss(observation);
    const auto timed_final_digest = engine->state_digest().hex();
    const auto timed_active_orders = static_cast<std::uint64_t>(engine->active_order_count());

    const auto prefix = derive_prefix_evidence(workload, preload_count);
    populate_prefix_observation(observation, prefix);
    if (measured.commands != prefix.statistics.commands ||
        measured.engine_errors != prefix.statistics.engine_errors ||
        timed_final_digest != prefix.final_digest || timed_active_orders != prefix.active_orders) {
      return fail(observation, "preload_validation_divergence",
                  benchmark_invalid_observation_exit_code);
    }
    if (const auto mismatch = validate_prefix_evidence(options, prefix); mismatch.has_value()) {
      return fail(observation, *mismatch, benchmark_invalid_observation_exit_code);
    }
    if (!rss_available) {
      return fail(observation, "rss_capture_unavailable", benchmark_invalid_observation_exit_code);
    }
  }

  observation.valid = true;
  observation.failure_reason.reset();
  return emit_observation(observation, benchmark_success_exit_code);
}

[[nodiscard]] int run_replay(const CliOptions& options, BenchmarkObservation& observation) {
  const auto replay_log_path = persistence::detail::path_from_utf8(options.replay_log_path);
  const auto prepared = prepare_replay_log(replay_log_path, options.replay_log_sha256);
  if (!prepared.valid) {
    return fail(observation, prepared.failure_reason,
                prepared.operational_failure ? benchmark_operational_error_exit_code
                                             : benchmark_invalid_observation_exit_code);
  }

  observation.commands = prepared.evidence.records;
  observation.events = prepared.evidence.events;
  observation.committed = prepared.evidence.committed;
  observation.rejected = prepared.evidence.rejected;
  observation.engine_errors = 0U;
  observation.event_digest = prepared.evidence.event_digest;
  if (prepared.evidence.records != options.measured_count) {
    return fail(observation, "replay_record_count_mismatch",
                benchmark_invalid_observation_exit_code);
  }
  if (prepared.evidence.events != options.expected_events) {
    return fail(observation, "event_count_mismatch", benchmark_invalid_observation_exit_code);
  }
  if (prepared.evidence.committed != options.expected_committed ||
      prepared.evidence.rejected != options.expected_rejected) {
    return fail(observation, "outcome_count_mismatch", benchmark_invalid_observation_exit_code);
  }
  if (prepared.evidence.event_digest != options.expected_event_digest) {
    return fail(observation, "event_digest_mismatch", benchmark_invalid_observation_exit_code);
  }

  const auto rss_before = current_rss_bytes();
  if (!rss_before.has_value()) {
    return fail(observation, "rss_capture_unavailable", benchmark_invalid_observation_exit_code);
  }
  observation.rss_before_bytes = *rss_before;

  const auto replay_mode = options.mode == ObservationMode::replay_fast
                               ? ReplayBenchmarkMode::fast
                               : ReplayBenchmarkMode::verify;
  const auto timed = execute_timed_replay(replay_log_path, replay_mode);
  observation.elapsed_ns = timed.elapsed_ns;
  const auto rss_after = current_rss_bytes();
  const auto peak_rss = peak_rss_bytes();
  const bool rss_capture_unavailable = !rss_after.has_value() || !peak_rss.has_value();
  if (rss_after.has_value()) {
    observation.rss_after_bytes = *rss_after;
  }
  if (peak_rss.has_value()) {
    observation.peak_rss_bytes =
        std::max(*peak_rss,
                 std::max(observation.rss_before_bytes, observation.rss_after_bytes));
  }
  if (!timed.valid) {
    return fail(observation, timed.failure_reason, benchmark_invalid_observation_exit_code);
  }

  const auto post_timing = prepare_replay_log(replay_log_path, options.replay_log_sha256);
  if (!post_timing.valid) {
    return fail(observation, post_timing.failure_reason,
                post_timing.operational_failure ? benchmark_operational_error_exit_code
                                                : benchmark_invalid_observation_exit_code);
  }
  if (post_timing.evidence != prepared.evidence) {
    return fail(observation, "replay_source_changed", benchmark_invalid_observation_exit_code);
  }

  const auto validation = validate_replay_log(replay_log_path);
  if (!validation.valid) {
    return fail(observation, validation.failure_reason,
                validation.operational_failure ? benchmark_operational_error_exit_code
                                               : benchmark_invalid_observation_exit_code);
  }
  observation.final_digest = validation.final_digest;
  if (validation.records_scanned != prepared.evidence.records ||
      validation.records_replayed != prepared.evidence.records ||
      validation.committed != prepared.evidence.committed ||
      validation.rejected != prepared.evidence.rejected) {
    return fail(observation, "replay_validation_divergence",
                benchmark_invalid_observation_exit_code);
  }
  if (observation.final_digest != options.expected_final_digest) {
    return fail(observation, "final_digest_mismatch", benchmark_invalid_observation_exit_code);
  }
  if (rss_capture_unavailable) {
    return fail(observation, "rss_capture_unavailable", benchmark_invalid_observation_exit_code);
  }
  if (observation.elapsed_ns == 0U) {
    return fail(observation, "zero_elapsed_time", benchmark_invalid_observation_exit_code);
  }

  observation.valid = true;
  observation.failure_reason.reset();
  return emit_observation(observation, benchmark_success_exit_code);
}

[[nodiscard]] int run(const CliOptions& options, RunnerFlavor flavor,
                      AllocationHooks allocation_hooks, std::string_view executable_path) {
  auto observation = initial_observation(options);

  const auto loaded_executable = running_executable_path(executable_path);
  if (!loaded_executable.has_value()) {
    return fail(observation, "binary_path_resolution_failure",
                benchmark_operational_error_exit_code);
  }
  const auto actual_binary_sha256 = hash_file(*loaded_executable);
  if (!actual_binary_sha256.has_value()) {
    return fail(observation, "binary_io_failure", benchmark_operational_error_exit_code);
  }
  if (*actual_binary_sha256 != options.binary_sha256) {
    return fail(observation, "binary_digest_mismatch", benchmark_invalid_observation_exit_code);
  }
  emit_diagnostic_phase(options, "binary-verified");
  emit_diagnostic_phase(options, "workload-parse-enter");

  std::uint64_t declared_total{};
  if (!checked_region_total(options, declared_total) ||
      declared_total > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
      declared_total > static_cast<std::uint64_t>(maximum_benchmark_command_count)) {
    return fail(observation, "command_region_exceeds_limit",
                benchmark_invalid_observation_exit_code);
  }

  std::ifstream input{persistence::detail::path_from_utf8(options.workload_path), std::ios::binary};
  if (!input) {
    return fail(observation, "workload_io_failure", benchmark_operational_error_exit_code);
  }
  auto parsed = read_atlas_diff_v2(input, maximum_benchmark_catalog_count,
                                   static_cast<std::size_t>(declared_total));
  if (const auto* error = std::get_if<WorkloadParseError>(&parsed)) {
    if (error->code == "input_read_failure") {
      return fail(observation, "workload_io_failure", benchmark_operational_error_exit_code);
    }
    return fail(
        observation,
        "workload_parse_error:" + std::to_string(error->line) + ":" + std::string{error->code},
        benchmark_invalid_observation_exit_code);
  }
  auto workload = std::get<BenchmarkWorkload>(std::move(parsed));
  if (workload.stream_digest.hex() != options.workload_sha256) {
    return fail(observation, "workload_digest_mismatch", benchmark_invalid_observation_exit_code);
  }

  if (declared_total != static_cast<std::uint64_t>(workload.commands.size())) {
    return fail(observation, "command_region_mismatch", benchmark_invalid_observation_exit_code);
  }
  emit_diagnostic_phase(options, "workload-parsed");

  observation.preload_commands = options.preload_count;
  observation.warmup_commands = options.warmup_count;
  if (is_replay_mode(options.mode)) {
    // W10 replay measures the persisted log, not the ATLAS_DIFF_V2 adapter.
    // Release the parsed command stream and its catalog before the RSS
    // baseline and timed replay so they cannot inflate replay memory evidence.
    workload = {};
    input.close();
    return run_replay(options, observation);
  }
  if (is_setup_mode(options.mode)) {
    return run_setup(options, workload, flavor, allocation_hooks, observation);
  }
  if (options.mode == ObservationMode::latency) {
    const auto possible_samples =
        std::min<std::uint64_t>(options.measured_count / latency_stride, maximum_latency_samples);
    observation.latency_ns.reserve(static_cast<std::size_t>(possible_samples));
  }

  emit_diagnostic_phase(options, "engine-create-enter");
  std::unique_ptr<MultiInstrumentEngine> engine;
  const auto rss_before = current_rss_bytes();
  if (!rss_before.has_value()) {
    return fail(observation, "rss_capture_unavailable", benchmark_invalid_observation_exit_code);
  }
  observation.rss_before_bytes = *rss_before;
  try {
    engine = std::make_unique<MultiInstrumentEngine>(
        std::span<const InstrumentConfig>{workload.catalog}, workload.engine_config);
  } catch (const std::invalid_argument&) {
    return fail(observation, "invalid_engine_config", benchmark_invalid_observation_exit_code);
  }
  emit_diagnostic_phase(options, "engine-created");
  emit_diagnostic_phase(options, "preload-enter");

  const auto preload = execute_untimed(*engine, workload.commands, 0U,
                                       static_cast<std::size_t>(options.preload_count));
  if (preload.engine_errors != 0U) {
    return fail(observation, "preload_engine_error", benchmark_invalid_observation_exit_code);
  }
  emit_diagnostic_phase(options, "preload-complete");
  emit_diagnostic_phase(options, "warmup-enter");
  const auto warmup_begin = static_cast<std::size_t>(options.preload_count);
  const auto warmup = execute_untimed(*engine, workload.commands, warmup_begin,
                                      static_cast<std::size_t>(options.warmup_count));
  if (warmup.engine_errors != 0U) {
    return fail(observation, "warmup_engine_error", benchmark_invalid_observation_exit_code);
  }
  emit_diagnostic_phase(options, "warmup-complete");

  const auto measured_begin =
      static_cast<std::size_t>(options.preload_count + options.warmup_count);
  RegionStatistics measured;
  emit_diagnostic_phase(options, "measured-region-enter");
  if (options.mode == ObservationMode::throughput) {
    measured = execute_throughput(*engine, workload.commands, measured_begin,
                                  static_cast<std::size_t>(options.measured_count),
                                  observation.elapsed_ns);
  } else if (options.mode == ObservationMode::latency) {
    measured = execute_latency(*engine, workload.commands, measured_begin,
                               static_cast<std::size_t>(options.measured_count),
                               observation.latency_ns, observation.elapsed_ns);
  } else {
    if (flavor != RunnerFlavor::allocation || allocation_hooks.begin == nullptr ||
        allocation_hooks.end == nullptr) {
      return fail(observation, "allocation_tracker_unavailable",
                  benchmark_operational_error_exit_code);
    }
    allocation_hooks.begin();
    try {
      measured = execute_allocation(*engine, workload.commands, measured_begin,
                                    static_cast<std::size_t>(options.measured_count));
    } catch (...) {
      observation.allocations = allocation_hooks.end();
      throw;
    }
    observation.allocations = allocation_hooks.end();
    observation.elapsed_ns = 0U;
  }
  emit_diagnostic_phase(options, "measured-region-exit");

  observation.commands = measured.commands;
  observation.engine_errors = measured.engine_errors;
  const auto rss_after = current_rss_bytes();
  const auto peak_rss = peak_rss_bytes();
  const bool rss_capture_unavailable = !rss_after.has_value() || !peak_rss.has_value();
  if (rss_after.has_value()) {
    observation.rss_after_bytes = *rss_after;
  }
  if (peak_rss.has_value()) {
    observation.peak_rss_bytes =
        std::max(*peak_rss,
                 std::max(observation.rss_before_bytes, observation.rss_after_bytes));
  }
  emit_diagnostic_phase(options, "state-digest-enter");
  observation.final_digest = engine->state_digest().hex();
  emit_diagnostic_phase(options, "state-digest-complete");

  emit_diagnostic_phase(options, "validation-replay-enter");
  const auto validation = derive_validation_evidence(
      workload, measured_begin, static_cast<std::size_t>(options.measured_count));
  emit_diagnostic_phase(options, "validation-replay-complete");
  if (is_sha256(validation.event_digest)) {
    observation.event_digest = validation.event_digest;
  }
  observation.commands = validation.measured.commands;
  observation.events = validation.measured.events;
  observation.committed = validation.measured.committed;
  observation.rejected = validation.measured.rejected;
  observation.engine_errors = validation.measured.engine_errors;
  if (validation.measured.commands != measured.commands ||
      validation.measured.engine_errors != measured.engine_errors ||
      validation.final_digest != observation.final_digest) {
    return fail(observation, "validation_replay_divergence",
                benchmark_invalid_observation_exit_code);
  }
  if (measured.engine_errors != 0U || measured.commands != options.measured_count) {
    return fail(observation, "measured_engine_error", benchmark_invalid_observation_exit_code);
  }
  if (!validation.complete) {
    return fail(observation, "validation_replay_engine_error",
                benchmark_invalid_observation_exit_code);
  }
  if (rss_capture_unavailable) {
    return fail(observation, "rss_capture_unavailable", benchmark_invalid_observation_exit_code);
  }
  if (validation.measured.events != options.expected_events) {
    return fail(observation, "event_count_mismatch", benchmark_invalid_observation_exit_code);
  }
  if (validation.measured.committed != options.expected_committed ||
      validation.measured.rejected != options.expected_rejected ||
      validation.measured.engine_errors != options.expected_engine_errors) {
    return fail(observation, "outcome_count_mismatch", benchmark_invalid_observation_exit_code);
  }
  if (validation.event_digest != options.expected_event_digest) {
    return fail(observation, "event_digest_mismatch", benchmark_invalid_observation_exit_code);
  }
  if (observation.final_digest != options.expected_final_digest) {
    return fail(observation, "final_digest_mismatch", benchmark_invalid_observation_exit_code);
  }
  if ((options.mode == ObservationMode::throughput || options.mode == ObservationMode::latency) &&
      observation.elapsed_ns == 0U) {
    return fail(observation, "zero_elapsed_time", benchmark_invalid_observation_exit_code);
  }

  observation.valid = true;
  observation.failure_reason.reset();
  emit_diagnostic_phase(options, "observation-ready");
  return emit_observation(observation, benchmark_success_exit_code);
}

}  // namespace

int run_benchmark_cli(int argc, char** argv, RunnerFlavor flavor,
                      AllocationHooks allocation_hooks) {
  if (!persistence::detail::configure_binary_standard_streams()) {
    std::cerr << "failed to configure canonical binary standard output\n";
    return benchmark_operational_error_exit_code;
  }
  const auto options = parse_cli(argc, argv, flavor);
  if (!options.has_value()) {
    print_usage(std::cerr, flavor);
    return benchmark_usage_exit_code;
  }
  try {
    return run(*options, flavor, allocation_hooks, argv[0]);
  } catch (const std::bad_alloc&) {
    auto observation = initial_observation(*options);
    return fail(observation, "resource_failure", benchmark_operational_error_exit_code);
  } catch (const std::exception&) {
    auto observation = initial_observation(*options);
    return fail(observation, "runner_exception", benchmark_operational_error_exit_code);
  } catch (...) {
    auto observation = initial_observation(*options);
    return fail(observation, "unknown_runner_exception", benchmark_operational_error_exit_code);
  }
}

int run_native_benchmark_cli(int argc, char** argv, RunnerFlavor flavor,
                             AllocationHooks allocation_hooks) {
  if (!persistence::detail::configure_binary_standard_streams()) {
    return benchmark_operational_error_exit_code;
  }
  try {
    const auto native = persistence::detail::native_command_line_arguments(argc, argv);
    std::vector<std::string> arguments;
    arguments.reserve(native.arguments().size());
    for (const auto argument : native.arguments()) {
      arguments.emplace_back(argument);
    }
    std::vector<char*> pointers;
    pointers.reserve(arguments.size());
    for (auto& argument : arguments) {
      pointers.push_back(argument.data());
    }
    return run_benchmark_cli(static_cast<int>(pointers.size()), pointers.data(), flavor,
                             allocation_hooks);
  } catch (...) {
    return benchmark_operational_error_exit_code;
  }
}

}  // namespace atlaslob::benchmark
