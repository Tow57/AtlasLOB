#include "atlaslob/persistence/snapshot_store.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

#include "log_io.hpp"
#include "snapshot_codec.hpp"
#include "snapshot_store_internal.hpp"

namespace atlaslob::persistence {
namespace {

struct SnapshotBytes final {
  std::vector<std::uint8_t> bytes;
  SnapshotError error{};
};

struct AtomicPublication final {
  bool final_file_visible{};
  std::error_code error{};
};

std::atomic<std::uint64_t> temporary_counter{};
detail::SnapshotPublicationHook publication_hook{};

[[nodiscard]] SnapshotError snapshot_error(SnapshotErrorCategory category,
                                           std::uint64_t offset = 0U,
                                           std::error_code system_error = {}) noexcept {
  return {
      .category = category,
      .byte_offset = offset,
      .system_error = system_error,
  };
}

[[nodiscard]] SnapshotError io_error(const detail::LogIoFailure& failure) noexcept {
  return snapshot_error(SnapshotErrorCategory::io_failure, failure.offset, failure.system_error);
}

[[nodiscard]] SnapshotError injected_failure(detail::SnapshotPublicationStage stage,
                                             std::uint64_t offset) noexcept {
  return publication_hook == nullptr ? SnapshotError{} : publication_hook(stage, offset);
}

[[nodiscard]] SnapshotBytes read_snapshot_bytes(const std::filesystem::path& path,
                                                CodecLimits limits) {
  if (!limits.valid()) {
    return {
        .bytes = {},
        .error = snapshot_error(SnapshotErrorCategory::invalid_length),
    };
  }

  auto opened = detail::open_native_log_source(path);
  if (!opened) {
    return {
        .bytes = {},
        .error = io_error(opened.failure),
    };
  }
  const auto extent = opened.source->extent();
  if (!extent) {
    return {
        .bytes = {},
        .error = io_error(extent.failure),
    };
  }
  if (extent.extent > limits.max_snapshot_bytes ||
      extent.extent > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return {
        .bytes = {},
        .error = snapshot_error(SnapshotErrorCategory::excessive_length),
    };
  }

  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(extent.extent));
  std::size_t filled{};
  while (filled < bytes.size()) {
    auto destination = std::span<std::uint8_t>{bytes}.subspan(filled);
    auto read = opened.source->read_at(static_cast<std::uint64_t>(filled),
                                       std::as_writable_bytes(destination));
    if (!read) {
      return {
          .bytes = {},
          .error = io_error(read.failure),
      };
    }
    if (read.bytes_read == 0U || read.eof) {
      return {
          .bytes = {},
          .error =
              snapshot_error(SnapshotErrorCategory::io_failure, static_cast<std::uint64_t>(filled),
                             std::make_error_code(std::errc::state_not_recoverable)),
      };
    }
    filled += read.bytes_read;
  }

