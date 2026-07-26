#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace atlaslob::benchmark {

enum class ReplayBenchmarkMode : std::uint8_t {
  fast = 1,
  verify = 2,
};

struct ReplayLogEvidence final {
  std::uint64_t records{};
  std::uint64_t events{};
  std::uint64_t committed{};
  std::uint64_t rejected{};
  std::string event_digest;

  [[nodiscard]] bool operator==(const ReplayLogEvidence&) const noexcept = default;
};

struct ReplayPreparation final {
  ReplayLogEvidence evidence;
  bool valid{};
  bool operational_failure{};
  std::string failure_reason;
};

struct TimedReplay final {
  std::uint64_t elapsed_ns{};
  bool valid{};
  std::string failure_reason;
};

struct ReplayValidation final {
  std::uint64_t records_scanned{};
  std::uint64_t records_replayed{};
  std::uint64_t committed{};
  std::uint64_t rejected{};
  std::string final_digest;
  bool valid{};
  bool operational_failure{};
  std::string failure_reason;
};

// Validates every encoded frame, hashes the exact complete log, and accumulates
// record evidence without retaining per-record objects. A successful call also
// warms the complete file through the same bounded scanner used by replay.
[[nodiscard]] ReplayPreparation prepare_replay_log(const std::filesystem::path& path,
                                                   std::string_view expected_sha256);

// Times only the public replay_log call and destruction of its owned result,
// engine, report, and event evidence.
[[nodiscard]] TimedReplay execute_timed_replay(const std::filesystem::path& path,
                                               ReplayBenchmarkMode mode);

// Performs the required untimed verified replay after timing.
[[nodiscard]] ReplayValidation validate_replay_log(const std::filesystem::path& path);

}  // namespace atlaslob::benchmark
