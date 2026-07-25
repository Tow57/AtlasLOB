#include "log_scanner.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "../utility/sha256.hpp"
#include "command_log_codec.hpp"

namespace atlaslob::persistence::detail {
namespace {

inline constexpr std::size_t header_length_field_end{20U};
inline constexpr std::size_t record_total_length_bytes{4U};
inline constexpr std::size_t record_sequence_offset{12U};

[[nodiscard]] std::uint32_t decode_u32(std::span<const std::uint8_t, 4U> bytes) noexcept {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

[[nodiscard]] LogIoFailure local_io_failure(LogIoOperation operation, std::uint64_t offset,
                                            std::errc error) noexcept {
  return {
      .operation = operation,
      .offset = offset,
      .system_error = std::make_error_code(error),
  };
}

[[nodiscard]] LogIoFailure read_exact(LogSource& source, std::uint64_t offset,
                                      std::span<std::uint8_t> destination,
                                      std::size_t chunk_bytes) noexcept {
  std::size_t completed = 0U;
  while (completed < destination.size()) {
    const auto requested = std::min(chunk_bytes, destination.size() - completed);
    auto writable = std::as_writable_bytes(destination.subspan(completed, requested));
    const auto current_offset = offset + static_cast<std::uint64_t>(completed);
    const auto read = source.read_at(current_offset, writable);
    if (!read) {
      return read.failure;
    }
    if (read.bytes_read == 0U || read.bytes_read > requested) {
      return local_io_failure(LogIoOperation::read, current_offset, std::errc::io_error);
    }
    completed += read.bytes_read;
  }
  return {};
}

[[nodiscard]] LogIoFailure write_exact(LogSink& sink, std::span<const std::uint8_t> source,
                                       std::size_t chunk_bytes) noexcept {
  std::size_t completed = 0U;
  while (completed < source.size()) {
    const auto requested = std::min(chunk_bytes, source.size() - completed);
    const auto bytes = std::as_bytes(source.subspan(completed, requested));
    const auto write = sink.write(bytes);
    if (!write) {
      return write.failure;
    }
    if (write.bytes_written == 0U || write.bytes_written > requested) {
      return local_io_failure(LogIoOperation::write, sink.position(), std::errc::io_error);
    }
    completed += write.bytes_written;
  }
  return {};
}

[[nodiscard]] LogError absolute_error(LogError error, std::uint64_t base) noexcept {
  if (error.byte_offset > std::numeric_limits<std::uint64_t>::max() - base) {
    error.category = LogErrorCategory::invalid_length;
    error.byte_offset = base;
    error.system_error.clear();
    return error;
  }
  error.byte_offset += base;
  return error;
}

void set_error(LogScanResult& result, LogScanTermination termination, LogErrorCategory category,
               std::uint64_t offset, std::error_code system_error = {}) noexcept {
  result.termination = termination;
  result.error = {
      .category = category,
      .byte_offset = offset,
      .system_error = system_error,
  };
}

void set_io_error(LogScanResult& result, const LogIoFailure& failure) noexcept {
  set_error(result, LogScanTermination::io_failure, LogErrorCategory::io_failure, failure.offset,
            failure.system_error);
}

void set_codec_corruption(LogScanResult& result, LogError error,
                          std::uint64_t frame_offset) noexcept {
  if (error.category == LogErrorCategory::truncated_final_record) {
    error.category = LogErrorCategory::invalid_length;
  }
  result.termination = LogScanTermination::corruption;
  result.error = absolute_error(error, frame_offset);
}

[[nodiscard]] bool read_available_prefix(LogSource& source, std::uint64_t offset,
                                         std::uint64_t available,
                                         std::span<std::uint8_t> destination,
                                         std::size_t chunk_bytes, LogScanResult& result) noexcept {
  if (available == 0U) {
    return true;
  }
  if (available > destination.size()) {
    set_error(result, LogScanTermination::corruption, LogErrorCategory::invalid_length, offset);
    return false;
  }
  const auto read_failure = read_exact(
      source, offset, destination.first(static_cast<std::size_t>(available)), chunk_bytes);
  if (read_failure) {
    set_io_error(result, read_failure);
    return false;
  }
  return true;
}

[[nodiscard]] LogScanResult scan_impl(LogSource& source, LogSink* sink, LogScanOptions options,
                                      LogScanVisitor* visitor) {
  LogScanResult result;
  if (!options.valid()) {
    set_error(result, LogScanTermination::corruption, LogErrorCategory::invalid_length, 0U);
    return result;
  }
  if (sink != nullptr && sink->position() != 0U) {
    set_error(result, LogScanTermination::io_failure, LogErrorCategory::io_failure,
              sink->position(), std::make_error_code(std::errc::invalid_argument));
    return result;
  }
  utility::Sha256 valid_prefix_hash;

  const auto extent = source.extent();
  if (!extent) {
    set_io_error(result, extent.failure);
    return result;
  }
  result.source_extent = extent.extent;

  std::array<std::uint8_t, command_log_header_fixed_prefix_bytes> header_prefix{};
  if (result.source_extent < header_length_field_end) {
    if (!read_available_prefix(source, 0U, result.source_extent, header_prefix,
                               options.read_chunk_bytes, result)) {
      return result;
    }
    set_error(result, LogScanTermination::corruption, LogErrorCategory::invalid_length,
              result.source_extent);
    return result;
  }

  auto read_failure =
      read_exact(source, 0U, std::span<std::uint8_t>{header_prefix}.first(header_length_field_end),
                 options.read_chunk_bytes);
  if (read_failure) {
    set_io_error(result, read_failure);
    return result;
  }
  const auto declared_header_length =
      decode_u32(std::span<const std::uint8_t, 4U>{header_prefix.data() + 16U, 4U});
  if (declared_header_length < command_log_header_fixed_bytes) {
    set_error(result, LogScanTermination::corruption, LogErrorCategory::invalid_length, 16U);
    return result;
  }
  if (declared_header_length > options.codec_limits.max_header_bytes ||
      declared_header_length > default_max_log_header_bytes) {
    set_error(result, LogScanTermination::corruption, LogErrorCategory::excessive_length, 16U);
    return result;
  }

  if (result.source_extent < command_log_header_fixed_prefix_bytes) {
    const auto remaining = result.source_extent - header_length_field_end;
    if (!read_available_prefix(
            source, header_length_field_end, remaining,
            std::span<std::uint8_t>{header_prefix}.subspan(header_length_field_end),
            options.read_chunk_bytes, result)) {
      return result;
    }
    set_error(result, LogScanTermination::corruption, LogErrorCategory::invalid_length,
              result.source_extent);
    return result;
  }

  read_failure = read_exact(source, header_length_field_end,
                            std::span<std::uint8_t>{header_prefix}.subspan(header_length_field_end),
                            options.read_chunk_bytes);
  if (read_failure) {
    set_io_error(result, read_failure);
    return result;
  }
  const auto inspected_header = inspect_log_header_length(header_prefix, options.codec_limits);
  if (!inspected_header) {
    set_codec_corruption(result, inspected_header.error, 0U);
    return result;
  }
  if (*inspected_header.value != declared_header_length) {
    set_error(result, LogScanTermination::corruption, LogErrorCategory::invalid_length, 16U);
    return result;
  }

  std::vector<std::uint8_t> header_bytes(declared_header_length);
  std::copy(header_prefix.begin(), header_prefix.end(), header_bytes.begin());
  if (result.source_extent < declared_header_length) {
    const auto available = result.source_extent - command_log_header_fixed_prefix_bytes;
    if (!read_available_prefix(
            source, command_log_header_fixed_prefix_bytes, available,
            std::span<std::uint8_t>{header_bytes}.subspan(command_log_header_fixed_prefix_bytes),
            options.read_chunk_bytes, result)) {
      return result;
    }
    set_error(result, LogScanTermination::corruption, LogErrorCategory::invalid_length,
              result.source_extent);
    return result;
  }

  read_failure = read_exact(
      source, command_log_header_fixed_prefix_bytes,
      std::span<std::uint8_t>{header_bytes}.subspan(command_log_header_fixed_prefix_bytes),
      options.read_chunk_bytes);
  if (read_failure) {
    set_io_error(result, read_failure);
    return result;
  }

  auto decoded_header = decode_log_header(header_bytes, options.codec_limits);
  if (!decoded_header) {
    set_codec_corruption(result, decoded_header.error, 0U);
    return result;
  }
  result.header = std::move(*decoded_header.value);
  result.header_end_offset = declared_header_length;
  result.valid_end_offset = declared_header_length;
  result.next_sequence = result.header->first_sequence;
  valid_prefix_hash.update(header_bytes);

  if (visitor != nullptr) {
    visitor->on_header(*result.header, header_bytes);
  }
  if (sink != nullptr) {
    const auto write_failure = write_exact(*sink, header_bytes, options.read_chunk_bytes);
    if (write_failure) {
      set_io_error(result, write_failure);
      return result;
    }
  }

  std::uint64_t frame_offset = declared_header_length;
  while (frame_offset < result.source_extent) {
    const auto remaining = result.source_extent - frame_offset;
    std::array<std::uint8_t, command_log_record_length_prefix_bytes> record_prefix{};
    if (remaining < record_total_length_bytes) {
      if (!read_available_prefix(source, frame_offset, remaining, record_prefix,
                                 options.read_chunk_bytes, result)) {
        return result;
      }
      set_error(result, LogScanTermination::truncated_tail,
                LogErrorCategory::truncated_final_record, result.source_extent);
      break;
    }

    read_failure =
        read_exact(source, frame_offset,
                   std::span<std::uint8_t>{record_prefix}.first(record_total_length_bytes),
                   options.read_chunk_bytes);
    if (read_failure) {
      set_io_error(result, read_failure);
      return result;
    }
    const auto declared_record_length =
        decode_u32(std::span<const std::uint8_t, 4U>{record_prefix.data(), 4U});
    if (declared_record_length < command_log_record_fixed_bytes) {
      set_error(result, LogScanTermination::corruption, LogErrorCategory::invalid_length,
                frame_offset);
      return result;
    }
    if (declared_record_length > options.codec_limits.max_record_bytes ||
        declared_record_length > default_max_log_record_bytes) {
      set_error(result, LogScanTermination::corruption, LogErrorCategory::excessive_length,
                frame_offset);
      return result;
    }

    if (remaining < command_log_record_length_prefix_bytes) {
      const auto available_after_total = remaining - record_total_length_bytes;
      if (!read_available_prefix(
              source, frame_offset + record_total_length_bytes, available_after_total,
              std::span<std::uint8_t>{record_prefix}.subspan(record_total_length_bytes),
              options.read_chunk_bytes, result)) {
        return result;
      }
      set_error(result, LogScanTermination::truncated_tail,
                LogErrorCategory::truncated_final_record, result.source_extent);
      break;
    }

    read_failure =
        read_exact(source, frame_offset + record_total_length_bytes,
                   std::span<std::uint8_t>{record_prefix}.subspan(record_total_length_bytes),
                   options.read_chunk_bytes);
    if (read_failure) {
      set_io_error(result, read_failure);
      return result;
    }
    const auto inspected_record = inspect_log_record_length(record_prefix, options.codec_limits);
    if (!inspected_record) {
      set_codec_corruption(result, inspected_record.error, frame_offset);
      return result;
    }
    if (*inspected_record.value != declared_record_length) {
      set_error(result, LogScanTermination::corruption, LogErrorCategory::invalid_length,
                frame_offset);
      return result;
    }

    std::vector<std::uint8_t> record_bytes(declared_record_length);
    std::copy(record_prefix.begin(), record_prefix.end(), record_bytes.begin());
    const auto available_record_bytes = std::min<std::uint64_t>(remaining, declared_record_length);
    const auto available_body = available_record_bytes - command_log_record_length_prefix_bytes;
    if (!read_available_prefix(
            source, frame_offset + command_log_record_length_prefix_bytes, available_body,
            std::span<std::uint8_t>{record_bytes}.subspan(command_log_record_length_prefix_bytes),
            options.read_chunk_bytes, result)) {
      return result;
    }
    if (declared_record_length > remaining) {
      set_error(result, LogScanTermination::truncated_tail,
                LogErrorCategory::truncated_final_record, result.source_extent);
      break;
    }

    auto decoded_record = decode_command_record(record_bytes, options.codec_limits);
    if (!decoded_record) {
      set_codec_corruption(result, decoded_record.error, frame_offset);
      return result;
    }

    const auto actual_sequence = decoded_record.value->sequence.value();
    if (!result.next_sequence.has_value() || actual_sequence < result.next_sequence->value()) {
      set_error(result, LogScanTermination::corruption, LogErrorCategory::duplicate_sequence,
                frame_offset + record_sequence_offset);
      return result;
    }
    if (actual_sequence > result.next_sequence->value()) {
      set_error(result, LogScanTermination::corruption, LogErrorCategory::missing_sequence,
                frame_offset + record_sequence_offset);
      return result;
    }

    valid_prefix_hash.update(record_bytes);
    const auto frame_end = frame_offset + declared_record_length;
    result.valid_end_offset = frame_end;
    ++result.record_count;
    result.last_sequence = decoded_record.value->sequence;
    if (actual_sequence == std::numeric_limits<std::uint64_t>::max()) {
      result.next_sequence.reset();
    } else {
      result.next_sequence = domain::Sequence{actual_sequence + 1U};
    }

    if (visitor != nullptr) {
      visitor->on_record(*decoded_record.value, record_bytes, frame_offset, frame_end);
    }
    if (sink != nullptr) {
      const auto write_failure = write_exact(*sink, record_bytes, options.read_chunk_bytes);
      if (write_failure) {
        set_io_error(result, write_failure);
        return result;
      }
    }
    frame_offset = frame_end;
  }

  if (frame_offset == result.source_extent) {
    result.termination = LogScanTermination::clean_eof;
    result.error = {};
  }
  if (sink != nullptr && result.repairable()) {
    const auto flush_failure = sink->flush();
    if (flush_failure) {
      set_io_error(result, flush_failure);
    }
  }
  if (result.clean() || result.repairable()) {
    result.valid_prefix_digest = valid_prefix_hash.finish();
  }
  return result;
}

[[nodiscard]] LogScanResult open_failure_result(const LogIoFailure& failure) noexcept {
  LogScanResult result;
  set_io_error(result, failure);
  return result;
}

[[nodiscard]] bool same_repairable_source(const LogScanResult& first,
                                          const LogScanResult& second) noexcept {
  return first.repairable() && second.repairable() && first.valid_prefix_digest.has_value() &&
         second.valid_prefix_digest.has_value() && first.source_extent == second.source_extent &&
         first.header_end_offset == second.header_end_offset &&
         first.valid_end_offset == second.valid_end_offset &&
         first.record_count == second.record_count && first.last_sequence == second.last_sequence &&
         first.next_sequence == second.next_sequence &&
         first.valid_prefix_digest == second.valid_prefix_digest;
}

}  // namespace

LogScanResult scan_command_log(LogSource& source, LogScanOptions options, LogScanVisitor* visitor) {
  return scan_impl(source, nullptr, options, visitor);
}

LogScanResult scan_command_log_to_sink(LogSource& source, LogSink& sink, LogScanOptions options,
                                       LogScanVisitor* visitor) {
  return scan_impl(source, &sink, options, visitor);
}

LogRepairResult repair_command_log_to_new_file(const std::filesystem::path& input_path,
                                               const std::filesystem::path& output_path,
                                               LogScanOptions options, LogScanVisitor* visitor) {
  auto source = open_native_log_source(input_path);
  if (!source) {
    return {
        .scan = open_failure_result(source.failure),
    };
  }
  if (!options.valid()) {
    return {
        .scan = scan_command_log(*source.source, options, visitor),
    };
  }

  auto validated = scan_command_log(*source.source, options, visitor);
  if (!validated.repairable()) {
    return {
        .scan = std::move(validated),
    };
  }

  auto sink = open_native_new_log_sink(output_path);
  if (!sink) {
    return {
        .scan = open_failure_result(sink.failure),
    };
  }

  auto scan = scan_command_log_to_sink(*source.source, *sink.sink, options);
  if (!same_repairable_source(validated, scan)) {
    const auto cleanup_failure = sink.sink->abandon();
    if (cleanup_failure) {
      set_io_error(scan, cleanup_failure);
    } else {
      set_error(scan, LogScanTermination::io_failure, LogErrorCategory::io_failure,
                scan.valid_end_offset, std::make_error_code(std::errc::state_not_recoverable));
    }
    return {
        .scan = std::move(scan),
    };
  }

  const auto commit_failure = sink.sink->commit();
  if (commit_failure) {
    set_io_error(scan, commit_failure);
    return {
        .scan = std::move(scan),
    };
  }
  return {
      .scan = std::move(scan),
      .output_created = true,
  };
}

}  // namespace atlaslob::persistence::detail
