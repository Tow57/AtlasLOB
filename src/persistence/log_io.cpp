#include "log_io.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace atlaslob::persistence::detail {
namespace {

RemoveFileHook remove_file_hook{};

[[nodiscard]] std::error_code errno_code() noexcept { return {errno, std::generic_category()}; }

[[nodiscard]] LogIoFailure failure(LogIoOperation operation, std::uint64_t offset,
                                   std::error_code error) noexcept {
  return {
      .operation = operation,
      .offset = offset,
      .system_error = error,
  };
}

[[nodiscard]] LogIoFailure bad_descriptor(LogIoOperation operation, std::uint64_t offset) noexcept {
  return failure(operation, offset, std::make_error_code(std::errc::bad_file_descriptor));
}

[[nodiscard]] int close_descriptor(int descriptor) noexcept {
#if defined(_WIN32)
  return ::_close(descriptor);
#else
  // POSIX leaves the descriptor state unspecified after EINTR. Retrying could
  // close a descriptor already recycled by another thread.
  return ::close(descriptor);
#endif
}

[[nodiscard]] LogIoFailure remove_partial_file(const std::filesystem::path& path) noexcept {
  std::error_code error;
  if (remove_file_hook != nullptr) {
    error = remove_file_hook(path);
  } else {
    static_cast<void>(std::filesystem::remove(path, error));
  }
  if (error) {
    return failure(LogIoOperation::remove_file, 0U, error);
  }
  return {};
}

[[nodiscard]] LogWriteResult write_descriptor(int descriptor, std::uint64_t& offset,
                                              std::span<const std::byte> bytes) noexcept {
  if (descriptor < 0) {
    return {
        .failure = bad_descriptor(LogIoOperation::write, offset),
    };
  }
  if (bytes.empty()) {
    return {};
  }

#if defined(_WIN32)
  const auto requested =
      std::min<std::size_t>(bytes.size(), std::numeric_limits<unsigned int>::max());
  int result{};
  do {
    result = ::_write(descriptor, bytes.data(), static_cast<unsigned int>(requested));
  } while (result < 0 && errno == EINTR);
#else
  const auto requested = std::min<std::size_t>(
      bytes.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
  ssize_t result{};
  do {
    result = ::write(descriptor, bytes.data(), requested);
  } while (result < 0 && errno == EINTR);
#endif

  if (result < 0) {
    return {
        .failure = failure(LogIoOperation::write, offset, errno_code()),
    };
  }
  if (result == 0) {
    return {
        .failure =
            failure(LogIoOperation::write, offset, std::make_error_code(std::errc::io_error)),
    };
  }

  const auto written = static_cast<std::size_t>(result);
  if (written > std::numeric_limits<std::uint64_t>::max() - offset) {
    return {
        .failure = failure(LogIoOperation::write, offset,
                           std::make_error_code(std::errc::value_too_large)),
    };
  }
  offset += static_cast<std::uint64_t>(written);
  return {
      .bytes_written = written,
  };
}

[[nodiscard]] LogIoFailure flush_descriptor(int descriptor, std::uint64_t offset) noexcept {
  if (descriptor < 0) {
    return bad_descriptor(LogIoOperation::flush, offset);
  }
  // Writes go directly to the descriptor, so there is no userspace buffer to
  // drain. sync_descriptor() is the explicit durability boundary.
  return {};
}

[[nodiscard]] LogIoFailure sync_descriptor(int descriptor, std::uint64_t offset) noexcept {
  if (descriptor < 0) {
    return bad_descriptor(LogIoOperation::sync, offset);
  }
#if defined(_WIN32)
  if (::_commit(descriptor) != 0) {
    return failure(LogIoOperation::sync, offset, errno_code());
  }
#else
  int result{};
  do {
    result = ::fsync(descriptor);
  } while (result != 0 && errno == EINTR);
  if (result != 0) {
    return failure(LogIoOperation::sync, offset, errno_code());
  }
#endif
  return {};
}

}  // namespace

void set_remove_file_hook_for_testing(RemoveFileHook hook) noexcept { remove_file_hook = hook; }

NativeFileSource::~NativeFileSource() {
  if (descriptor_ >= 0) {
    static_cast<void>(close_descriptor(descriptor_));
  }
}

LogExtentResult NativeFileSource::extent() noexcept {
  if (descriptor_ < 0) {
    return {
        .failure = bad_descriptor(LogIoOperation::inspect_extent, 0U),
    };
  }
#if defined(_WIN32)
  struct ::_stati64 status {};
  if (::_fstati64(descriptor_, &status) != 0) {
    return {
        .failure = failure(LogIoOperation::inspect_extent, 0U, errno_code()),
    };
  }
  if (status.st_size < 0) {
    return {
        .failure = failure(LogIoOperation::inspect_extent, 0U,
                           std::make_error_code(std::errc::value_too_large)),
    };
  }
#else
  struct ::stat status {};
  if (::fstat(descriptor_, &status) != 0) {
    return {
        .failure = failure(LogIoOperation::inspect_extent, 0U, errno_code()),
    };
  }
  if (status.st_size < 0) {
    return {
        .failure = failure(LogIoOperation::inspect_extent, 0U,
                           std::make_error_code(std::errc::value_too_large)),
    };
  }
#endif
  return {
      .extent = static_cast<std::uint64_t>(status.st_size),
  };
}

LogReadResult NativeFileSource::read_at(std::uint64_t offset,
                                        std::span<std::byte> destination) noexcept {
  if (descriptor_ < 0) {
    return {
        .failure = bad_descriptor(LogIoOperation::read, offset),
    };
  }
  if (destination.empty()) {
    return {};
  }

#if defined(_WIN32)
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<__int64>::max())) {
    return {
        .failure =
            failure(LogIoOperation::read, offset, std::make_error_code(std::errc::value_too_large)),
    };
  }
  if (::_lseeki64(descriptor_, static_cast<__int64>(offset), SEEK_SET) < 0) {
    return {
        .failure = failure(LogIoOperation::read, offset, errno_code()),
    };
  }
  const auto requested =
      std::min<std::size_t>(destination.size(), std::numeric_limits<unsigned int>::max());
  int result{};
  do {
    result = ::_read(descriptor_, destination.data(), static_cast<unsigned int>(requested));
  } while (result < 0 && errno == EINTR);
