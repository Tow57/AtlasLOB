#include "benchmark_log_materializer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "atlaslob/digest.hpp"
#include "atlaslob/persistence/logged_engine.hpp"
#include "benchmark_replay.hpp"
#include "benchmark_workload.hpp"
#include "log_io.hpp"
#include "log_scanner.hpp"
#include "platform_cli.hpp"
#include "sha256.hpp"

namespace atlaslob::benchmark {
namespace {

constexpr std::string_view result_schema{"ATLAS_BENCH_LOG_MATERIALIZATION_V1"};
constexpr std::size_t maximum_temporary_candidates{1'000U};

struct MaterializerOptions final {
  std::string workload_path;
  std::string workload_sha256;
  std::string output_path;
};

struct MaterializedEvidence final {
  std::uint64_t records{};
  std::uint64_t events{};
  std::uint64_t committed{};
  std::uint64_t rejected{};
  std::string event_digest;
  std::string final_digest;
};

[[nodiscard]] bool is_sha256(std::string_view value) noexcept {
  return value.size() == 64U && std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] bool is_missing_path_error(const std::error_code& error) noexcept {
  return error == std::errc::no_such_file_or_directory;
}

[[nodiscard]] std::optional<MaterializerOptions> parse_cli(
    std::span<const std::string_view> arguments) {
  MaterializerOptions options;
  bool workload_seen{};
  bool digest_seen{};
  bool output_seen{};
  if (arguments.empty()) {
    return std::nullopt;
  }
  for (std::size_t index = 1U; index < arguments.size(); index += 2U) {
    if (index + 1U >= arguments.size()) {
      return std::nullopt;
    }
    const auto name = arguments[index];
    const auto value = arguments[index + 1U];
    if (name == "--workload" && !workload_seen) {
      workload_seen = true;
      options.workload_path.assign(value);
    } else if (name == "--workload-sha256" && !digest_seen) {
      digest_seen = true;
      options.workload_sha256.assign(value);
    } else if (name == "--output" && !output_seen) {
      output_seen = true;
      options.output_path.assign(value);
    } else {
      return std::nullopt;
    }
  }
  if (!workload_seen || !digest_seen || !output_seen || options.workload_path.empty() ||
      options.output_path.empty() || !is_sha256(options.workload_sha256)) {
    return std::nullopt;
  }
  return options;
}

void print_usage() {
  std::cerr << "usage: atlas_bench_log_materializer"
               " --workload <ATLAS_DIFF_V2> --workload-sha256 <sha256>"
               " --output <new-ATLSLG01>\n";
}

[[nodiscard]] std::optional<std::string> hash_file(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::nullopt;
  }
  utility::Sha256 hash;
  std::array<std::uint8_t, 64U * 1024U> buffer{};
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      hash.update(std::span<const std::uint8_t>{buffer.data(), static_cast<std::size_t>(count)});
    }
  }
  if (!input.eof()) {
    return std::nullopt;
  }
  return hash.finish().hex();
}

[[nodiscard]] persistence::LogId deterministic_log_id(const Digest256& workload_digest) noexcept {
  persistence::LogId result;
  std::copy_n(workload_digest.bytes.begin(), result.bytes.size(), result.bytes.begin());
  return result;
}

[[nodiscard]] bool checked_increment(std::uint64_t& value) noexcept {
  if (value == std::numeric_limits<std::uint64_t>::max()) {
    return false;
  }
  ++value;
  return true;
}

[[nodiscard]] bool checked_add(std::uint64_t& value, std::uint64_t increment) noexcept {
  if (increment > std::numeric_limits<std::uint64_t>::max() - value) {
    return false;
  }
  value += increment;
  return true;
}

