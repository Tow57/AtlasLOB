#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "atlaslob/multi_instrument_engine.hpp"
#include "atlaslob/persistence/replay.hpp"
#include "atlaslob/persistence/snapshot.hpp"

namespace atlaslob::persistence {

inline constexpr std::string_view snapshot_report_schema{"ATLAS_SNAPSHOT_REPORT_V1"};
inline constexpr std::string_view snapshot_replay_report_schema{"ATLAS_REPLAY_REPORT_V2"};

enum class RecoverySource : std::uint8_t {
  full_log = 1,
  explicit_snapshot = 2,
  directory_snapshot = 3,
};

[[nodiscard]] constexpr std::string_view to_string(RecoverySource source) noexcept {
  switch (source) {
    case RecoverySource::full_log:
      return "full_log";
    case RecoverySource::explicit_snapshot:
      return "explicit_snapshot";
    case RecoverySource::directory_snapshot:
      return "directory_snapshot";
  }
  return "unknown";
}

struct SnapshotInspectionReport final {
  std::optional<SnapshotFile> snapshot;
  std::uint64_t input_bytes{};
  SnapshotError error{};

  [[nodiscard]] bool valid() const noexcept { return snapshot.has_value() && error.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
};

struct SnapshotPublicationResult final {
  std::filesystem::path path;
  domain::Sequence covered_sequence{};
  std::uint64_t covered_log_byte_offset{};
  std::uint64_t encoded_bytes{};
  bool final_file_visible{};
  SnapshotError error{};

  [[nodiscard]] bool has_value() const noexcept {
    return final_file_visible && !path.empty() && error.ok();
  }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
};

struct SkippedSnapshot final {
  std::filesystem::path path;
  std::optional<domain::Sequence> filename_sequence;
  SnapshotError error{};
};

struct SnapshotRecoveryReport final {
  RecoverySource recovery_source{RecoverySource::full_log};
  std::optional<std::filesystem::path> selected_snapshot;
  std::optional<domain::Sequence> covered_sequence;
  std::optional<std::uint64_t> covered_log_byte_offset;
  std::optional<Digest256> snapshot_state_digest;
  std::vector<SkippedSnapshot> skipped_snapshots;
  SnapshotError snapshot_error{};
  ReplayReport replay{};
};

struct SnapshotRecoveryResult final {
  std::unique_ptr<MultiInstrumentEngine> engine;
  SnapshotRecoveryReport report{};

  [[nodiscard]] bool has_value() const noexcept {
    return engine != nullptr && report.snapshot_error.ok() && report.replay.error.ok() &&
           !report.replay.divergence.has_value();
  }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
};

[[nodiscard]] SnapshotInspectionReport inspect_snapshot(const std::filesystem::path& path,
                                                        CodecLimits limits = {});

[[nodiscard]] SnapshotRecoveryResult recover_log_from_snapshot(
    const std::filesystem::path& log_path, const std::filesystem::path& snapshot_path,
    ReplayOptions options = {});

[[nodiscard]] SnapshotRecoveryResult recover_log_from_snapshot_directory(
    const std::filesystem::path& log_path, const std::filesystem::path& snapshot_directory,
    ReplayOptions options = {});

}  // namespace atlaslob::persistence
