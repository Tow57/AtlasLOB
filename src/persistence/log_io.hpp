#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <system_error>
#include <utility>

namespace atlaslob::persistence::detail {

inline constexpr std::size_t default_log_io_chunk_bytes = 64U * 1024U;
using RemoveFileHook = std::error_code (*)(const std::filesystem::path&) noexcept;

// Private fault-injection seam; never installed as public API.
void set_remove_file_hook_for_testing(RemoveFileHook hook) noexcept;

enum class LogIoOperation : std::uint8_t {
  none = 0,
  open_source = 1,
  open_destination = 2,
  inspect_extent = 3,
  read = 4,
  write = 5,
  flush = 6,
  sync = 7,
  close = 8,
  remove_file = 9,
  publish_file = 10,
  sync_directory = 11,
};

struct LogIoFailure final {
  LogIoOperation operation{LogIoOperation::none};
  std::uint64_t offset{};
  std::error_code system_error{};

  [[nodiscard]] bool failed() const noexcept {
    return operation != LogIoOperation::none || static_cast<bool>(system_error);
  }

  [[nodiscard]] explicit operator bool() const noexcept { return failed(); }

  bool operator==(const LogIoFailure&) const = default;
};

struct LogReadResult final {
  std::size_t bytes_read{};
  bool eof{};
  LogIoFailure failure{};

  [[nodiscard]] bool succeeded() const noexcept { return !failure; }
  [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
};

struct LogExtentResult final {
  std::uint64_t extent{};
  LogIoFailure failure{};

  [[nodiscard]] bool succeeded() const noexcept { return !failure; }
  [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
};

struct LogWriteResult final {
  std::size_t bytes_written{};
  LogIoFailure failure{};

  [[nodiscard]] bool succeeded() const noexcept { return !failure; }
  [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
};

// Scriptable scanner seam. Implementations may return short successful reads;
// scanners must continue until the requested bytes are filled, EOF is observed,
// or a structured failure is returned.
class LogSource {
 public:
  LogSource() = default;
  LogSource(const LogSource&) = delete;
  LogSource& operator=(const LogSource&) = delete;
  LogSource(LogSource&&) = delete;
  LogSource& operator=(LogSource&&) = delete;
  virtual ~LogSource() = default;

  [[nodiscard]] virtual LogExtentResult extent() noexcept = 0;
  [[nodiscard]] virtual LogReadResult read_at(std::uint64_t offset,
                                              std::span<std::byte> destination) noexcept = 0;
};

// Scriptable repair seam. Writes are sequential and may complete partially.
class LogSink {
 public:
  LogSink() = default;
  LogSink(const LogSink&) = delete;
  LogSink& operator=(const LogSink&) = delete;
  LogSink(LogSink&&) = delete;
  LogSink& operator=(LogSink&&) = delete;
  virtual ~LogSink() = default;

  [[nodiscard]] virtual LogWriteResult write(std::span<const std::byte> bytes) noexcept = 0;
  [[nodiscard]] virtual LogIoFailure flush() noexcept = 0;
  [[nodiscard]] virtual LogIoFailure sync() noexcept = 0;
  [[nodiscard]] virtual std::uint64_t position() const noexcept = 0;
  [[nodiscard]] std::uint64_t bytes_written() const noexcept { return position(); }
};

struct NativeSourceOpenResult;
struct NativeSinkOpenResult;
struct NativeAppendSinkOpenResult;

class NativeFileSource final : public LogSource {
 public:
  ~NativeFileSource() override;

  [[nodiscard]] LogExtentResult extent() noexcept override;
  [[nodiscard]] LogReadResult read_at(std::uint64_t offset,
                                      std::span<std::byte> destination) noexcept override;

 private:
  friend struct NativeSourceOpenResult;
  friend NativeSourceOpenResult open_native_log_source(const std::filesystem::path& path);

  explicit NativeFileSource(int descriptor) noexcept : descriptor_{descriptor} {}

  int descriptor_{-1};
};

struct NativeSourceOpenResult final {
  std::unique_ptr<NativeFileSource> source;
  LogIoFailure failure{};

  [[nodiscard]] bool succeeded() const noexcept { return source != nullptr && !failure; }
  [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
};

// Opens an existing file without modifying it. Allocation failure propagates;
// operating-system failures are returned as structured I/O failures.
[[nodiscard]] NativeSourceOpenResult open_native_log_source(const std::filesystem::path& path);

class NativeNewFileSink final : public LogSink {
 public:
  ~NativeNewFileSink() override;

