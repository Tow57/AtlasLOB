#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "allocation_tracker.hpp"

namespace atlaslob::benchmark {

using MeasurementParameters = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] bool is_measurement_parameter_name(std::string_view value) noexcept;
[[nodiscard]] bool is_measurement_parameter_value(std::string_view value) noexcept;
[[nodiscard]] bool are_canonical_measurement_parameters(
    const MeasurementParameters& parameters) noexcept;

struct BenchmarkObservation final {
  std::string boundary;
  MeasurementParameters measurement_parameters;
  std::string workload_id;
  std::string workload_manifest_sha256;
  std::string workload_sha256;
  std::string binary_sha256;
  std::string environment_sha256;
  std::string host_context_sha256;
  std::string run_label;
  std::string suite_label;
  std::string variant;
  std::string timed_input_kind{"none"};
  std::optional<std::string> timed_input_sha256;
  std::uint64_t block_index{};
  std::uint64_t block_position{};
  std::uint64_t preload_commands{};
  std::uint64_t warmup_commands{};
  std::uint64_t commands{};
  std::uint64_t events{};
  std::uint64_t committed{};
  std::uint64_t rejected{};
  std::uint64_t engine_errors{};
  std::uint64_t elapsed_ns{};
  std::uint64_t peak_rss_bytes{};
  std::uint64_t rss_before_bytes{};
  std::uint64_t rss_after_bytes{};
  std::vector<std::uint64_t> latency_ns;
  std::optional<AllocationStatistics> allocations;
  std::string event_digest;
  std::string final_digest;
  bool valid{};
  std::optional<std::string> failure_reason;
};

// Emits one LF-terminated, compact, canonical-key-order ASCII JSON object.
// All integer counts and nanoseconds are encoded as quoted canonical decimals.
// String members must contain ASCII bytes; a violation sets badbit before any
// output is written. JSON control bytes and DEL are escaped canonically.
void write_observation_json(std::ostream& output, const BenchmarkObservation& observation);

}  // namespace atlaslob::benchmark