#else
  if constexpr (std::numeric_limits<off_t>::is_signed) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
      return {
          .failure = failure(LogIoOperation::read, offset,
                             std::make_error_code(std::errc::value_too_large)),
      };
    }
  }
  const auto requested = std::min<std::size_t>(
      destination.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
  ssize_t result{};
  do {
    result = ::pread(descriptor_, destination.data(), requested, static_cast<off_t>(offset));
  } while (result < 0 && errno == EINTR);
#endif

  if (result < 0) {
    return {
        .failure = failure(LogIoOperation::read, offset, errno_code()),
    };
  }
  return {
      .bytes_read = static_cast<std::size_t>(result),
      .eof = result == 0,
  };
}

NativeSourceOpenResult open_native_log_source(const std::filesystem::path& path) {
  int descriptor{-1};
#if defined(_WIN32)
  const auto open_error =
      ::_wsopen_s(&descriptor, path.c_str(), _O_BINARY | _O_RDONLY, _SH_DENYNO, _S_IREAD);
  if (open_error != 0) {
    return {
        .source = nullptr,
        .failure = failure(LogIoOperation::open_source, 0U,
                           {static_cast<int>(open_error), std::generic_category()}),
    };
  }
#else
  descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return {
        .source = nullptr,
        .failure = failure(LogIoOperation::open_source, 0U, errno_code()),
    };
  }
#endif
  return {
      .source = std::unique_ptr<NativeFileSource>{new NativeFileSource{descriptor}},
  };
}

NativeNewFileSink::~NativeNewFileSink() { static_cast<void>(abandon()); }

LogWriteResult NativeNewFileSink::write(std::span<const std::byte> bytes) noexcept {
  return write_descriptor(descriptor_, offset_, bytes);
}

LogIoFailure NativeNewFileSink::flush() noexcept { return flush_descriptor(descriptor_, offset_); }

LogIoFailure NativeNewFileSink::sync() noexcept { return sync_descriptor(descriptor_, offset_); }

LogIoFailure NativeNewFileSink::commit() noexcept {
  if (committed_) {
    return {};
  }
  if (descriptor_ < 0) {
    return bad_descriptor(LogIoOperation::close, offset_);
  }

  const auto flush_failure = flush();
  if (flush_failure) {
    const auto cleanup_failure = abandon();
    return cleanup_failure ? cleanup_failure : flush_failure;
  }
  const auto sync_failure = sync();
  if (sync_failure) {
    const auto cleanup_failure = abandon();
    return cleanup_failure ? cleanup_failure : sync_failure;
  }

  const int descriptor = std::exchange(descriptor_, -1);
  if (close_descriptor(descriptor) != 0) {
    const auto close_failure = failure(LogIoOperation::close, offset_, errno_code());
    const auto cleanup_failure = remove_partial_file(path_);
    return cleanup_failure ? cleanup_failure : close_failure;
  }
  committed_ = true;
  return {};
}

LogIoFailure NativeNewFileSink::abandon() noexcept {
  LogIoFailure close_failure;
  if (descriptor_ >= 0) {
    const int descriptor = std::exchange(descriptor_, -1);
    if (close_descriptor(descriptor) != 0) {
      close_failure = failure(LogIoOperation::close, offset_, errno_code());
    }
  }
  if (!committed_) {
    const auto cleanup_failure = remove_partial_file(path_);
    if (cleanup_failure) {
      return cleanup_failure;
    }
  }
  return close_failure;
}

