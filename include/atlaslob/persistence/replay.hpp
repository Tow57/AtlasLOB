#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "atlaslob/digest.hpp"
#include "atlaslob/domain/commands.hpp"
#include "atlaslob/domain/events.hpp"
#include "atlaslob/multi_instrument_engine.hpp"
#include "atlaslob/persistence/command_log.hpp"

namespace atlaslob::persistence {

inline constexpr std::string_view log_report_schema{"ATLAS_LOG_REPORT_V1"};
inline constexpr std::string_view replay_report_schema{"ATLAS_REPLAY_REPORT_V1"};

enum class ReplayMode : std::uint8_t {
  fast = 1,
  verify = 2,
  diagnostic = 3,
};

enum class TailPolicy : std::uint8_t {
  strict = 1,
  valid_prefix = 2,
};

[[nodiscard]] constexpr bool is_valid(ReplayMode value) noexcept {
  switch (value) {
    case ReplayMode::fast:
    case ReplayMode::verify:
    case ReplayMode::diagnostic:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool is_valid(TailPolicy value) noexcept {
  switch (value) {
    case TailPolicy::strict:
    case TailPolicy::valid_prefix:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr std::string_view to_string(ReplayMode value) noexcept {
  switch (value) {
    case ReplayMode::fast:
      return "fast";
    case ReplayMode::verify:
      return "verify";
    case ReplayMode::diagnostic:
      return "diagnostic";
  }
  return "unknown";
}

enum class ReplayTail : std::uint8_t {
  clean = 1,
  torn = 2,
  unknown = 3,
};

[[nodiscard]] constexpr std::string_view to_string(ReplayTail value) noexcept {
  switch (value) {
    case ReplayTail::clean:
      return "clean";
    case ReplayTail::torn:
      return "torn";
    case ReplayTail::unknown:
      return "unknown";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(TailPolicy value) noexcept {
  switch (value) {
    case TailPolicy::strict:
      return "strict";
    case TailPolicy::valid_prefix:
      return "valid-prefix";
  }
  return "unknown";
}

struct ReplayOptions final {
  ReplayMode mode{ReplayMode::verify};
  TailPolicy tail_policy{TailPolicy::strict};
  CodecLimits codec_limits{};
  std::size_t invariant_interval{1024U};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return is_valid(mode) && is_valid(tail_policy) && invariant_interval != 0U;
  }
};

enum class ReplayDivergenceCategory : std::uint8_t {
  engine_error = 1,
  outcome = 2,
  rejection_reason = 3,
  event_count = 4,
  event_digest = 5,
  invariant = 6,
};

[[nodiscard]] constexpr std::string_view to_string(ReplayDivergenceCategory value) noexcept {
  switch (value) {
    case ReplayDivergenceCategory::engine_error:
      return "engine_error";
    case ReplayDivergenceCategory::outcome:
      return "outcome";
    case ReplayDivergenceCategory::rejection_reason:
      return "rejection_reason";
    case ReplayDivergenceCategory::event_count:
      return "event_count";
    case ReplayDivergenceCategory::event_digest:
      return "event_digest";
    case ReplayDivergenceCategory::invariant:
      return "invariant";
  }
  return "unknown";
}

struct ReplayEvidenceSummary final {
  std::optional<RecordOutcome> outcome;
  std::optional<domain::RejectReason> rejection_reason;
  std::optional<std::uint64_t> event_count;
  std::optional<Digest256> event_digest;
};

struct ReplayDivergence final {
  std::uint64_t record_offset{};
  domain::Sequence sequence{};
  ReplayDivergenceCategory category{ReplayDivergenceCategory::engine_error};
  std::optional<domain::Command> command;
  ReplayEvidenceSummary expected;
  ReplayEvidenceSummary actual;
  EngineError actual_engine_error{EngineError::none};
  std::vector<domain::Event> actual_events;
};

struct ReplayReport final {
  ReplayMode mode{ReplayMode::verify};
  TailPolicy tail_policy{TailPolicy::strict};
  ReplayTail tail{ReplayTail::unknown};
  std::optional<LogHeader> header;
  std::optional<domain::Sequence> last_sequence;
  std::uint64_t valid_end_offset{};
  std::uint64_t records_scanned{};
  std::uint64_t records_replayed{};
  std::uint64_t committed{};
  std::uint64_t rejected{};
  bool used_valid_prefix{};
  LogError warning{};
  LogError error{};
  std::optional<ReplayDivergence> divergence;
  std::optional<Digest256> final_state_digest;
};

struct ReplayResult final {
  std::unique_ptr<MultiInstrumentEngine> engine;
  ReplayReport report{};

  [[nodiscard]] bool has_value() const noexcept {
    return engine != nullptr && report.error.ok() && !report.divergence.has_value();
  }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
};

[[nodiscard]] ReplayResult replay_log(const std::filesystem::path& path,
                                      ReplayOptions options = {});

}  // namespace atlaslob::persistence