[[nodiscard]] bool same_header_configuration(const persistence::LogHeader& header,
                                             const BenchmarkWorkload& workload,
                                             persistence::LogId expected_log_id) noexcept {
  if (header.log_id != expected_log_id ||
      header.engine_config.max_total_active_orders !=
          static_cast<std::uint64_t>(workload.engine_config.max_total_active_orders) ||
      header.catalog.size() != workload.catalog.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < workload.catalog.size(); ++index) {
    const auto& persisted = header.catalog[index];
    const auto& expected = workload.catalog[index];
    if (persisted.instrument_id != expected.instrument_id ||
        persisted.max_order_quantity != expected.matching.max_order_quantity.value() ||
        persisted.tick_increment != expected.matching.tick_increment ||
        persisted.max_active_orders !=
            static_cast<std::uint64_t>(expected.matching.max_active_orders)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool verify_header(const std::filesystem::path& path,
                                 const BenchmarkWorkload& workload,
                                 persistence::LogId expected_log_id) {
  auto source = persistence::detail::open_native_log_source(path);
  if (!source) {
    return false;
  }
  const auto scan = persistence::detail::scan_command_log(*source.source);
  return scan.clean() && scan.header.has_value() &&
         same_header_configuration(*scan.header, workload, expected_log_id);
}

[[nodiscard]] std::string temporary_suffix(std::size_t candidate) {
  auto number = std::to_string(candidate);
  number.insert(number.begin(), 6U - number.size(), '0');
  return ".atlas-bench-tmp-" + number;
}

[[nodiscard]] bool remove_owned_temporary(const std::filesystem::path& path) noexcept {
  return !persistence::detail::remove_native_file(path);
}

class OwnedTemporary final {
 public:
  OwnedTemporary() = default;
  OwnedTemporary(const OwnedTemporary&) = delete;
  OwnedTemporary& operator=(const OwnedTemporary&) = delete;
  ~OwnedTemporary() {
    if (owned_) {
      static_cast<void>(remove_owned_temporary(path_));
    }
  }

  void take(std::filesystem::path path) {
    path_ = std::move(path);
    owned_ = true;
  }

  void release() noexcept { owned_ = false; }

  [[nodiscard]] bool cleanup() noexcept {
    if (!owned_) {
      return true;
    }
    if (!remove_owned_temporary(path_)) {
      return false;
    }
    owned_ = false;
    return true;
  }

 private:
  std::filesystem::path path_;
  bool owned_{};
};

[[nodiscard]] bool emit_result(const MaterializerOptions& options, persistence::LogId log_id,
                               std::string_view log_sha256, const MaterializedEvidence& evidence) {
  // Keys are emitted in Python sort_keys order. Every count is a canonical
  // decimal string, matching the Phase 5 evidence convention.
  std::cout << "{\"committed\":\"" << evidence.committed << "\",\"event_digest\":\""
            << evidence.event_digest << "\",\"events\":\"" << evidence.events
            << "\",\"final_digest\":\"" << evidence.final_digest << "\",\"log_id\":\""
            << log_id.hex() << "\",\"log_sha256\":\"" << log_sha256 << "\",\"records\":\""
            << evidence.records << "\",\"rejected\":\"" << evidence.rejected << "\",\"schema\":\""
            << result_schema << "\",\"workload_sha256\":\"" << options.workload_sha256 << "\"}\n";
  std::cout.flush();
  return static_cast<bool>(std::cout);
}

[[nodiscard]] int fail_after_temporary(std::string_view reason, OwnedTemporary& temporary,
                                       int exit_code) {
  std::cerr << reason << '\n';
  if (!temporary.cleanup()) {
    std::cerr << "temporary_cleanup_failure\n";
  }
  return exit_code;
}

[[nodiscard]] int materialize(const MaterializerOptions& options) {
  const auto workload_path = persistence::detail::path_from_utf8(options.workload_path);
  const auto output_path = persistence::detail::path_from_utf8(options.output_path);
  std::ifstream input{workload_path, std::ios::binary};
  if (!input) {
    std::cerr << "workload_io_failure\n";
    return materializer_operational_error_exit_code;
  }
  auto parsed = read_atlas_diff_v2_declared(input, maximum_benchmark_catalog_count);
  if (const auto* error = std::get_if<WorkloadParseError>(&parsed)) {
    std::cerr << (error->code == "input_read_failure" ? "workload_io_failure"
                                                      : "workload_parse_error")
              << '\n';
    return error->code == "input_read_failure" ? materializer_operational_error_exit_code
                                               : materializer_invalid_input_exit_code;
  }
  auto workload = std::get<BenchmarkWorkload>(std::move(parsed));
  if (workload.stream_digest.hex() != options.workload_sha256) {
    std::cerr << "workload_digest_mismatch\n";
    return materializer_invalid_input_exit_code;
  }

  std::error_code status_error;
  const auto destination_status = std::filesystem::symlink_status(output_path, status_error);
  if (status_error && !is_missing_path_error(status_error)) {
    std::cerr << "output_status_failure\n";
    return materializer_operational_error_exit_code;
  }
  if (!status_error && std::filesystem::exists(destination_status)) {
    std::cerr << "output_exists\n";
    return materializer_operational_error_exit_code;
  }

  const auto log_id = deterministic_log_id(workload.stream_digest);
  persistence::LoggedEngineOptions engine_options;
  engine_options.durability = persistence::Durability::buffered;

  std::filesystem::path temporary_path;
  OwnedTemporary temporary;
  std::unique_ptr<persistence::LoggedEngine> engine;
  for (std::size_t candidate = 0U; candidate < maximum_temporary_candidates; ++candidate) {
    temporary_path =
        persistence::detail::path_from_utf8(options.output_path + temporary_suffix(candidate));
    status_error.clear();
    const auto temporary_status = std::filesystem::symlink_status(temporary_path, status_error);
    if (status_error && !is_missing_path_error(status_error)) {
      std::cerr << "temporary_status_failure\n";
      return materializer_operational_error_exit_code;
    }
    if (!status_error && std::filesystem::exists(temporary_status)) {
      continue;
    }
    auto opened = persistence::LoggedEngine::create_new(temporary_path, workload.catalog, log_id,
                                                        workload.engine_config, engine_options);
    if (opened) {
      engine = std::move(opened.engine);
      temporary.take(temporary_path);
      break;
    }
    status_error.clear();
    if (std::filesystem::exists(temporary_path, status_error) && !status_error) {
      continue;
    }
    std::cerr << "temporary_create_failure\n";
    return materializer_operational_error_exit_code;
  }
  if (engine == nullptr) {
    std::cerr << "temporary_name_exhausted\n";
    return materializer_operational_error_exit_code;
  }

  MaterializedEvidence evidence;
  utility::Sha256 event_hash;
  for (const auto& command : workload.commands) {
    auto submitted = engine->submit(command.command);
    if (!submitted) {
      engine.reset();
      return fail_after_temporary("logged_submission_failure", temporary,
                                  materializer_operational_error_exit_code);
    }
    const auto* batch = submitted.engine_result->batch();
    if (batch == nullptr || !checked_increment(evidence.records) ||
        !checked_add(evidence.events, static_cast<std::uint64_t>(batch->size()))) {
      engine.reset();
      return fail_after_temporary("materialization_evidence_failure", temporary,
                                  materializer_invalid_input_exit_code);
    }
    if (submitted.engine_result->committed()) {
      if (!checked_increment(evidence.committed)) {
        engine.reset();
        return fail_after_temporary("materialization_evidence_failure", temporary,
                                    materializer_invalid_input_exit_code);
      }
    } else if (!checked_increment(evidence.rejected)) {
      engine.reset();
      return fail_after_temporary("materialization_evidence_failure", temporary,
                                  materializer_invalid_input_exit_code);
    }
    event_hash.update(atlaslob::event_digest(*batch).bytes);
  }
  evidence.event_digest = event_hash.finish().hex();
  evidence.final_digest = engine->engine().state_digest().hex();
  if (const auto sync_error = engine->synchronize(); !sync_error.ok()) {
    engine.reset();
    return fail_after_temporary("log_sync_failure", temporary,
                                materializer_operational_error_exit_code);
  }
  engine.reset();

  const auto log_sha256 = hash_file(temporary_path);
  if (!log_sha256.has_value()) {
    return fail_after_temporary("log_hash_failure", temporary,
                                materializer_operational_error_exit_code);
  }
  const auto prepared = prepare_replay_log(temporary_path, *log_sha256);
  const auto verified = validate_replay_log(temporary_path);
  if (!prepared.valid || !verified.valid || prepared.evidence.records != evidence.records ||
      prepared.evidence.events != evidence.events ||
      prepared.evidence.committed != evidence.committed ||
      prepared.evidence.rejected != evidence.rejected ||
      prepared.evidence.event_digest != evidence.event_digest ||
      verified.records_scanned != evidence.records ||
      verified.records_replayed != evidence.records || verified.committed != evidence.committed ||
      verified.rejected != evidence.rejected || verified.final_digest != evidence.final_digest ||
      !verify_header(temporary_path, workload, log_id)) {
    return fail_after_temporary("log_verification_failure", temporary,
                                materializer_invalid_input_exit_code);
  }

  const auto publication =
      persistence::detail::publish_native_new_file_no_replace(temporary_path, output_path);
  if (!publication.source_visible) {
    temporary.release();
  }
  if (!publication) {
    return fail_after_temporary("log_publication_failure", temporary,
                                materializer_operational_error_exit_code);
  }
  temporary.release();

  if (!emit_result(options, log_id, *log_sha256, evidence)) {
    return materializer_operational_error_exit_code;
  }
  return materializer_success_exit_code;
}

}  // namespace

int run_log_materializer_cli(std::span<const std::string_view> arguments) {
  if (!persistence::detail::configure_binary_standard_streams()) {
    return materializer_operational_error_exit_code;
  }
  const auto options = parse_cli(arguments);
  if (!options.has_value()) {
    print_usage();
    return materializer_usage_exit_code;
  }
  try {
    return materialize(*options);
  } catch (const std::bad_alloc&) {
    std::cerr << "resource_failure\n";
    return materializer_operational_error_exit_code;
  } catch (const std::exception&) {
    std::cerr << "materializer_exception\n";
    return materializer_operational_error_exit_code;
  } catch (...) {
    std::cerr << "unknown_materializer_exception\n";
    return materializer_operational_error_exit_code;
  }
}

int run_native_log_materializer_cli(int argc, char** argv) {
  if (!persistence::detail::configure_binary_standard_streams()) {
    return materializer_operational_error_exit_code;
  }
  try {
    const auto native = persistence::detail::native_command_line_arguments(argc, argv);
    return run_log_materializer_cli(native.arguments());
  } catch (...) {
    return materializer_operational_error_exit_code;
  }
}

}  // namespace atlaslob::benchmark
