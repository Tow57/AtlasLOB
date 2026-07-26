#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string_view>
#include <variant>
#include <vector>

#include "atlaslob/digest.hpp"
#include "atlaslob/domain/commands.hpp"
#include "atlaslob/multi_instrument_engine.hpp"

namespace atlaslob::benchmark {

// Private, parsed representation of the frozen ATLAS_DIFF_V2 adapter. Benchmark
// tools intentionally reuse the existing command protocol rather than
// introducing a second command encoding.
struct BenchmarkCommand final {
  domain::Command command;
  std::uint64_t source_line{};
};

struct BenchmarkWorkload final {
  MultiInstrumentEngineConfig engine_config{};
  std::vector<InstrumentConfig> catalog;
  std::uint64_t checkpoint_interval{};
  Digest256 stream_digest{};
  std::vector<BenchmarkCommand> commands;
};

struct WorkloadParseError final {
  std::uint64_t line{};
  std::string_view code;
};

using WorkloadParseResult = std::variant<BenchmarkWorkload, WorkloadParseError>;

inline constexpr std::size_t maximum_benchmark_catalog_count{4'096U};
inline constexpr std::size_t maximum_benchmark_command_count{100'000'000U};
inline constexpr std::size_t maximum_benchmark_line_bytes{1'024U};

// The entire stream is validated before a BenchmarkWorkload is returned.
// Command syntax and raw-enum preservation reuse the frozen ATLAS_DIFF_V2
// adapter. Benchmark evidence is a stricter canonical subset: catalogs are
// ascending and checkpoint records are disabled.
// Header counts and individual record lengths are checked before potentially
// large storage is reserved. The returned stream digest covers the exact bytes
// consumed by this same successful parse, so callers never need to reopen the
// workload between validation and execution.
[[nodiscard]] WorkloadParseResult read_atlas_diff_v2(std::istream& input,
                                                     std::size_t maximum_catalog_count,
                                                     std::size_t expected_command_count);

// Materialization tools trust only the bounded command count encoded in the
// canonical header. The same parser and hard 100M-command ceiling still apply.
[[nodiscard]] WorkloadParseResult read_atlas_diff_v2_declared(std::istream& input,
                                                              std::size_t maximum_catalog_count);

}  // namespace atlaslob::benchmark
