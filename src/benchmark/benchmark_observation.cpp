#include "benchmark_observation.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <ios>
#include <ostream>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace atlaslob::benchmark {
namespace {

[[nodiscard]] constexpr bool is_ascii_alphanumeric(char value) noexcept {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9');
}

[[nodiscard]] bool is_ascii(std::string_view value) noexcept {
  return std::all_of(value.begin(), value.end(),
                     [](char character) { return static_cast<unsigned char>(character) <= 0x7fU; });
}

[[nodiscard]] bool is_lowercase_sha256(std::string_view value) noexcept {
  return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] bool has_valid_timed_input(const BenchmarkObservation& observation) noexcept {
  if (observation.timed_input_kind == "none") {
    return !observation.timed_input_sha256.has_value();
  }
  return observation.timed_input_kind == "atlslg01" && observation.timed_input_sha256.has_value() &&
         is_lowercase_sha256(*observation.timed_input_sha256);
}

[[nodiscard]] bool is_suite_label(std::string_view value) noexcept {
  return value.size() <= 32U && is_measurement_parameter_name(value);
}

void write_json_string(std::ostream& output, std::string_view value) {
  constexpr std::string_view hex{"0123456789abcdef"};
  output.put('"');
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    switch (character) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\b':
        output << "\\b";
        break;
      case '\f':
        output << "\\f";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (byte < 0x20U || byte == 0x7fU) {
          output << "\\u00";
          output.put(hex[(byte >> 4U) & 0x0fU]);
          output.put(hex[byte & 0x0fU]);
        } else {
          output.put(character);
        }
        break;
    }
  }
  output.put('"');
}

template <typename Integer>
void write_decimal(std::ostream& output, Integer value) {
  static_assert(std::is_integral_v<Integer>);
  char buffer[32U]{};
  const auto [end, error] = std::to_chars(std::begin(buffer), std::end(buffer), value, 10);
  if (error != std::errc{}) {
    output.setstate(std::ios::badbit);
    return;
  }
  output.write(buffer, end - buffer);
}

template <typename Integer>
void write_quoted_decimal(std::ostream& output, Integer value) {
  output.put('"');
  write_decimal(output, value);
  output.put('"');
}

}  // namespace

bool is_measurement_parameter_name(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U || !is_ascii_alphanumeric(value.front())) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return is_ascii_alphanumeric(character) || character == '_' || character == '.' ||
           character == '-';
  });
}

bool is_measurement_parameter_value(std::string_view value) noexcept {
  if (value.empty() || value.size() > 256U) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return is_ascii_alphanumeric(character) || character == '_' || character == '.' ||
           character == '+' || character == ',' || character == '-';
  });
}

bool are_canonical_measurement_parameters(const MeasurementParameters& parameters) noexcept {
  std::string_view previous;
  bool first = true;
  for (const auto& [name, value] : parameters) {
    if (!is_measurement_parameter_name(name) || !is_measurement_parameter_value(value) ||
        (!first && previous >= name)) {
      return false;
    }
    first = false;
    previous = name;
  }
  return true;
}

