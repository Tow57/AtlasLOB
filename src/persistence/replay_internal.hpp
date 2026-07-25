#pragma once

#include <cstdint>
#include <optional>

#include "atlaslob/multi_instrument_engine.hpp"
#include "atlaslob/persistence/replay.hpp"
#include "log_scanner.hpp"

namespace atlaslob::persistence::detail {

struct ReplayPassResult final {
  LogScanResult scan;
  std::uint64_t records_replayed{};
  std::uint64_t committed{};
  std::uint64_t rejected{};
  std::optional<ReplayDivergence> divergence;
};

[[nodiscard]] ReplayTail public_replay_tail(LogScanTermination termination) noexcept;

[[nodiscard]] bool same_validated_source(const LogScanResult& first,
                                         const LogScanResult& second) noexcept;

// Revalidates the complete source while executing only records whose frame
// begins at or after start_offset. Outcome totals always describe the complete
// validated prefix; records_replayed describes only the executed suffix.
[[nodiscard]] ReplayPassResult execute_replay_pass(LogSource& source, MultiInstrumentEngine& engine,
                                                   ReplayOptions options,
                                                   std::uint64_t start_offset);

// Replays a source whose first structural scan has already been retained.
// The execution pass must identify the same complete source before the engine
// can be published.
[[nodiscard]] ReplayResult replay_validated_log_source(LogSource& source,
                                                       const LogScanResult& first_scan,
                                                       ReplayOptions options);

// Performs both validation passes against one already-open source.
[[nodiscard]] ReplayResult replay_log_source(LogSource& source, ReplayOptions options);

}  // namespace atlaslob::persistence::detail