NativeSinkOpenResult open_native_new_log_sink(const std::filesystem::path& path) {
  int descriptor{-1};
#if defined(_WIN32)
  const auto open_error =
      ::_wsopen_s(&descriptor, path.c_str(), _O_BINARY | _O_WRONLY | _O_CREAT | _O_EXCL, _SH_DENYRW,
                  _S_IREAD | _S_IWRITE);
  if (open_error != 0) {
    return {
        .sink = nullptr,
        .failure = failure(LogIoOperation::open_destination, 0U,
                           {static_cast<int>(open_error), std::generic_category()}),
    };
  }
#else
  descriptor =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (descriptor < 0) {
    return {
        .sink = nullptr,
        .failure = failure(LogIoOperation::open_destination, 0U, errno_code()),
    };
  }
#endif
  return {
      .sink = std::unique_ptr<NativeNewFileSink>{new NativeNewFileSink{descriptor, path}},
  };
}

NativeAppendFileSink::~NativeAppendFileSink() {
  if (stream_ != nullptr) {
    std::FILE* const stream = std::exchange(stream_, nullptr);
    static_cast<void>(std::fclose(stream));
  }
  if (!header_published_) {
    static_cast<void>(remove_partial_file(path_));
  }
}

LogWriteResult NativeAppendFileSink::write(std::span<const std::byte> bytes) noexcept {
  if (stream_ == nullptr) {
    return {
        .failure = bad_descriptor(LogIoOperation::write, offset_),
    };
  }
  if (bytes.empty()) {
    return {};
  }

  errno = 0;
  const auto written = std::fwrite(bytes.data(), 1U, bytes.size(), stream_);
  if (written == 0U) {
    const auto error = errno == 0 ? std::make_error_code(std::errc::io_error) : errno_code();
    return {
        .failure = failure(LogIoOperation::write, offset_, error),
    };
  }
  if (written > std::numeric_limits<std::uint64_t>::max() - offset_) {
    return {
        .failure = failure(LogIoOperation::write, offset_,
                           std::make_error_code(std::errc::value_too_large)),
    };
  }
  offset_ += static_cast<std::uint64_t>(written);
  return {
      .bytes_written = written,
  };
}

LogIoFailure NativeAppendFileSink::flush() noexcept {
  if (stream_ == nullptr) {
    return bad_descriptor(LogIoOperation::flush, offset_);
  }
  if (std::fflush(stream_) != 0) {
    return failure(LogIoOperation::flush, offset_, errno_code());
  }
  return {};
}

LogIoFailure NativeAppendFileSink::sync() noexcept {
  const auto flush_failure = flush();
  if (flush_failure) {
    return flush_failure;
  }
#if defined(_WIN32)
  const int descriptor = ::_fileno(stream_);
#else
  const int descriptor = ::fileno(stream_);
#endif
  return sync_descriptor(descriptor, offset_);
}

LogIoFailure NativeAppendFileSink::close() noexcept {
  if (stream_ == nullptr) {
    return bad_descriptor(LogIoOperation::close, offset_);
  }
  std::FILE* const stream = std::exchange(stream_, nullptr);
  if (std::fclose(stream) != 0) {
    return failure(LogIoOperation::close, offset_, errno_code());
  }
  return {};
}

NativeAppendSinkOpenResult open_native_append_log_sink(const std::filesystem::path& path) {
  int descriptor{-1};
#if defined(_WIN32)
  const auto open_error =
      ::_wsopen_s(&descriptor, path.c_str(), _O_BINARY | _O_WRONLY | _O_CREAT | _O_EXCL, _SH_DENYRW,
                  _S_IREAD | _S_IWRITE);
  if (open_error != 0) {
    return {
        .sink = nullptr,
        .failure = failure(LogIoOperation::open_destination, 0U,
                           {static_cast<int>(open_error), std::generic_category()}),
    };
  }
#else
  descriptor =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (descriptor < 0) {
    return {
        .sink = nullptr,
        .failure = failure(LogIoOperation::open_destination, 0U, errno_code()),
    };
  }
#endif
  std::FILE* stream{};
#if defined(_WIN32)
  stream = ::_fdopen(descriptor, "wb");
#else
  stream = ::fdopen(descriptor, "wb");
#endif
  if (stream == nullptr) {
    const auto stream_error = errno_code();
    static_cast<void>(close_descriptor(descriptor));
    static_cast<void>(remove_partial_file(path));
    return {
        .sink = nullptr,
        .failure = failure(LogIoOperation::open_destination, 0U, stream_error),
    };
  }
  return {
      .sink = std::unique_ptr<NativeAppendFileSink>{new NativeAppendFileSink{stream, path}},
  };
}

}  // namespace atlaslob::persistence::detail