void write_observation_json(std::ostream& output, const BenchmarkObservation& observation) {
  if (!is_ascii(observation.boundary) || !is_ascii(observation.workload_id) ||
      !is_ascii(observation.workload_manifest_sha256) || !is_ascii(observation.workload_sha256) ||
      !is_ascii(observation.binary_sha256) || !is_ascii(observation.environment_sha256) ||
      !is_ascii(observation.host_context_sha256) || !is_ascii(observation.run_label) ||
      !is_suite_label(observation.suite_label) || !is_ascii(observation.variant) ||
      !is_ascii(observation.timed_input_kind) ||
      (observation.timed_input_sha256.has_value() && !is_ascii(*observation.timed_input_sha256)) ||
      !is_ascii(observation.event_digest) || !is_ascii(observation.final_digest) ||
      (observation.failure_reason.has_value() && !is_ascii(*observation.failure_reason)) ||
      !are_canonical_measurement_parameters(observation.measurement_parameters) ||
      !has_valid_timed_input(observation)) {
    output.setstate(std::ios::badbit);
    return;
  }

  // Keep this key order byte-identical to Python's json.dumps(sort_keys=True,
  // separators=(",", ":"), ensure_ascii=True).
  output << "{\"allocations\":";
  if (!observation.allocations.has_value()) {
    output << "null";
  } else {
    const auto& value = *observation.allocations;
    output << "{\"allocated_bytes\":";
    write_quoted_decimal(output, value.allocated_bytes);
    output << ",\"allocation_count\":";
    write_quoted_decimal(output, value.allocation_count);
    output << ",\"deallocation_count\":";
    write_quoted_decimal(output, value.deallocation_count);
    output << ",\"live_bytes\":";
    write_quoted_decimal(output, value.live_bytes);
    output << ",\"peak_live_bytes\":";
    write_quoted_decimal(output, value.peak_live_bytes);
    output.put('}');
  }
  output << ",\"binary_sha256\":";
  write_json_string(output, observation.binary_sha256);
  output << ",\"block_index\":";
  write_quoted_decimal(output, observation.block_index);
  output << ",\"block_position\":";
  write_quoted_decimal(output, observation.block_position);
  output << ",\"boundary\":";
  write_json_string(output, observation.boundary);
  output << ",\"commands\":";
  write_quoted_decimal(output, observation.commands);
  output << ",\"committed\":";
  write_quoted_decimal(output, observation.committed);
  output << ",\"elapsed_ns\":";
  write_quoted_decimal(output, observation.elapsed_ns);
  output << ",\"engine_errors\":";
  write_quoted_decimal(output, observation.engine_errors);
  output << ",\"environment_sha256\":";
  write_json_string(output, observation.environment_sha256);
  output << ",\"event_digest\":";
  write_json_string(output, observation.event_digest);
  output << ",\"events\":";
  write_quoted_decimal(output, observation.events);
  output << ",\"failure_reason\":";
  if (observation.failure_reason.has_value()) {
    write_json_string(output, *observation.failure_reason);
  } else {
    output << "null";
  }
  output << ",\"final_digest\":";
  write_json_string(output, observation.final_digest);
  output << ",\"host_context_sha256\":";
  write_json_string(output, observation.host_context_sha256);
  output << ",\"latency_ns\":";
  if (observation.boundary != "core_latency") {
    output << "null";
  } else {
    output.put('[');
    for (std::size_t index = 0U; index < observation.latency_ns.size(); ++index) {
      if (index != 0U) {
        output.put(',');
      }
      write_quoted_decimal(output, observation.latency_ns[index]);
    }
    output.put(']');
  }
  output << ",\"measurement_parameters\":{";
  for (std::size_t index = 0U; index < observation.measurement_parameters.size(); ++index) {
    if (index != 0U) {
      output.put(',');
    }
    const auto& [name, value] = observation.measurement_parameters[index];
    write_json_string(output, name);
    output.put(':');
    write_json_string(output, value);
  }
  output.put('}');
  output << ",\"peak_rss_bytes\":";
  write_quoted_decimal(output, observation.peak_rss_bytes);
  output << ",\"preload_commands\":";
  write_quoted_decimal(output, observation.preload_commands);
  output << ",\"rejected\":";
  write_quoted_decimal(output, observation.rejected);
  output << ",\"rss_after_bytes\":";
  write_quoted_decimal(output, observation.rss_after_bytes);
  output << ",\"rss_before_bytes\":";
  write_quoted_decimal(output, observation.rss_before_bytes);
  output << ",\"run_label\":";
  write_json_string(output, observation.run_label);
  output << ",\"schema\":\"ATLAS_BENCH_OBSERVATION_V1\"";
  output << ",\"suite_label\":";
  write_json_string(output, observation.suite_label);
  output << ",\"timed_input_kind\":";
  write_json_string(output, observation.timed_input_kind);
  output << ",\"timed_input_sha256\":";
  if (observation.timed_input_sha256.has_value()) {
    write_json_string(output, *observation.timed_input_sha256);
  } else {
    output << "null";
  }
  output << ",\"valid\":" << (observation.valid ? "true" : "false");
  output << ",\"variant\":";
  write_json_string(output, observation.variant);
  output << ",\"warmup_commands\":";
  write_quoted_decimal(output, observation.warmup_commands);
  output << ",\"workload_id\":";
  write_json_string(output, observation.workload_id);
  output << ",\"workload_manifest_sha256\":";
  write_json_string(output, observation.workload_manifest_sha256);
  output << ",\"workload_sha256\":";
  write_json_string(output, observation.workload_sha256);
  output << "}\n";
}

}  // namespace atlaslob::benchmark