  [[nodiscard]] LogWriteResult write(std::span<const std::byte> bytes) noexcept override;
  [[nodiscard]] LogIoFailure flush() noexcept override;
  [[nodiscard]] LogIoFailure sync() noexcept override;
  [[nodiscard]] std::uint64_t position() const noexcept override { return offset_; }

  // Keeps the new file only after its bytes are durable and the descriptor is
  // closed successfully. A failed commit removes the partial destination.
  [[nodiscard]] LogIoFailure commit() noexcept;
  [[nodiscard]] LogIoFailure abandon() noexcept;

 private:
  friend struct NativeSinkOpenResult;
  friend NativeSinkOpenResult open_native_new_log_sink(const std::filesystem::path& path);

  NativeNewFileSink(int descriptor, std::filesystem::path path)
      : descriptor_{descriptor}, path_{std::move(path)} {}

  int descriptor_{-1};
  std::filesystem::path path_;
  std::uint64_t offset_{};
  bool committed_{};
};

struct NativeSinkOpenResult final {
  std::unique_ptr<NativeNewFileSink> sink;
  LogIoFailure failure{};

  [[nodiscard]] bool succeeded() const noexcept { return sink != nullptr && !failure; }
  [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
};

// Creates a brand-new destination with exclusive-create semantics. It never
// truncates an existing path. Unless commit() succeeds, ordinary destruction
// removes the partial new file.
[[nodiscard]] NativeSinkOpenResult open_native_new_log_sink(const std::filesystem::path& path);

struct NativeFilePublicationResult final {
  bool destination_visible{};
  bool source_visible{true};
  LogIoFailure failure{};

  [[nodiscard]] bool succeeded() const noexcept {
    return destination_visible && !source_visible && !failure;
  }
  [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
};

// Publishes a closed, synchronized new file at a different path without ever
// replacing an existing destination. The paths must share a directory. The
// visibility bits remain authoritative even when publication or directory
// synchronization fails.
[[nodiscard]] NativeFilePublicationResult publish_native_new_file_no_replace(
    const std::filesystem::path& source, const std::filesystem::path& destination);

// Uses the same private removal seam as NativeNewFileSink::abandon(). Callers
// can therefore report a surviving unpublished artifact deterministically.
[[nodiscard]] LogIoFailure remove_native_file(const std::filesystem::path& path) noexcept;

class NativeAppendFileSink final : public LogSink {
 public:
  ~NativeAppendFileSink() override;

  [[nodiscard]] LogWriteResult write(std::span<const std::byte> bytes) noexcept override;
  [[nodiscard]] LogIoFailure flush() noexcept override;
  [[nodiscard]] LogIoFailure sync() noexcept override;
  [[nodiscard]] std::uint64_t position() const noexcept override { return offset_; }

  // Call only after the complete header has been written and synced. Before
  // publication, destruction removes the incomplete new file. Afterwards the
  // append-only log remains on disk even if a later append fails.
  void mark_header_published() noexcept { header_published_ = true; }
  [[nodiscard]] bool header_published() const noexcept { return header_published_; }

  [[nodiscard]] LogIoFailure close() noexcept;

 private:
  friend struct NativeAppendSinkOpenResult;
  friend NativeAppendSinkOpenResult open_native_append_log_sink(const std::filesystem::path& path);
  friend NativeAppendSinkOpenResult open_native_existing_append_log_sink(
      const std::filesystem::path& path, std::uint64_t expected_extent);

  NativeAppendFileSink(std::FILE* stream, std::filesystem::path path,
                       std::uint64_t initial_offset = 0U, bool header_published = false) noexcept
      : stream_{stream},
        path_{std::move(path)},
        offset_{initial_offset},
        header_published_{header_published} {}

  std::FILE* stream_{};
  std::filesystem::path path_;
  std::uint64_t offset_{};
  bool header_published_{};
};

struct NativeAppendSinkOpenResult final {
  std::unique_ptr<NativeAppendFileSink> sink;
  LogIoFailure failure{};

  [[nodiscard]] bool succeeded() const noexcept { return sink != nullptr && !failure; }
  [[nodiscard]] explicit operator bool() const noexcept { return succeeded(); }
};

// Creates a brand-new append session atomically. The sink remains open across
// record writes; existing paths are never truncated.
[[nodiscard]] NativeAppendSinkOpenResult open_native_append_log_sink(
    const std::filesystem::path& path);

// Opens an existing authoritative log without creating or truncating it. The
// opened descriptor is append-only, its extent must still equal the validated
// recovery extent, and the sink is published from construction so no failure
// path can remove the existing log.
[[nodiscard]] NativeAppendSinkOpenResult open_native_existing_append_log_sink(
    const std::filesystem::path& path, std::uint64_t expected_extent);

}  // namespace atlaslob::persistence::detail
