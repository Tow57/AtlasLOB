#pragma once

#include <span>

#include "atlaslob/persistence/logged_engine.hpp"
#include "log_io.hpp"

namespace atlaslob::persistence::detail {

[[nodiscard]] LogError as_log_error(const LogIoFailure& failure) noexcept;

[[nodiscard]] LogError write_all(LogSink& sink, std::span<const std::uint8_t> bytes) noexcept;

[[nodiscard]] LogError apply_durability(LogSink& sink, Durability durability) noexcept;

// Shared production/test seam. A failed append or durability operation never
// commits the prepared token and permanently poisons the caller-owned session.
[[nodiscard]] LoggedSubmissionResult submit_logged(MultiInstrumentEngine& engine, LogSink& sink,
                                                   bool& poisoned, Durability durability,
                                                   CodecLimits limits,
                                                   const domain::Command& command);

}  // namespace atlaslob::persistence::detail
