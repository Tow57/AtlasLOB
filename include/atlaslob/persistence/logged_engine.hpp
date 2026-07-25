#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>

#include "atlaslob/domain/commands.hpp"
#include "atlaslob/matching_engine.hpp"
#include "atlaslob/multi_instrument_engine.hpp"
#include "atlaslob/persistence/types.hpp"

namespace atlaslob::persistence {

struct LoggedEngineOptions final {
  Durability durability{Durability::sync_each_record};
  CodecLimits codec_limits{};

  [[nodiscard]] constexpr bool valid() const noexcept { return is_valid(durability); }

  bool operator==(const LoggedEngineOptions&) const = default;
};

class LoggedEngine;

struct LoggedEngineOpenResult final {
  std::unique_ptr<LoggedEngine> engine;
  LogError error{};

  [[nodiscard]] bool has_value() const noexcept { return engine != nullptr && error.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
};

struct LoggedSubmissionResult final {
  std::optional<EngineResult> engine_result;
  LogError error{};
  bool session_poisoned{};

  [[nodiscard]] bool has_value() const noexcept { return engine_result.has_value() && error.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
};

// One logical writer that places every sequenced command in ATLSLG01 before
// publishing its already-prepared engine mutation.
class LoggedEngine final {
 public:
  [[nodiscard]] static LoggedEngineOpenResult create_new(
      const std::filesystem::path& path, std::span<const InstrumentConfig> catalog,
      MultiInstrumentEngineConfig engine_config = {}, LoggedEngineOptions options = {});
  [[nodiscard]] static LoggedEngineOpenResult create_new(
      const std::filesystem::path& path, std::span<const InstrumentConfig> catalog, LogId log_id,
      MultiInstrumentEngineConfig engine_config = {}, LoggedEngineOptions options = {});

  ~LoggedEngine() noexcept;

  LoggedEngine(const LoggedEngine&) = delete;
  LoggedEngine& operator=(const LoggedEngine&) = delete;
  LoggedEngine(LoggedEngine&&) = delete;
  LoggedEngine& operator=(LoggedEngine&&) = delete;

  [[nodiscard]] LoggedSubmissionResult submit(const domain::NewOrder& command);
  [[nodiscard]] LoggedSubmissionResult submit(const domain::CancelOrder& command);
  [[nodiscard]] LoggedSubmissionResult submit(const domain::ReplaceOrder& command);
  [[nodiscard]] LoggedSubmissionResult submit(const domain::Command& command);

  // Required by snapshot publication even when normal writes are buffered.
  [[nodiscard]] LogError synchronize() noexcept;

  [[nodiscard]] const MultiInstrumentEngine& engine() const noexcept;
  [[nodiscard]] bool poisoned() const noexcept;
  [[nodiscard]] Durability durability() const noexcept;
  [[nodiscard]] LogId log_id() const noexcept;
  [[nodiscard]] std::uint64_t log_offset() const noexcept;

 private:
  class Impl;
  explicit LoggedEngine(std::unique_ptr<Impl> implementation) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace atlaslob::persistence
