#include "atlaslob/persistence/logged_engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <variant>

#include "atlaslob/domain/events.hpp"
#include "atlaslob/persistence/command_log.hpp"
#include "command_log_codec.hpp"
#include "logged_engine_internal.hpp"
#include "multi_instrument_engine_access.hpp"

namespace atlaslob::persistence {
namespace {

[[nodiscard]] LogId generate_log_id() {
  std::random_device source;
  LogId result;
  for (auto& byte : result.bytes) {
    byte = static_cast<std::uint8_t>(source() & 0xffU);
  }
  return result;
}

[[nodiscard]] LogError state_error(std::uint64_t offset, std::errc condition) noexcept {
  return {
      .category = LogErrorCategory::io_failure,
      .byte_offset = offset,
      .system_error = std::make_error_code(condition),
  };
}

}  // namespace

namespace detail {

LogError as_log_error(const LogIoFailure& failure) noexcept {
  if (!failure) {
    return {};
  }
  return {
      .category = LogErrorCategory::io_failure,
      .byte_offset = failure.offset,
      .system_error = failure.system_error,
  };
}

LogError write_all(LogSink& sink, std::span<const std::uint8_t> bytes) noexcept {
  auto remaining = std::as_bytes(bytes);
  while (!remaining.empty()) {
    const auto result = sink.write(remaining);
    if (result.failure) {
      return as_log_error(result.failure);
    }
    if (result.bytes_written == 0U || result.bytes_written > remaining.size()) {
      return state_error(sink.position(), std::errc::io_error);
    }
    remaining = remaining.subspan(result.bytes_written);
  }
  return {};
}

LogError apply_durability(LogSink& sink, Durability durability) noexcept {
  switch (durability) {
    case Durability::buffered:
      return {};
    case Durability::flush_each_record:
      return as_log_error(sink.flush());
    case Durability::sync_each_record: {
      const auto flush_error = as_log_error(sink.flush());
      if (flush_error) {
        return flush_error;
      }
      return as_log_error(sink.sync());
    }
  }
  return state_error(sink.position(), std::errc::invalid_argument);
}

LoggedSubmissionResult submit_logged(MultiInstrumentEngine& engine, LogSink& sink, bool& poisoned,
                                     Durability durability, CodecLimits limits,
                                     const domain::Command& command) {
  if (poisoned) {
    return {
        .engine_result = std::nullopt,
        .error = state_error(sink.position(), std::errc::state_not_recoverable),
        .session_poisoned = true,
    };
  }

  auto prepared = core::MultiInstrumentEngineAccess::prepare(engine, command);
  const auto* batch = prepared.batch();
  if (batch == nullptr) {
    return {
        .engine_result = prepared.commit(),
        .error = {},
        .session_poisoned = false,
    };
  }

  auto rejection_reason = domain::RejectReason::none;
  const auto outcome = prepared.rejected() ? RecordOutcome::rejected : RecordOutcome::committed;
  if (outcome == RecordOutcome::rejected) {
    if (batch->empty()) {
      return {
          .engine_result = std::nullopt,
          .error = state_error(sink.position(), std::errc::state_not_recoverable),
          .session_poisoned = false,
      };
    }
    const auto* rejection = std::get_if<domain::RejectedEvent>(&(*batch)[0]);
    if (rejection == nullptr) {
      return {
          .engine_result = std::nullopt,
          .error = state_error(sink.position(), std::errc::state_not_recoverable),
          .session_poisoned = false,
      };
    }
    rejection_reason = rejection->reason;
  }

  const CommandRecord record{
      .record_version = command_log_record_version,
      .command = command,
      .sequence = batch->command_sequence(),
      .outcome = outcome,
      .rejection_reason = rejection_reason,
      .event_count = static_cast<std::uint64_t>(batch->size()),
      .event_digest = atlaslob::event_digest(*batch),
  };
  auto encoded = encode_command_record(record, limits);
  if (!encoded) {
    return {
        .engine_result = std::nullopt,
        .error = encoded.error,
        .session_poisoned = false,
    };
  }

  const auto write_error = write_all(sink, *encoded.value);
  if (write_error) {
    poisoned = true;
    return {
        .engine_result = std::nullopt,
        .error = write_error,
        .session_poisoned = true,
    };
  }
  const auto durability_error = apply_durability(sink, durability);
  if (durability_error) {
    poisoned = true;
    return {
        .engine_result = std::nullopt,
        .error = durability_error,
        .session_poisoned = true,
    };
  }

  auto committed = prepared.commit();
  const auto* committed_batch = committed.batch();
  if (committed_batch == nullptr || committed_batch->command_sequence() != record.sequence ||
      committed_batch->size() != record.event_count ||
      atlaslob::event_digest(*committed_batch) != record.event_digest ||
      committed.rejected() != (record.outcome == RecordOutcome::rejected)) {
    poisoned = true;
    return {
        .engine_result = std::nullopt,
        .error = state_error(sink.position(), std::errc::state_not_recoverable),
        .session_poisoned = true,
    };
  }

  return {
      .engine_result = std::move(committed),
      .error = {},
      .session_poisoned = false,
  };
}

}  // namespace detail

class LoggedEngine::Impl final {
 public:
  Impl(std::unique_ptr<MultiInstrumentEngine> engine,
       std::unique_ptr<detail::NativeAppendFileSink> sink, LogId log_id,
       LoggedEngineOptions options) noexcept
      : engine_{std::move(engine)}, sink_{std::move(sink)}, log_id_{log_id}, options_{options} {}