  const auto final_extent = opened.source->extent();
  if (!final_extent) {
    return {
        .bytes = {},
        .error = io_error(final_extent.failure),
    };
  }
  if (final_extent.extent != extent.extent) {
    return {
        .bytes = {},
        .error = snapshot_error(SnapshotErrorCategory::io_failure, extent.extent,
                                std::make_error_code(std::errc::state_not_recoverable)),
    };
  }
  return {
      .bytes = std::move(bytes),
      .error = {},
  };
}

[[nodiscard]] SnapshotError write_all(detail::LogSink& sink,
                                      std::span<const std::uint8_t> bytes) noexcept {
  std::size_t written{};
  while (written < bytes.size()) {
    const auto remaining = std::as_bytes(bytes.subspan(written));
    const auto result = sink.write(remaining);
    if (!result) {
      return io_error(result.failure);
    }
    if (result.bytes_written == 0U || result.bytes_written > remaining.size()) {
      return snapshot_error(SnapshotErrorCategory::io_failure, sink.position(),
                            std::make_error_code(std::errc::io_error));
    }
    written += result.bytes_written;
  }
  return {};
}

[[nodiscard]] std::string padded_sequence(domain::Sequence sequence) {
  std::array<char, 20U> digits{};
  digits.fill('0');
  std::array<char, 32U> converted{};
  const auto [end, error] =
      std::to_chars(converted.data(), converted.data() + converted.size(), sequence.value());
  if (error != std::errc{}) {
    throw std::length_error{"snapshot sequence cannot be formatted"};
  }
  const auto count = static_cast<std::size_t>(end - converted.data());
  if (count > digits.size()) {
    throw std::length_error{"snapshot sequence exceeds its canonical filename width"};
  }
  std::copy(converted.data(), end, digits.end() - static_cast<std::ptrdiff_t>(count));
  return {digits.data(), digits.size()};
}

[[nodiscard]] std::filesystem::path final_snapshot_path(const std::filesystem::path& directory,
                                                        LogId log_id, domain::Sequence sequence) {
  return directory / ("atlaslob-" + log_id.hex() + "-" + padded_sequence(sequence) + ".snapshot");
}

[[nodiscard]] std::filesystem::path next_temporary_path(const std::filesystem::path& final_path) {
  const auto nonce = temporary_counter.fetch_add(1U, std::memory_order_relaxed) + 1U;
  return final_path.parent_path() /
         ("." + final_path.filename().string() + ".tmp-" + std::to_string(nonce));
}

#if !defined(_WIN32)
[[nodiscard]] std::error_code errno_error() noexcept { return {errno, std::generic_category()}; }

[[nodiscard]] std::error_code synchronize_directory(
    const std::filesystem::path& directory) noexcept {
#if defined(O_DIRECTORY)
  int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
#else
  int descriptor = ::open(directory.c_str(), O_RDONLY);
#endif
  if (descriptor < 0) {
    return errno_error();
  }
  int result{};
  do {
    result = ::fsync(descriptor);
  } while (result != 0 && errno == EINTR);
  const auto sync_error = result == 0 ? std::error_code{} : errno_error();
  const int close_result = ::close(descriptor);
  if (sync_error) {
    return sync_error;
  }
  return close_result == 0 ? std::error_code{} : errno_error();
}
#endif

[[nodiscard]] AtomicPublication atomically_publish(const std::filesystem::path& temporary_path,
                                                   const std::filesystem::path& final_path,
                                                   const std::filesystem::path& parent_directory) {
#if defined(_WIN32)
  static_cast<void>(parent_directory);
  if (::MoveFileExW(temporary_path.c_str(), final_path.c_str(), MOVEFILE_WRITE_THROUGH) == 0) {
    return {
        .final_file_visible = false,
        .error = {static_cast<int>(::GetLastError()), std::system_category()},
    };
  }
  return {
      .final_file_visible = true,
      .error = {},
  };
#else
  bool published{};
#if defined(__linux__) && defined(SYS_renameat2)
  errno = 0;
  constexpr unsigned int rename_no_replace{1U};
  const auto rename_result = ::syscall(SYS_renameat2, AT_FDCWD, temporary_path.c_str(), AT_FDCWD,
                                       final_path.c_str(), rename_no_replace);
  if (rename_result == 0) {
    published = true;
  } else if (errno != ENOSYS && errno != EINVAL) {
    return {
        .final_file_visible = false,
        .error = errno_error(),
    };
  }
#endif
  if (!published) {
    if (::link(temporary_path.c_str(), final_path.c_str()) != 0) {
      return {
          .final_file_visible = false,
          .error = errno_error(),
      };
    }
    published = true;
    if (::unlink(temporary_path.c_str()) != 0) {
      return {
          .final_file_visible = true,
          .error = errno_error(),
      };
    }
  }
  const auto directory_error = synchronize_directory(parent_directory);
  return {
      .final_file_visible = published,
      .error = directory_error,
  };
#endif
}

[[nodiscard]] SnapshotError remove_temporary(const std::filesystem::path& path) {
  if (const auto injected = injected_failure(detail::SnapshotPublicationStage::cleanup, 0U);
      injected) {
    return injected;
  }
  std::error_code error;
  static_cast<void>(std::filesystem::remove(path, error));
  return error ? snapshot_error(SnapshotErrorCategory::io_failure, 0U, error) : SnapshotError{};
}

}  // namespace

SnapshotInspectionReport inspect_snapshot(const std::filesystem::path& path, CodecLimits limits) {
  auto read = read_snapshot_bytes(path, limits);
  if (read.error) {
    return {
        .snapshot = std::nullopt,
        .input_bytes = 0U,
        .error = read.error,
    };
  }
  const auto input_bytes = static_cast<std::uint64_t>(read.bytes.size());
  auto decoded = detail::decode_snapshot(read.bytes, limits);
  return {
      .snapshot = std::move(decoded.value),
      .input_bytes = input_bytes,
      .error = decoded.error,
  };
}

