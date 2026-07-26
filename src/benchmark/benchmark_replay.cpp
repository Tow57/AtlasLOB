#include "benchmark_replay.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <utility>

#include "atlaslob/persistence/command_log.hpp"
#include "atlaslob/persistence/replay.hpp"
#include "log_io.hpp"
#include "log_scanner.hpp"
#include "sha256.hpp"

namespace atlaslob::benchmark {
namespace {

class EvidenceVisitor final : public persistence::detail::LogScanVisitor {
 public:
  void on_record(const persistence::CommandRecord& record, std::span<const std::uint8_t> encoded,
                 std::uint64_t frame_begin, std::uint64_t frame_end) override {
    static_cast<void>(encoded);
    static_cast<void>(frame_begin);
    static_cast<void>(frame_end);
    if (record.event_count > std::numeric_limits<std::uint64_t>::max() - evidence_.events) {
      overflow_ = true;
      return;
    }
    evidence_.events += record.event_count;
    if (record.outcome == persistence::RecordOutcome::committed) {
      if (evidence_.committed == std::numeric_limits<std::uint64_t>::max()) {
        overflow_ = true;
        return;
      }
      ++evidence_.committed;
    } else {
      if (evidence_.rejected == std::numeric_limits<std::uint64_t>::max()) {
        overflow_ = true;
        return;
      }
      ++evidence_.rejected;
    }
    event_hash_.update(record.event_digest.bytes);
  }

  [[nodiscard]] ReplayLogEvidence finish(std::uint64_t records) noexcept {
    evidence_.records = records;
    evidence_.event_digest = event_hash_.finish().hex();
    return std::move(evidence_);
  }

  [[nodiscard]] bool overflowed() const noexcept { return overflow_; }

 private:
  ReplayLogEvidence evidence_;
  utility::Sha256 event_hash_;
  bool overflow_{};
};

[[nodiscard]] std::uint64_t elapsed_nanoseconds(
    std::chrono::steady_clock::time_point begin,
    std::chrono::steady_clock::time_point end) noexcept {
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
  return elapsed <= 0 ? 0U : static_cast<std::uint64_t>(elapsed);
}

[[nodiscard]] bool operational(persistence::detail::LogScanTermination termination) noexcept {
  return termination == persistence::detail::LogScanTermination::io_failure;
}

}  // namespace

ReplayPreparation prepare_replay_log(const std::filesystem::path& path,
                                     std::string_view expected_sha256) {
  auto opened = persistence::detail::open_native_log_source(path);
  if (!opened) {
    return {
        .evidence = {},
        .valid = false,
        .operational_failure = true,
        .failure_reason = "replay_log_io_failure",
    };
  }

  EvidenceVisitor visitor;
  const auto scan = persistence::detail::scan_command_log(*opened.source, {}, &visitor);
  if (!scan.clean()) {
    return {
        .evidence = {},
        .valid = false,
        .operational_failure = operational(scan.termination),
        .failure_reason =
            operational(scan.termination) ? "replay_log_io_failure" : "replay_log_invalid",
    };
  }
  if (visitor.overflowed()) {
    return {
        .evidence = {},
        .valid = false,
        .operational_failure = false,
        .failure_reason = "replay_evidence_overflow",
    };
  }
  if (!scan.valid_prefix_digest.has_value() || scan.valid_prefix_digest->hex() != expected_sha256) {
    return {
        .evidence = {},
        .valid = false,
        .operational_failure = false,
        .failure_reason = "replay_log_digest_mismatch",
    };
  }
  return {
      .evidence = visitor.finish(scan.record_count),
      .valid = true,
      .operational_failure = false,
      .failure_reason = {},
  };
}

TimedReplay execute_timed_replay(const std::filesystem::path& path, ReplayBenchmarkMode mode) {
  persistence::ReplayOptions options;
  options.mode = mode == ReplayBenchmarkMode::fast ? persistence::ReplayMode::fast
                                                   : persistence::ReplayMode::verify;
  options.tail_policy = persistence::TailPolicy::strict;

  bool valid{};
  const auto started = std::chrono::steady_clock::now();
  {
    auto replayed = persistence::replay_log(path, options);
    valid = replayed.has_value();
  }
  const auto finished = std::chrono::steady_clock::now();

  return {
      .elapsed_ns = elapsed_nanoseconds(started, finished),
      .valid = valid,
      .failure_reason = valid ? std::string{} : std::string{"timed_replay_failure"},
  };
}

ReplayValidation validate_replay_log(const std::filesystem::path& path) {
  persistence::ReplayOptions options;
  options.mode = persistence::ReplayMode::verify;
  options.tail_policy = persistence::TailPolicy::strict;
  auto replayed = persistence::replay_log(path, options);
  if (!replayed) {
    const bool io_failure =
        replayed.report.error.category == persistence::LogErrorCategory::io_failure;
    return {
        .records_scanned = replayed.report.records_scanned,
        .records_replayed = replayed.report.records_replayed,
        .committed = replayed.report.committed,
        .rejected = replayed.report.rejected,
        .final_digest = {},
        .valid = false,
        .operational_failure = io_failure,
        .failure_reason = io_failure ? "replay_validation_io_failure" : "replay_validation_failure",
    };
  }
  return {
      .records_scanned = replayed.report.records_scanned,
      .records_replayed = replayed.report.records_replayed,
      .committed = replayed.report.committed,
      .rejected = replayed.report.rejected,
      .final_digest = replayed.report.final_state_digest->hex(),
      .valid = true,
      .operational_failure = false,
      .failure_reason = {},
  };
}

}  // namespace atlaslob::benchmark