  ~Impl() noexcept {
    if (sink_ != nullptr) {
      static_cast<void>(sink_->close());
    }
  }

  std::unique_ptr<MultiInstrumentEngine> engine_;
  std::unique_ptr<detail::NativeAppendFileSink> sink_;
  LogId log_id_{};
  LoggedEngineOptions options_{};
  bool poisoned_{};
};

LoggedEngineOpenResult LoggedEngine::create_new(const std::filesystem::path& path,
                                                std::span<const InstrumentConfig> catalog,
                                                MultiInstrumentEngineConfig engine_config,
                                                LoggedEngineOptions options) {
  return create_new(path, catalog, generate_log_id(), engine_config, options);
}

LoggedEngineOpenResult LoggedEngine::create_new(const std::filesystem::path& path,
                                                std::span<const InstrumentConfig> catalog,
                                                LogId log_id,
                                                MultiInstrumentEngineConfig engine_config,
                                                LoggedEngineOptions options) {
  if (!options.valid()) {
    throw std::invalid_argument{"invalid logged-engine options"};
  }

  auto engine = std::make_unique<MultiInstrumentEngine>(catalog, engine_config);
  const auto initial_snapshot = engine->snapshot();
  auto header =
      detail::make_log_header(initial_snapshot.catalog, initial_snapshot.engine_config, log_id);
  if (!header) {
    return {
        .engine = nullptr,
        .error = header.error,
    };
  }
  auto encoded = detail::encode_log_header(*header.value, options.codec_limits);
  if (!encoded) {
    return {
        .engine = nullptr,
        .error = encoded.error,
    };
  }

  auto opened = detail::open_native_append_log_sink(path);
  if (!opened) {
    return {
        .engine = nullptr,
        .error = detail::as_log_error(opened.failure),
    };
  }
  auto sink = std::move(opened.sink);
  const auto write_error = detail::write_all(*sink, *encoded.value);
  if (write_error) {
    return {
        .engine = nullptr,
        .error = write_error,
    };
  }
  const auto durability_error = detail::apply_durability(*sink, options.durability);
  if (durability_error) {
    return {
        .engine = nullptr,
        .error = durability_error,
    };
  }
  sink->mark_header_published();

  return {
      .engine = std::unique_ptr<LoggedEngine>{new LoggedEngine{
          std::make_unique<Impl>(std::move(engine), std::move(sink), log_id, options)}},
      .error = {},
  };
}

LoggedEngine::LoggedEngine(std::unique_ptr<Impl> implementation) noexcept
    : impl_{std::move(implementation)} {}

LoggedEngine::~LoggedEngine() noexcept = default;

LoggedSubmissionResult LoggedEngine::submit(const domain::NewOrder& command) {
  return submit(domain::Command{command});
}

LoggedSubmissionResult LoggedEngine::submit(const domain::CancelOrder& command) {
  return submit(domain::Command{command});
}

LoggedSubmissionResult LoggedEngine::submit(const domain::ReplaceOrder& command) {
  return submit(domain::Command{command});
}

LoggedSubmissionResult LoggedEngine::submit(const domain::Command& command) {
  return detail::submit_logged(*impl_->engine_, *impl_->sink_, impl_->poisoned_,
                               impl_->options_.durability, impl_->options_.codec_limits, command);
}

LogError LoggedEngine::synchronize() noexcept {
  if (impl_->poisoned_) {
    return state_error(impl_->sink_->position(), std::errc::state_not_recoverable);
  }
  const auto flush_error = detail::as_log_error(impl_->sink_->flush());
  if (flush_error) {
    impl_->poisoned_ = true;
    return flush_error;
  }
  const auto sync_error = detail::as_log_error(impl_->sink_->sync());
  if (sync_error) {
    impl_->poisoned_ = true;
  }
  return sync_error;
}

const MultiInstrumentEngine& LoggedEngine::engine() const noexcept { return *impl_->engine_; }

bool LoggedEngine::poisoned() const noexcept { return impl_->poisoned_; }

Durability LoggedEngine::durability() const noexcept { return impl_->options_.durability; }

LogId LoggedEngine::log_id() const noexcept { return impl_->log_id_; }

std::uint64_t LoggedEngine::log_offset() const noexcept { return impl_->sink_->position(); }

}  // namespace atlaslob::persistence