namespace detail {

void set_snapshot_publication_hook_for_testing(SnapshotPublicationHook hook) noexcept {
  publication_hook = hook;
}

SnapshotPublicationResult publish_snapshot(const std::filesystem::path& directory,
                                           const MultiInstrumentEngine& engine, LogId log_id,
                                           std::uint64_t covered_log_byte_offset,
                                           CodecLimits limits) {
  const auto state = engine.snapshot();
  auto snapshot = make_snapshot_file(state, log_id, covered_log_byte_offset);
  if (!snapshot) {
    return {
        .path = {},
        .covered_sequence = state.last_sequence,
        .covered_log_byte_offset = covered_log_byte_offset,
        .encoded_bytes = 0U,
        .final_file_visible = false,
        .error = snapshot.error,
    };
  }
  auto encoded = encode_snapshot(*snapshot.value, limits);
  if (!encoded) {
    return {
        .path = {},
        .covered_sequence = state.last_sequence,
        .covered_log_byte_offset = covered_log_byte_offset,
        .encoded_bytes = 0U,
        .final_file_visible = false,
        .error = encoded.error,
    };
  }

  std::error_code directory_error;
  if (!std::filesystem::is_directory(directory, directory_error)) {
    if (!directory_error) {
      directory_error = std::make_error_code(std::errc::not_a_directory);
    }
    return {
        .path = {},
        .covered_sequence = state.last_sequence,
        .covered_log_byte_offset = covered_log_byte_offset,
        .encoded_bytes = static_cast<std::uint64_t>(encoded.value->size()),
        .final_file_visible = false,
        .error = snapshot_error(SnapshotErrorCategory::io_failure, 0U, directory_error),
    };
  }

  const auto final_path = final_snapshot_path(directory, log_id, state.last_sequence);
  const auto parent_directory = final_path.parent_path();
  if (const auto injected = injected_failure(SnapshotPublicationStage::create_temporary, 0U);
      injected) {
    return {
        .path = final_path,
        .covered_sequence = state.last_sequence,
        .covered_log_byte_offset = covered_log_byte_offset,
        .encoded_bytes = static_cast<std::uint64_t>(encoded.value->size()),
        .final_file_visible = false,
        .error = injected,
    };
  }
  std::filesystem::path temporary_path;
  NativeSinkOpenResult opened;
  constexpr std::size_t maximum_attempts{1024U};
  for (std::size_t attempt = 0U; attempt < maximum_attempts; ++attempt) {
    temporary_path = next_temporary_path(final_path);
    opened = open_native_new_log_sink(temporary_path);
    if (opened) {
      break;
    }
    if (opened.failure.system_error != std::errc::file_exists) {
      return {
          .path = final_path,
          .covered_sequence = state.last_sequence,
          .covered_log_byte_offset = covered_log_byte_offset,
          .encoded_bytes = static_cast<std::uint64_t>(encoded.value->size()),
          .final_file_visible = false,
          .error = io_error(opened.failure),
      };
    }
  }
  if (!opened) {
    return {
        .path = final_path,
        .covered_sequence = state.last_sequence,
        .covered_log_byte_offset = covered_log_byte_offset,
        .encoded_bytes = static_cast<std::uint64_t>(encoded.value->size()),
        .final_file_visible = false,
        .error = snapshot_error(SnapshotErrorCategory::io_failure, 0U,
                                std::make_error_code(std::errc::file_exists)),
    };
  }

  auto* sink = opened.sink.get();
  if (const auto injected = injected_failure(SnapshotPublicationStage::write, sink->position());
      injected) {
    const auto cleanup_error = sink->abandon();
    return {
        .path = final_path,
        .covered_sequence = state.last_sequence,
        .covered_log_byte_offset = covered_log_byte_offset,
        .encoded_bytes = static_cast<std::uint64_t>(encoded.value->size()),
        .final_file_visible = false,
        .error = cleanup_error ? io_error(cleanup_error) : injected,
    };
  }
  auto write_error = write_all(*sink, *encoded.value);
  if (write_error) {
    const auto cleanup_error = sink->abandon();
    if (cleanup_error) {
      write_error = io_error(cleanup_error);
    }
    return {
        .path = final_path,
        .covered_sequence = state.last_sequence,
        .covered_log_byte_offset = covered_log_byte_offset,
        .encoded_bytes = static_cast<std::uint64_t>(encoded.value->size()),
        .final_file_visible = false,
        .error = write_error,
    };
  }
  for (const auto stage : {SnapshotPublicationStage::flush, SnapshotPublicationStage::sync,
                           SnapshotPublicationStage::close}) {
    if (const auto injected = injected_failure(stage, sink->position()); injected) {
      const auto cleanup_error = sink->abandon();
      return {
          .path = final_path,
          .covered_sequence = state.last_sequence,
          .covered_log_byte_offset = covered_log_byte_offset,
          .encoded_bytes = static_cast<std::uint64_t>(encoded.value->size()),
          .final_file_visible = false,
          .error = cleanup_error ? io_error(cleanup_error) : injected,
      };
    }
  }
  const auto commit_error = sink->commit();
  if (commit_error) {
    return {
        .path = final_path,
        .covered_sequence = state.last_sequence,
        .covered_log_byte_offset = covered_log_byte_offset,
        .encoded_bytes = static_cast<std::uint64_t>(encoded.value->size()),
        .final_file_visible = false,
        .error = io_error(commit_error),
    };
  }

  if (const auto injected = injected_failure(SnapshotPublicationStage::reread,
                                             static_cast<std::uint64_t>(encoded.value->size()));
      injected) {
    auto failure = injected;
    if (const auto cleanup_error = remove_temporary(temporary_path); cleanup_error) {
      failure = cleanup_error;
    }
    return {
        .path = final_path,
        .covered_sequence = state.last_sequence,
        .covered_log_byte_offset = covered_log_byte_offset,
        .encoded_bytes = static_cast<std::uint64_t>(encoded.value->size()),
        .final_file_visible = false,
        .error = failure,
    };
  }
  const auto verified = read_snapshot_bytes(temporary_path, limits);
  if (verified.error || verified.bytes != *encoded.value) {
    auto verification_error = verified.error
                                  ? verified.error
                                  : snapshot_error(SnapshotErrorCategory::state_digest_mismatch);
    const auto cleanup_error = remove_temporary(temporary_path);
    if (cleanup_error) {
      verification_error = cleanup_error;
    }
    return {
        .path = final_path,
        .covered_sequence = state.last_sequence,
        .covered_log_byte_offset = covered_log_byte_offset,
        .encoded_bytes = static_cast<std::uint64_t>(encoded.value->size()),
        .final_file_visible = false,
        .error = verification_error,
    };
  }
  if (const auto injected = injected_failure(SnapshotPublicationStage::verify,
                                             static_cast<std::uint64_t>(verified.bytes.size()));
      injected) {
    auto failure = injected;
    if (const auto cleanup_error = remove_temporary(temporary_path); cleanup_error) {
      failure = cleanup_error;
    }
    return {
        .path = final_path,
        .covered_sequence = state.last_sequence,
        .covered_log_byte_offset = covered_log_byte_offset,
        .encoded_bytes = static_cast<std::uint64_t>(encoded.value->size()),
        .final_file_visible = false,
        .error = failure,
    };
  }
  const auto decoded = decode_snapshot(verified.bytes, limits);
  if (!decoded || *decoded.value != *snapshot.value) {
    auto verification_error = decoded.error
                                  ? decoded.error
                                  : snapshot_error(SnapshotErrorCategory::state_digest_mismatch);
    const auto cleanup_error = remove_temporary(temporary_path);
    if (cleanup_error) {
      verification_error = cleanup_error;
    }
    return {
        .path = final_path,
        .covered_sequence = state.last_sequence,
        .covered_log_byte_offset = covered_log_byte_offset,
        .encoded_bytes = static_cast<std::uint64_t>(encoded.value->size()),
        .final_file_visible = false,
        .error = verification_error,
    };
  }

  if (const auto injected = injected_failure(SnapshotPublicationStage::rename,
                                             static_cast<std::uint64_t>(encoded.value->size()));
      injected) {
    auto failure = injected;
    if (const auto cleanup_error = remove_temporary(temporary_path); cleanup_error) {
      failure = cleanup_error;
    }
    return {
        .path = final_path,
        .covered_sequence = state.last_sequence,
        .covered_log_byte_offset = covered_log_byte_offset,
        .encoded_bytes = static_cast<std::uint64_t>(encoded.value->size()),
        .final_file_visible = false,
        .error = failure,
    };
  }
  const auto publication = atomically_publish(temporary_path, final_path, parent_directory);
  if (publication.error) {
    auto publication_error =
        snapshot_error(SnapshotErrorCategory::io_failure, 0U, publication.error);
    if (!publication.final_file_visible) {
      const auto cleanup_error = remove_temporary(temporary_path);
      if (cleanup_error) {
        publication_error = cleanup_error;
      }
    }
    return {
        .path = final_path,
        .covered_sequence = state.last_sequence,
        .covered_log_byte_offset = covered_log_byte_offset,
        .encoded_bytes = static_cast<std::uint64_t>(encoded.value->size()),
        .final_file_visible = publication.final_file_visible,
        .error = publication_error,
    };
  }
  return {
      .path = final_path,
      .covered_sequence = state.last_sequence,
      .covered_log_byte_offset = covered_log_byte_offset,
      .encoded_bytes = static_cast<std::uint64_t>(encoded.value->size()),
      .final_file_visible = true,
      .error = {},
  };
}

}  // namespace detail
}  // namespace atlaslob::persistence
