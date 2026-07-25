#pragma once

#include <filesystem>

#include "atlaslob/multi_instrument_engine.hpp"
#include "atlaslob/persistence/snapshot_store.hpp"

namespace atlaslob::persistence::detail {

class LogSource;

enum class SnapshotPublicationStage : std::uint8_t {
  create_temporary = 1,
  write = 2,
  flush = 3,
  sync = 4,
  close = 5,
  reread = 6,
  verify = 7,
  rename = 8,
  cleanup = 9,
};

using SnapshotPublicationHook = SnapshotError (*)(SnapshotPublicationStage, std::uint64_t) noexcept;

// Private deterministic fault-injection seam.
void set_snapshot_publication_hook_for_testing(SnapshotPublicationHook hook) noexcept;

[[nodiscard]] SnapshotPublicationResult publish_snapshot(const std::filesystem::path& directory,
                                                         const MultiInstrumentEngine& engine,
                                                         LogId log_id,
                                                         std::uint64_t covered_log_byte_offset,
                                                         CodecLimits limits);

// Recovery variants that retain a caller-owned, already-open source for every
// validation and replay pass. LoggedEngine uses these so the append handle is
// opened only after recovery completes against one stable source handle.
[[nodiscard]] SnapshotRecoveryResult recover_log_from_snapshot_source(
    LogSource& source, const std::filesystem::path& snapshot_path, ReplayOptions options = {});

[[nodiscard]] SnapshotRecoveryResult recover_log_from_snapshot_directory_source(
    LogSource& source, const std::filesystem::path& snapshot_directory, ReplayOptions options = {});

}  // namespace atlaslob::persistence::detail
