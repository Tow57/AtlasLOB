#include <pybind11/pybind11.h>
#include <pybind11/stl/filesystem.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "atlaslob/book_snapshot.hpp"
#include "atlaslob/digest.hpp"
#include "atlaslob/domain/commands.hpp"
#include "atlaslob/domain/events.hpp"
#include "atlaslob/matching_engine.hpp"
#include "atlaslob/multi_instrument_engine.hpp"
#include "atlaslob/persistence/logged_engine.hpp"
#include "atlaslob/persistence/replay.hpp"
#include "atlaslob/persistence/snapshot_store.hpp"

namespace py = pybind11;

namespace atlaslob::python {
namespace {

inline constexpr std::uint32_t binding_abi{2U};

PyObject* native_persistence_error = nullptr;
PyObject* native_recovery_error = nullptr;
PyObject* native_snapshot_error = nullptr;
PyObject* native_read_only_error = nullptr;

[[noreturn]] void raise_native_error(PyObject* exception_type, std::string_view message,
                                     py::dict details) {
  py::tuple arguments{2U};
  arguments[0] = py::str{message};
  arguments[1] = std::move(details);
  PyErr_SetObject(exception_type, arguments.ptr());
  throw py::error_already_set{};
}

[[noreturn]] void raise_type_error(std::string_view message) {
  PyErr_SetString(PyExc_TypeError, std::string{message}.c_str());
  throw py::error_already_set{};
}

[[noreturn]] void raise_overflow_error(std::string_view name) {
  const auto message = std::string{name} + " is outside the native representation";
  PyErr_SetString(PyExc_OverflowError, message.c_str());
  throw py::error_already_set{};
}

[[nodiscard]] py::dict require_exact_dict(py::handle value, std::string_view name) {
  if (PyDict_CheckExact(value.ptr()) == 0) {
    raise_type_error(std::string{name} + " must be a plain dict");
  }
  return py::reinterpret_borrow<py::dict>(value);
}

[[nodiscard]] py::handle required_item(const py::dict& value, const char* key,
                                       std::string_view container_name) {
  auto* item = PyDict_GetItemString(value.ptr(), key);
  if (item == nullptr) {
    raise_type_error(std::string{container_name} + " is missing field '" + key + "'");
  }
  return py::handle{item};
}

void require_field_count(const py::dict& value, Py_ssize_t expected,
                         std::string_view container_name) {
  if (PyDict_Size(value.ptr()) != expected) {
    raise_type_error(std::string{container_name} + " has unexpected fields");
  }
}

[[nodiscard]] std::uint64_t require_u64(py::handle value, std::string_view name) {
  if (PyBool_Check(value.ptr()) != 0 || PyLong_Check(value.ptr()) == 0) {
    raise_type_error(std::string{name} + " must be an int, not bool");
  }
  const auto converted = PyLong_AsUnsignedLongLong(value.ptr());
  if (PyErr_Occurred() != nullptr) {
    PyErr_Clear();
    raise_overflow_error(name);
  }
  static_assert(sizeof(unsigned long long) >= sizeof(std::uint64_t));
  if constexpr (sizeof(unsigned long long) > sizeof(std::uint64_t)) {
    if (converted > std::numeric_limits<std::uint64_t>::max()) {
      raise_overflow_error(name);
    }
  }
  return static_cast<std::uint64_t>(converted);
}

[[nodiscard]] std::uint32_t require_u32(py::handle value, std::string_view name) {
  const auto converted = require_u64(value, name);
  if (converted > std::numeric_limits<std::uint32_t>::max()) {
    raise_overflow_error(name);
  }
  return static_cast<std::uint32_t>(converted);
}

[[nodiscard]] std::uint8_t require_u8(py::handle value, std::string_view name) {
  const auto converted = require_u64(value, name);
  if (converted > std::numeric_limits<std::uint8_t>::max()) {
    raise_overflow_error(name);
  }
  return static_cast<std::uint8_t>(converted);
}

[[nodiscard]] std::size_t require_size(py::handle value, std::string_view name) {
  const auto converted = require_u64(value, name);
  if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
    if (converted > std::numeric_limits<std::size_t>::max()) {
      raise_overflow_error(name);
    }
  }
  return static_cast<std::size_t>(converted);
}

[[nodiscard]] std::int64_t require_i64(py::handle value, std::string_view name) {
  if (PyBool_Check(value.ptr()) != 0 || PyLong_Check(value.ptr()) == 0) {
    raise_type_error(std::string{name} + " must be an int, not bool");
  }
  const auto converted = PyLong_AsLongLong(value.ptr());
  if (PyErr_Occurred() != nullptr) {
    PyErr_Clear();
    raise_overflow_error(name);
  }
  static_assert(sizeof(long long) >= sizeof(std::int64_t));
  if constexpr (sizeof(long long) > sizeof(std::int64_t)) {
    if (converted < std::numeric_limits<std::int64_t>::min() ||
        converted > std::numeric_limits<std::int64_t>::max()) {
      raise_overflow_error(name);
    }
  }
  return static_cast<std::int64_t>(converted);
}

[[nodiscard]] std::string require_string(py::handle value, std::string_view name) {
  if (PyUnicode_Check(value.ptr()) == 0) {
    raise_type_error(std::string{name} + " must be a str");
  }
  return py::reinterpret_borrow<py::str>(value).cast<std::string>();
}

[[nodiscard]] std::filesystem::path require_path(py::handle value, std::string_view name) {
  if (PyBytes_Check(value.ptr()) != 0) {
    raise_type_error(std::string{name} + " must be str or os.PathLike[str], not bytes");
  }

  py::object normalized;
  if (PyUnicode_Check(value.ptr()) != 0) {
    normalized = py::reinterpret_borrow<py::object>(value);
  } else {
    auto* result = PyOS_FSPath(value.ptr());
    if (result == nullptr) {
      throw py::error_already_set{};
    }
    normalized = py::reinterpret_steal<py::object>(result);
  }
  if (PyUnicode_Check(normalized.ptr()) == 0) {
    raise_type_error(std::string{name} + " must resolve to str, not bytes");
  }
  return normalized.cast<std::filesystem::path>();
}

[[nodiscard]] persistence::Durability parse_durability(py::handle value) {
  const auto text = require_string(value, "durability");
  if (text == "buffered") {
    return persistence::Durability::buffered;
  }
  if (text == "flush_each_record") {
    return persistence::Durability::flush_each_record;
  }
  if (text == "sync_each_record") {
    return persistence::Durability::sync_each_record;
  }
  raise_type_error("durability must be 'buffered', 'flush_each_record', or 'sync_each_record'");
}

[[nodiscard]] persistence::ReplayMode parse_replay_mode(py::handle value) {
  const auto text = require_string(value, "mode");
  if (text == "fast") {
    return persistence::ReplayMode::fast;
  }
  if (text == "verify") {
    return persistence::ReplayMode::verify;
  }
  if (text == "diagnostic") {
    return persistence::ReplayMode::diagnostic;
  }
  raise_type_error("mode must be 'fast', 'verify', or 'diagnostic'");
}

[[nodiscard]] persistence::TailPolicy parse_tail_policy(py::handle value) {
  const auto text = require_string(value, "tail_policy");
  if (text == "strict") {
    return persistence::TailPolicy::strict;
  }
  if (text == "valid-prefix") {
    return persistence::TailPolicy::valid_prefix;
  }
  raise_type_error("tail_policy must be 'strict' or 'valid-prefix'");
}

enum class OutputMode : std::uint8_t {
  objects = 1,
  columns = 2,
  summary = 3,
};

[[nodiscard]] OutputMode parse_output_mode(py::handle value) {
  const auto text = require_string(value, "output");
  if (text == "objects") {
    return OutputMode::objects;
  }
  if (text == "columns") {
    return OutputMode::columns;
  }
  if (text == "summary") {
    return OutputMode::summary;
  }
  raise_type_error("output must be 'objects', 'columns', or 'summary'");
}

[[nodiscard]] std::string_view to_string(OutputMode value) noexcept {
  switch (value) {
    case OutputMode::objects:
      return "objects";
    case OutputMode::columns:
      return "columns";
    case OutputMode::summary:
      return "summary";
  }
  return "unknown";
}

[[nodiscard]] InstrumentConfig convert_instrument_config(py::handle input) {
  const auto config = require_exact_dict(input, "catalog entry");
  require_field_count(config, 4, "catalog entry");
  return InstrumentConfig{
      .instrument_id = domain::InstrumentId{require_u32(
          required_item(config, "instrument_id", "catalog entry"), "instrument_id")},
      .matching =
          MatchingEngineConfig{
              .max_order_quantity = domain::Quantity{require_u64(
                  required_item(config, "max_order_quantity", "catalog entry"),
                  "max_order_quantity")},
              .tick_increment = domain::PriceTicks{require_i64(
                  required_item(config, "tick_increment", "catalog entry"), "tick_increment")},
              .max_active_orders = require_size(
                  required_item(config, "max_active_orders", "catalog entry"), "max_active_orders"),
          },
  };
}

[[nodiscard]] std::vector<InstrumentConfig> convert_catalog(py::handle input) {
  if (PyList_CheckExact(input.ptr()) == 0) {
    raise_type_error("catalog must be a plain list");
  }
  const auto catalog = py::reinterpret_borrow<py::list>(input);
  std::vector<InstrumentConfig> converted;
  converted.reserve(catalog.size());
  for (const auto entry : catalog) {
    converted.push_back(convert_instrument_config(entry));
  }
  return converted;
}

[[nodiscard]] domain::Command convert_command(py::handle input) {
  const auto command = require_exact_dict(input, "command");
  const auto type = require_string(required_item(command, "type", "command"), "command type");
  if (type == "new") {
    require_field_count(command, 9, "new command");
    std::optional<domain::PriceTicks> limit_price;
    const auto price = required_item(command, "limit_price", "new command");
    if (!price.is_none()) {
      limit_price = domain::PriceTicks{require_i64(price, "limit_price")};
    }
    return domain::NewOrder{
        .client_id = domain::ClientId{require_u32(
            required_item(command, "client_id", "new command"), "client_id")},
        .order_id = domain::OrderId{require_u64(required_item(command, "order_id", "new command"),
                                                "order_id")},
        .instrument_id = domain::InstrumentId{require_u32(
            required_item(command, "instrument_id", "new command"), "instrument_id")},
        .side = static_cast<domain::Side>(
            require_u8(required_item(command, "side", "new command"), "side")),
        .order_type = static_cast<domain::OrderType>(
            require_u8(required_item(command, "order_type", "new command"), "order_type")),
        .time_in_force = static_cast<domain::TimeInForce>(
            require_u8(required_item(command, "time_in_force", "new command"), "time_in_force")),
        .limit_price = limit_price,
        .quantity = domain::Quantity{require_u64(required_item(command, "quantity", "new command"),
                                                 "quantity")},
    };
  }
  if (type == "cancel") {
    require_field_count(command, 4, "cancel command");
    return domain::CancelOrder{
        .client_id = domain::ClientId{require_u32(
            required_item(command, "client_id", "cancel command"), "client_id")},
        .order_id = domain::OrderId{require_u64(
            required_item(command, "order_id", "cancel command"), "order_id")},
        .instrument_id = domain::InstrumentId{require_u32(
            required_item(command, "instrument_id", "cancel command"), "instrument_id")},
    };
  }
  if (type == "replace") {
    require_field_count(command, 7, "replace command");
    return domain::ReplaceOrder{
        .client_id = domain::ClientId{require_u32(
            required_item(command, "client_id", "replace command"), "client_id")},
        .old_order_id = domain::OrderId{require_u64(
            required_item(command, "old_order_id", "replace command"), "old_order_id")},
        .new_order_id = domain::OrderId{require_u64(
            required_item(command, "new_order_id", "replace command"), "new_order_id")},
        .instrument_id = domain::InstrumentId{require_u32(
            required_item(command, "instrument_id", "replace command"), "instrument_id")},
        .new_limit_price = domain::PriceTicks{require_i64(
            required_item(command, "new_limit_price", "replace command"), "new_limit_price")},
        .new_quantity = domain::Quantity{require_u64(
            required_item(command, "new_quantity", "replace command"), "new_quantity")},
    };
  }
  raise_type_error("command type must be 'new', 'cancel', or 'replace'");
}

[[nodiscard]] std::vector<domain::Command> convert_commands(py::handle input) {
  if (PyList_CheckExact(input.ptr()) == 0) {
    raise_type_error("commands must be a plain list");
  }
  const auto commands = py::reinterpret_borrow<py::list>(input);
  std::vector<domain::Command> converted;
  converted.reserve(commands.size());
  for (const auto command : commands) {
    converted.push_back(convert_command(command));
  }
  return converted;
}

using RecoveryReport =
    std::variant<std::monostate, persistence::ReplayReport, persistence::SnapshotRecoveryReport>;

struct BatchNative final {
  std::size_t submitted_count{};
  std::size_t processed_count{};
  std::uint64_t committed_count{};
  std::uint64_t rejected_count{};
  EngineError terminal_error{EngineError::none};
  Digest256 final_state_digest{};
  OutputMode output{OutputMode::objects};
  std::vector<EngineResult> results;
};

struct PersistenceFailureNative final {
  persistence::LogError error{};
  bool session_poisoned{};
  BatchNative prefix;
};

struct BatchExecution final {
  BatchNative batch;
  std::optional<PersistenceFailureNative> persistence_failure;
};

struct LoggedOpenNative final {
  std::shared_ptr<class NativeEngine> engine;
  persistence::LogError error{};
};

struct RecoveryFailureNative final {
  RecoveryReport report;
};

struct RecoveryNative final {
  std::shared_ptr<class NativeEngine> engine;
  std::optional<RecoveryFailureNative> failure;
};

enum class Backend : std::uint8_t {
  live = 1,
  logged = 2,
  recovered_read_only = 3,
};

class NativeEngine final {
 public:
  [[nodiscard]] static std::shared_ptr<NativeEngine> create_live(
      std::vector<InstrumentConfig> catalog, MultiInstrumentEngineConfig config) {
    auto engine = std::make_unique<MultiInstrumentEngine>(catalog, config);
    return std::shared_ptr<NativeEngine>{
        new NativeEngine{Backend::live, std::move(engine), nullptr, std::monostate{}}};
  }

  [[nodiscard]] static LoggedOpenNative create_logged(const std::filesystem::path& path,
                                                      std::vector<InstrumentConfig> catalog,
                                                      MultiInstrumentEngineConfig config,
                                                      persistence::LoggedEngineOptions options) {
    auto opened = persistence::LoggedEngine::create_new(path, catalog, config, options);
    if (!opened) {
      return {
          .engine = nullptr,
          .error = opened.error,
      };
    }
    return {
        .engine = std::shared_ptr<NativeEngine>{new NativeEngine{
            Backend::logged, nullptr, std::move(opened.engine), std::monostate{}}},
        .error = {},
    };
  }

  [[nodiscard]] static RecoveryNative recover(
      const std::filesystem::path& log_path,
      const std::optional<std::filesystem::path>& snapshot_path,
      const std::optional<std::filesystem::path>& snapshot_directory,
      persistence::ReplayOptions replay_options, persistence::LoggedEngineOptions logged_options) {
    if (snapshot_path.has_value() && snapshot_directory.has_value()) {
      throw std::invalid_argument{"snapshot_path and snapshot_directory are mutually exclusive"};
    }

    if (snapshot_path.has_value()) {
      auto attached = persistence::LoggedEngine::recover_from_snapshot(
          log_path, *snapshot_path, replay_options, logged_options);
      if (attached) {
        auto report = attached.report;
        return {
            .engine = std::shared_ptr<NativeEngine>{new NativeEngine{
                Backend::logged, nullptr, std::move(attached.engine), std::move(report)}},
            .failure = std::nullopt,
        };
      }
      auto report = std::move(attached.report);
      if (valid_prefix_read_only_allowed(report.replay, report.snapshot_error, replay_options)) {
        auto replayed =
            persistence::recover_log_from_snapshot(log_path, *snapshot_path, replay_options);
        if (replayed) {
          auto read_only_report = replayed.report;
          return {
              .engine = std::shared_ptr<NativeEngine>{new NativeEngine{
                  Backend::recovered_read_only, std::move(replayed.engine), nullptr,
                  std::move(read_only_report)}},
              .failure = std::nullopt,
          };
        }
        report = std::move(replayed.report);
      }
      return {
          .engine = nullptr,
          .failure = RecoveryFailureNative{.report = std::move(report)},
      };
    }

    if (snapshot_directory.has_value()) {
      auto attached = persistence::LoggedEngine::recover_from_snapshot_directory(
          log_path, *snapshot_directory, replay_options, logged_options);
      if (attached) {
        auto report = attached.report;
        return {
            .engine = std::shared_ptr<NativeEngine>{new NativeEngine{
                Backend::logged, nullptr, std::move(attached.engine), std::move(report)}},
            .failure = std::nullopt,
        };
      }
      auto report = std::move(attached.report);
      if (valid_prefix_read_only_allowed(report.replay, report.snapshot_error, replay_options)) {
        auto replayed = persistence::recover_log_from_snapshot_directory(
            log_path, *snapshot_directory, replay_options);
        if (replayed) {
          auto read_only_report = replayed.report;
          return {
              .engine = std::shared_ptr<NativeEngine>{new NativeEngine{
                  Backend::recovered_read_only, std::move(replayed.engine), nullptr,
                  std::move(read_only_report)}},
              .failure = std::nullopt,
          };
        }
        report = std::move(replayed.report);
      }
      return {
          .engine = nullptr,
          .failure = RecoveryFailureNative{.report = std::move(report)},
      };
    }

    auto attached = persistence::LoggedEngine::recover(log_path, replay_options, logged_options);
    if (attached) {
      auto report = attached.report;
      return {
          .engine = std::shared_ptr<NativeEngine>{new NativeEngine{
              Backend::logged, nullptr, std::move(attached.engine), std::move(report)}},
          .failure = std::nullopt,
      };
    }
    auto report = std::move(attached.report);
    if (valid_prefix_read_only_allowed(report, {}, replay_options)) {
      auto replayed = persistence::replay_log(log_path, replay_options);
      if (replayed) {
        auto read_only_report = replayed.report;
        return {
            .engine = std::shared_ptr<NativeEngine>{new NativeEngine{
                Backend::recovered_read_only, std::move(replayed.engine), nullptr,
                std::move(read_only_report)}},
            .failure = std::nullopt,
        };
      }
      report = std::move(replayed.report);
    }
    return {
        .engine = nullptr,
        .failure = RecoveryFailureNative{.report = std::move(report)},
    };
  }

  NativeEngine(const NativeEngine&) = delete;
  NativeEngine& operator=(const NativeEngine&) = delete;
  NativeEngine(NativeEngine&&) = delete;
  NativeEngine& operator=(NativeEngine&&) = delete;
  ~NativeEngine() = default;

  [[nodiscard]] bool logged() const noexcept { return backend_ == Backend::logged; }
  [[nodiscard]] bool read_only() const noexcept { return backend_ == Backend::recovered_read_only; }
  [[nodiscard]] const RecoveryReport& recovery_report() const noexcept { return recovery_report_; }

  [[nodiscard]] bool poisoned() const {
    std::lock_guard lock{mutex_};
    return logged_ != nullptr && logged_->poisoned();
  }

  [[nodiscard]] BatchExecution submit_batch(std::span<const domain::Command> commands,
                                            OutputMode output,
                                            bool include_final_state_digest = true) {
    std::lock_guard lock{mutex_};
    if (read_only()) {
      throw std::logic_error{"read-only recovered engine cannot submit commands"};
    }

    BatchNative batch{
        .submitted_count = commands.size(),
        .processed_count = 0U,
        .committed_count = 0U,
        .rejected_count = 0U,
        .terminal_error = EngineError::none,
        .final_state_digest = {},
        .output = output,
        .results = {},
    };
    if (output != OutputMode::summary) {
      batch.results.reserve(commands.size());
    }

    // Summary still retains one result at a time so the engine outcome can be
    // classified without constructing Python event objects or column storage.
    for (const auto& command : commands) {
      if (logged_ != nullptr) {
        auto submitted = logged_->submit(command);
        if (!submitted) {
          if (include_final_state_digest) {
            batch.final_state_digest = observer().state_digest();
          }
          return {
              .batch = {},
              .persistence_failure =
                  PersistenceFailureNative{
                      .error = submitted.error,
                      .session_poisoned = submitted.session_poisoned,
                      .prefix = std::move(batch),
                  },
          };
        }
        auto result = std::move(*submitted.engine_result);
        ++batch.processed_count;
        if (!classify_and_continue(result, batch)) {
          if (output != OutputMode::summary) {
            batch.results.push_back(std::move(result));
          }
          break;
        }
        if (output != OutputMode::summary) {
          batch.results.push_back(std::move(result));
        }
      } else {
        auto result = live_->execute(command);
        ++batch.processed_count;
        if (!classify_and_continue(result, batch)) {
          if (output != OutputMode::summary) {
            batch.results.push_back(std::move(result));
          }
          break;
        }
        if (output != OutputMode::summary) {
          batch.results.push_back(std::move(result));
        }
      }
    }

    if (include_final_state_digest) {
      batch.final_state_digest = observer().state_digest();
    }
    return {
        .batch = std::move(batch),
        .persistence_failure = std::nullopt,
    };
  }

  [[nodiscard]] std::optional<BookTop> top(domain::InstrumentId instrument_id) const {
    std::lock_guard lock{mutex_};
    return observer().top(instrument_id);
  }

  [[nodiscard]] std::optional<InstrumentSnapshot> instrument_snapshot(
      domain::InstrumentId instrument_id) const {
    std::lock_guard lock{mutex_};
    return observer().snapshot(instrument_id);
  }

  [[nodiscard]] EngineSnapshot engine_snapshot() const {
    std::lock_guard lock{mutex_};
    return observer().snapshot();
  }

  [[nodiscard]] Digest256 state_digest() const {
    std::lock_guard lock{mutex_};
    return observer().state_digest();
  }

  [[nodiscard]] persistence::SnapshotPublicationResult write_snapshot(
      const std::filesystem::path& directory) {
    std::lock_guard lock{mutex_};
    if (read_only()) {
      throw std::logic_error{"read-only recovered engine cannot write snapshots"};
    }
    if (logged_ == nullptr) {
      return {
          .path = {},
          .covered_sequence = {},
          .covered_log_byte_offset = 0U,
          .encoded_bytes = 0U,
          .final_file_visible = false,
          .error =
              {
                  .category = persistence::SnapshotErrorCategory::io_failure,
                  .byte_offset = 0U,
                  .system_error = std::make_error_code(std::errc::operation_not_supported),
              },
      };
    }
    return logged_->write_snapshot(directory);
  }

 private:
  NativeEngine(Backend backend, std::unique_ptr<MultiInstrumentEngine> live,
               std::unique_ptr<persistence::LoggedEngine> logged, RecoveryReport recovery_report)
      : backend_{backend},
        live_{std::move(live)},
        logged_{std::move(logged)},
        recovery_report_{std::move(recovery_report)} {
    if ((backend_ == Backend::logged) != (logged_ != nullptr) ||
        (backend_ != Backend::logged) != (live_ != nullptr)) {
      throw std::logic_error{"native engine backend ownership is inconsistent"};
    }
  }

  [[nodiscard]] static bool valid_prefix_read_only_allowed(
      const persistence::ReplayReport& report, const persistence::SnapshotError& snapshot_error,
      const persistence::ReplayOptions& options) noexcept {
    return options.tail_policy == persistence::TailPolicy::valid_prefix &&
           report.tail == persistence::ReplayTail::torn && report.used_valid_prefix &&
           report.error.ok() && !report.divergence.has_value() && snapshot_error.ok();
  }

  [[nodiscard]] static bool classify_and_continue(const EngineResult& result,
                                                  BatchNative& batch) noexcept {
    if (result.committed()) {
      ++batch.committed_count;
      return true;
    }
    if (result.rejected()) {
      ++batch.rejected_count;
      return true;
    }
    batch.terminal_error = result.error();
    return false;
  }

  [[nodiscard]] const MultiInstrumentEngine& observer() const noexcept {
    return logged_ != nullptr ? logged_->engine() : *live_;
  }

  Backend backend_;
  std::unique_ptr<MultiInstrumentEngine> live_;
  std::unique_ptr<persistence::LoggedEngine> logged_;
  RecoveryReport recovery_report_;
  mutable std::mutex mutex_;
};

template <typename Value>
[[nodiscard]] py::object integer_object(Value value) {
  return py::cast(value);
}

[[nodiscard]] py::dict top_level_to_python(const domain::TopOfBookLevel& level) {
  py::dict output;
  output["price"] = integer_object(level.price.value());
  output["aggregate_quantity"] = integer_object(level.aggregate_quantity.value());
  return output;
}

[[nodiscard]] py::object optional_top_level_to_python(
    const std::optional<domain::TopOfBookLevel>& level) {
  if (!level.has_value()) {
    return py::none{};
  }
  return top_level_to_python(*level);
}

[[nodiscard]] py::dict event_to_python(const domain::Event& event) {
  return std::visit(
      [](const auto& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        py::dict output;
        output["type"] =
            integer_object(static_cast<std::uint8_t>(domain::expected_event_type<Value>()));
        output["command_sequence"] = integer_object(value.header.command_sequence.value());
        output["event_index"] = integer_object(value.header.event_index);
        output["instrument_id"] = integer_object(value.header.instrument_id.value());

        if constexpr (std::is_same_v<Value, domain::AcceptedEvent>) {
          output["command_type"] = integer_object(static_cast<std::uint8_t>(value.command_type));
        } else if constexpr (std::is_same_v<Value, domain::RejectedEvent>) {
          output["command_type"] = integer_object(static_cast<std::uint8_t>(value.command_type));
          output["reason"] = integer_object(static_cast<std::uint16_t>(value.reason));
          output["order_id"] = value.order_id.has_value() ? integer_object(value.order_id->value())
                                                          : py::object{py::none{}};
        } else if constexpr (std::is_same_v<Value, domain::TradeEvent>) {
          output["aggressor_order_id"] = integer_object(value.aggressor_order_id.value());
          output["resting_order_id"] = integer_object(value.resting_order_id.value());
          output["aggressor_client_id"] = integer_object(value.aggressor_client_id.value());
          output["resting_client_id"] = integer_object(value.resting_client_id.value());
          output["aggressor_side"] =
              integer_object(static_cast<std::uint8_t>(value.aggressor_side));
          output["execution_price"] = integer_object(value.execution_price.value());
          output["execution_quantity"] = integer_object(value.execution_quantity.value());
          output["aggressor_remaining"] = integer_object(value.aggressor_remaining.value());
          output["resting_remaining"] = integer_object(value.resting_remaining.value());
        } else if constexpr (std::is_same_v<Value, domain::RestedEvent>) {
          output["order_id"] = integer_object(value.order_id.value());
          output["client_id"] = integer_object(value.client_id.value());
          output["side"] = integer_object(static_cast<std::uint8_t>(value.side));
          output["price"] = integer_object(value.price.value());
          output["remaining_quantity"] = integer_object(value.remaining_quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::CanceledEvent>) {
          output["order_id"] = integer_object(value.order_id.value());
          output["canceled_quantity"] = integer_object(value.canceled_quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::ReplacedEvent>) {
          output["old_order_id"] = integer_object(value.old_order_id.value());
          output["new_order_id"] = integer_object(value.new_order_id.value());
        } else if constexpr (std::is_same_v<Value, domain::DoneEvent>) {
          output["order_id"] = integer_object(value.order_id.value());
          output["reason"] = integer_object(static_cast<std::uint8_t>(value.reason));
          output["remaining_quantity"] = integer_object(value.remaining_quantity.value());
        } else {
          static_assert(std::is_same_v<Value, domain::BookChangedEvent>);
          output["best_bid"] = optional_top_level_to_python(value.best_bid);
          output["best_ask"] = optional_top_level_to_python(value.best_ask);
        }
        return output;
      },
      event);
}

[[nodiscard]] py::dict engine_result_to_python(const EngineResult& result) {
  py::dict output;
  const auto* batch = result.batch();
  if (batch == nullptr) {
    output["error"] = integer_object(static_cast<std::uint8_t>(result.error()));
    output["events"] = py::list{};
    output["command_sequence"] = py::none{};
    output["instrument_id"] = py::none{};
    output["committed"] = false;
    output["rejected"] = false;
    return output;
  }

  py::list events;
  for (const auto& event : batch->events()) {
    events.append(event_to_python(event));
  }
  output["error"] = py::none{};
  output["events"] = std::move(events);
  output["command_sequence"] = integer_object(batch->command_sequence().value());
  output["instrument_id"] = integer_object(batch->instrument_id().value());
  output["committed"] = result.committed();
  output["rejected"] = result.rejected();
  return output;
}

[[nodiscard]] py::dict order_snapshot_to_python(const OrderSnapshot& order) {
  py::dict output;
  output["order_id"] = integer_object(order.order_id.value());
  output["client_id"] = integer_object(order.client_id.value());
  output["instrument_id"] = integer_object(order.instrument_id.value());
  output["side"] = integer_object(static_cast<std::uint8_t>(order.side));
  output["price"] = integer_object(order.price.value());
  output["remaining_quantity"] = integer_object(order.remaining_quantity.value());
  output["priority_sequence"] = integer_object(order.priority_sequence.value());
  return output;
}

[[nodiscard]] py::dict price_level_snapshot_to_python(const PriceLevelSnapshot& level) {
  py::list orders;
  for (const auto& order : level.orders) {
    orders.append(order_snapshot_to_python(order));
  }
  py::dict output;
  output["price"] = integer_object(level.price.value());
  output["aggregate_quantity"] = integer_object(level.aggregate_quantity.value());
  output["orders"] = std::move(orders);
  return output;
}

[[nodiscard]] py::list price_levels_to_python(std::span<const PriceLevelSnapshot> levels) {
  py::list output;
  for (const auto& level : levels) {
    output.append(price_level_snapshot_to_python(level));
  }
  return output;
}

[[nodiscard]] py::dict instrument_snapshot_to_python(const InstrumentSnapshot& snapshot) {
  py::dict output;
  output["instrument_id"] = integer_object(snapshot.instrument_id.value());
  output["active_order_count"] = integer_object(snapshot.active_order_count);
  output["bids"] = price_levels_to_python(snapshot.bids);
  output["asks"] = price_levels_to_python(snapshot.asks);
  return output;
}

[[nodiscard]] py::dict matching_config_to_python(const MatchingEngineConfig& config) {
  py::dict output;
  output["max_order_quantity"] = integer_object(config.max_order_quantity.value());
  output["tick_increment"] = integer_object(config.tick_increment.value());
  output["max_active_orders"] = integer_object(config.max_active_orders);
  return output;
}

[[nodiscard]] py::dict engine_snapshot_to_python(const EngineSnapshot& snapshot) {
  py::dict engine_config;
  engine_config["max_total_active_orders"] =
      integer_object(snapshot.engine_config.max_total_active_orders);

  py::list catalog;
  for (const auto& entry : snapshot.catalog) {
    py::dict item;
    item["instrument_id"] = integer_object(entry.instrument_id.value());
    item["matching"] = matching_config_to_python(entry.matching);
    catalog.append(std::move(item));
  }

  py::list instruments;
  for (const auto& instrument : snapshot.instruments) {
    instruments.append(instrument_snapshot_to_python(instrument));
  }

  py::dict output;
  output["semantics_version"] = integer_object(snapshot.semantics_version);
  output["engine_config"] = std::move(engine_config);
  output["catalog"] = std::move(catalog);
  output["last_sequence"] = integer_object(snapshot.last_sequence.value());
  output["sequence_exhausted"] = snapshot.sequence_exhausted;
  output["active_order_count"] = integer_object(snapshot.active_order_count);
  output["instruments"] = std::move(instruments);
  return output;
}

[[nodiscard]] py::object top_to_python(const std::optional<BookTop>& top) {
  if (!top.has_value()) {
    return py::none{};
  }
  py::dict output;
  output["best_bid"] = optional_top_level_to_python(top->best_bid);
  output["best_ask"] = optional_top_level_to_python(top->best_ask);
  return output;
}

struct VariantColumn final {
  const char* name;
};

inline constexpr VariantColumn variant_columns[]{
    {"accepted_command_type"},
    {"rejected_command_type"},
    {"reject_reason"},
    {"rejected_order_id"},
    {"trade_aggressor_order_id"},
    {"trade_resting_order_id"},
    {"trade_aggressor_client_id"},
    {"trade_resting_client_id"},
    {"trade_aggressor_side"},
    {"trade_execution_price"},
    {"trade_execution_quantity"},
    {"trade_aggressor_remaining"},
    {"trade_resting_remaining"},
    {"rested_order_id"},
    {"rested_client_id"},
    {"rested_side"},
    {"rested_price"},
    {"rested_remaining_quantity"},
    {"canceled_order_id"},
    {"canceled_quantity"},
    {"replaced_old_order_id"},
    {"replaced_new_order_id"},
    {"done_order_id"},
    {"done_reason"},
    {"done_remaining_quantity"},
    {"book_changed_best_bid_price"},
    {"book_changed_best_bid_aggregate_quantity"},
    {"book_changed_best_ask_price"},
    {"book_changed_best_ask_aggregate_quantity"},
};

void append_to_column(py::dict& columns, const char* name, py::object value) {
  py::reinterpret_borrow<py::list>(columns[py::str{name}]).append(std::move(value));
}

template <typename Value>
void append_to_column(py::dict& columns, const char* name, Value value) {
  append_to_column(columns, name, integer_object(value));
}

void set_last_column(py::dict& columns, const char* name, py::object value) {
  auto column = py::reinterpret_borrow<py::list>(columns[py::str{name}]);
  column[column.size() - 1U] = std::move(value);
}

template <typename Value>
void set_last_column(py::dict& columns, const char* name, Value value) {
  set_last_column(columns, name, integer_object(value));
}

void set_present_value(py::dict& columns, const char* name, py::object value) {
  const auto presence_name = std::string{name} + "_present";
  set_last_column(columns, presence_name.c_str(), std::uint8_t{1U});
  set_last_column(columns, name, std::move(value));
}

template <typename Value>
void set_present_value(py::dict& columns, const char* name, Value value) {
  set_present_value(columns, name, integer_object(value));
}

void append_event_columns(py::dict& columns, const domain::Event& event) {
  const auto& header = domain::event_header(event);
  append_to_column(columns, "command_sequence", header.command_sequence.value());
  append_to_column(columns, "event_index", header.event_index);
  append_to_column(columns, "event_type", static_cast<std::uint8_t>(domain::event_type(event)));
  append_to_column(columns, "instrument_id", header.instrument_id.value());

  for (const auto& column : variant_columns) {
    append_to_column(columns, column.name, std::uint64_t{0U});
    const auto presence_name = std::string{column.name} + "_present";
    append_to_column(columns, presence_name.c_str(), std::uint8_t{0U});
  }

  std::visit(
      [&columns](const auto& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, domain::AcceptedEvent>) {
          set_present_value(columns, "accepted_command_type",
                            static_cast<std::uint8_t>(value.command_type));
        } else if constexpr (std::is_same_v<Value, domain::RejectedEvent>) {
          set_present_value(columns, "rejected_command_type",
                            static_cast<std::uint8_t>(value.command_type));
          set_present_value(columns, "reject_reason", static_cast<std::uint16_t>(value.reason));
          if (value.order_id.has_value()) {
            set_present_value(columns, "rejected_order_id", value.order_id->value());
          }
        } else if constexpr (std::is_same_v<Value, domain::TradeEvent>) {
          set_present_value(columns, "trade_aggressor_order_id", value.aggressor_order_id.value());
          set_present_value(columns, "trade_resting_order_id", value.resting_order_id.value());
          set_present_value(columns, "trade_aggressor_client_id",
                            value.aggressor_client_id.value());
          set_present_value(columns, "trade_resting_client_id", value.resting_client_id.value());
          set_present_value(columns, "trade_aggressor_side",
                            static_cast<std::uint8_t>(value.aggressor_side));
          set_present_value(columns, "trade_execution_price", value.execution_price.value());
          set_present_value(columns, "trade_execution_quantity", value.execution_quantity.value());
          set_present_value(columns, "trade_aggressor_remaining",
                            value.aggressor_remaining.value());
          set_present_value(columns, "trade_resting_remaining", value.resting_remaining.value());
        } else if constexpr (std::is_same_v<Value, domain::RestedEvent>) {
          set_present_value(columns, "rested_order_id", value.order_id.value());
          set_present_value(columns, "rested_client_id", value.client_id.value());
          set_present_value(columns, "rested_side", static_cast<std::uint8_t>(value.side));
          set_present_value(columns, "rested_price", value.price.value());
          set_present_value(columns, "rested_remaining_quantity", value.remaining_quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::CanceledEvent>) {
          set_present_value(columns, "canceled_order_id", value.order_id.value());
          set_present_value(columns, "canceled_quantity", value.canceled_quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::ReplacedEvent>) {
          set_present_value(columns, "replaced_old_order_id", value.old_order_id.value());
          set_present_value(columns, "replaced_new_order_id", value.new_order_id.value());
        } else if constexpr (std::is_same_v<Value, domain::DoneEvent>) {
          set_present_value(columns, "done_order_id", value.order_id.value());
          set_present_value(columns, "done_reason", static_cast<std::uint8_t>(value.reason));
          set_present_value(columns, "done_remaining_quantity", value.remaining_quantity.value());
        } else {
          static_assert(std::is_same_v<Value, domain::BookChangedEvent>);
          if (value.best_bid.has_value()) {
            set_present_value(columns, "book_changed_best_bid_price",
                              value.best_bid->price.value());
            set_present_value(columns, "book_changed_best_bid_aggregate_quantity",
                              value.best_bid->aggregate_quantity.value());
          }
          if (value.best_ask.has_value()) {
            set_present_value(columns, "book_changed_best_ask_price",
                              value.best_ask->price.value());
            set_present_value(columns, "book_changed_best_ask_aggregate_quantity",
                              value.best_ask->aggregate_quantity.value());
          }
        }
      },
      event);
}

[[nodiscard]] py::dict column_payload_to_python(const BatchNative& batch) {
  py::dict columns;
  columns["command_event_offsets"] = py::list{};
  columns["command_outcomes"] = py::list{};
  columns["engine_error_present"] = py::list{};
  columns["engine_errors"] = py::list{};
  columns["command_sequence"] = py::list{};
  columns["event_index"] = py::list{};
  columns["event_type"] = py::list{};
  columns["instrument_id"] = py::list{};
  for (const auto& column : variant_columns) {
    columns[py::str{column.name}] = py::list{};
    columns[py::str{std::string{column.name} + "_present"}] = py::list{};
  }

  std::uint64_t event_offset = 0U;
  append_to_column(columns, "command_event_offsets", event_offset);
  for (const auto& result : batch.results) {
    if (result.committed()) {
      append_to_column(columns, "command_outcomes", std::uint8_t{1U});
      append_to_column(columns, "engine_error_present", std::uint8_t{0U});
      append_to_column(columns, "engine_errors", std::uint8_t{0U});
    } else if (result.rejected()) {
      append_to_column(columns, "command_outcomes", std::uint8_t{2U});
      append_to_column(columns, "engine_error_present", std::uint8_t{0U});
      append_to_column(columns, "engine_errors", std::uint8_t{0U});
    } else {
      append_to_column(columns, "command_outcomes", std::uint8_t{3U});
      append_to_column(columns, "engine_error_present", std::uint8_t{1U});
      append_to_column(columns, "engine_errors", static_cast<std::uint8_t>(result.error()));
    }

    if (const auto* event_batch = result.batch(); event_batch != nullptr) {
      for (const auto& event : event_batch->events()) {
        append_event_columns(columns, event);
        ++event_offset;
      }
    }
    append_to_column(columns, "command_event_offsets", event_offset);
  }
  return columns;
}

[[nodiscard]] py::dict batch_to_python(const BatchNative& batch,
                                       bool include_final_state_digest = true) {
  py::object payload;
  if (batch.output == OutputMode::objects) {
    py::list results;
    for (const auto& result : batch.results) {
      results.append(engine_result_to_python(result));
    }
    payload = std::move(results);
  } else if (batch.output == OutputMode::columns) {
    payload = column_payload_to_python(batch);
  } else {
    payload = py::none{};
  }

  py::dict output;
  output["submitted_count"] = integer_object(batch.submitted_count);
  output["processed_count"] = integer_object(batch.processed_count);
  output["committed_count"] = integer_object(batch.committed_count);
  output["rejected_count"] = integer_object(batch.rejected_count);
  output["terminal_error"] = batch.terminal_error == EngineError::none
                                 ? py::object{py::none{}}
                                 : integer_object(static_cast<std::uint8_t>(batch.terminal_error));
  if (include_final_state_digest) {
    output["final_state_digest"] = py::str{batch.final_state_digest.hex()};
  }
  output["output"] = py::str{to_string(batch.output)};
  output["payload"] = std::move(payload);
  return output;
}

[[nodiscard]] py::dict log_error_to_python(const persistence::LogError& error) {
  py::dict output;
  output["category"] = py::str{persistence::to_string(error.category)};
  output["byte_offset"] = integer_object(error.byte_offset);
  output["system_error_value"] = integer_object(error.system_error.value());
  output["system_error_message"] = py::str{error.system_error.message()};
  return output;
}

[[nodiscard]] py::dict snapshot_error_to_python(const persistence::SnapshotError& error) {
  py::dict output;
  output["category"] = py::str{persistence::to_string(error.category)};
  output["byte_offset"] = integer_object(error.byte_offset);
  output["system_error_value"] = integer_object(error.system_error.value());
  output["system_error_message"] = py::str{error.system_error.message()};
  return output;
}

[[nodiscard]] py::object optional_log_error_to_python(const persistence::LogError& error) {
  if (error.ok()) {
    return py::none{};
  }
  return log_error_to_python(error);
}

[[nodiscard]] py::object optional_snapshot_error_to_python(
    const persistence::SnapshotError& error) {
  if (error.ok()) {
    return py::none{};
  }
  return snapshot_error_to_python(error);
}

[[nodiscard]] py::dict log_header_to_python(const persistence::LogHeader& header) {
  py::dict engine_config;
  engine_config["max_total_active_orders"] =
      integer_object(header.engine_config.max_total_active_orders);

  py::list catalog;
  for (const auto& entry : header.catalog) {
    py::dict item;
    item["instrument_id"] = integer_object(entry.instrument_id.value());
    item["max_order_quantity"] = integer_object(entry.max_order_quantity);
    item["tick_increment"] = integer_object(entry.tick_increment.value());
    item["max_active_orders"] = integer_object(entry.max_active_orders);
    catalog.append(std::move(item));
  }

  py::dict output;
  output["format_version"] = integer_object(header.format_version);
  output["semantics_version"] = integer_object(header.semantics_version);
  output["log_id"] = py::str{header.log_id.hex()};
  output["first_sequence"] = integer_object(header.first_sequence.value());
  output["engine_config"] = std::move(engine_config);
  output["catalog"] = std::move(catalog);
  return output;
}

[[nodiscard]] py::dict replay_evidence_to_python(
    const persistence::ReplayEvidenceSummary& evidence) {
  py::dict output;
  output["outcome"] = evidence.outcome.has_value()
                          ? py::object{py::str{persistence::to_string(*evidence.outcome)}}
                          : py::object{py::none{}};
  output["rejection_reason"] =
      evidence.rejection_reason.has_value()
          ? integer_object(static_cast<std::uint16_t>(*evidence.rejection_reason))
          : py::object{py::none{}};
  output["event_count"] = evidence.event_count.has_value() ? integer_object(*evidence.event_count)
                                                           : py::object{py::none{}};
  output["event_digest"] = evidence.event_digest.has_value()
                               ? py::object{py::str{evidence.event_digest->hex()}}
                               : py::object{py::none{}};
  return output;
}

[[nodiscard]] py::dict command_to_python(const domain::Command& command) {
  return std::visit(
      [](const auto& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        py::dict output;
        if constexpr (std::is_same_v<Value, domain::NewOrder>) {
          output["type"] = py::str{"new"};
          output["client_id"] = integer_object(value.client_id.value());
          output["order_id"] = integer_object(value.order_id.value());
          output["instrument_id"] = integer_object(value.instrument_id.value());
          output["side"] = integer_object(static_cast<std::uint8_t>(value.side));
          output["order_type"] = integer_object(static_cast<std::uint8_t>(value.order_type));
          output["time_in_force"] = integer_object(static_cast<std::uint8_t>(value.time_in_force));
          output["limit_price"] = value.limit_price.has_value()
                                      ? integer_object(value.limit_price->value())
                                      : py::object{py::none{}};
          output["quantity"] = integer_object(value.quantity.value());
        } else if constexpr (std::is_same_v<Value, domain::CancelOrder>) {
          output["type"] = py::str{"cancel"};
          output["client_id"] = integer_object(value.client_id.value());
          output["order_id"] = integer_object(value.order_id.value());
          output["instrument_id"] = integer_object(value.instrument_id.value());
        } else {
          static_assert(std::is_same_v<Value, domain::ReplaceOrder>);
          output["type"] = py::str{"replace"};
          output["client_id"] = integer_object(value.client_id.value());
          output["old_order_id"] = integer_object(value.old_order_id.value());
          output["new_order_id"] = integer_object(value.new_order_id.value());
          output["instrument_id"] = integer_object(value.instrument_id.value());
          output["new_limit_price"] = integer_object(value.new_limit_price.value());
          output["new_quantity"] = integer_object(value.new_quantity.value());
        }
        return output;
      },
      command);
}

[[nodiscard]] py::dict replay_divergence_to_python(
    const persistence::ReplayDivergence& divergence) {
  py::list actual_events;
  for (const auto& event : divergence.actual_events) {
    actual_events.append(event_to_python(event));
  }
  py::dict output;
  output["record_offset"] = integer_object(divergence.record_offset);
  output["sequence"] = integer_object(divergence.sequence.value());
  output["category"] = py::str{persistence::to_string(divergence.category)};
  output["command"] = divergence.command.has_value()
                          ? py::object{command_to_python(*divergence.command)}
                          : py::object{py::none{}};
  output["expected"] = replay_evidence_to_python(divergence.expected);
  output["actual"] = replay_evidence_to_python(divergence.actual);
  output["actual_engine_error"] =
      divergence.actual_engine_error == EngineError::none
          ? py::object{py::none{}}
          : integer_object(static_cast<std::uint8_t>(divergence.actual_engine_error));
  output["actual_events"] = std::move(actual_events);
  return output;
}

[[nodiscard]] py::dict replay_report_to_python(const persistence::ReplayReport& report) {
  py::dict output;
  output["kind"] = py::str{"replay"};
  output["mode"] = py::str{persistence::to_string(report.mode)};
  output["tail_policy"] = py::str{persistence::to_string(report.tail_policy)};
  output["tail"] = py::str{persistence::to_string(report.tail)};
  output["header"] = report.header.has_value() ? py::object{log_header_to_python(*report.header)}
                                               : py::object{py::none{}};
  output["last_sequence"] = report.last_sequence.has_value()
                                ? integer_object(report.last_sequence->value())
                                : py::object{py::none{}};
  output["valid_end_offset"] = integer_object(report.valid_end_offset);
  output["records_scanned"] = integer_object(report.records_scanned);
  output["records_replayed"] = integer_object(report.records_replayed);
  output["committed"] = integer_object(report.committed);
  output["rejected"] = integer_object(report.rejected);
  output["used_valid_prefix"] = report.used_valid_prefix;
  output["warning"] = optional_log_error_to_python(report.warning);
  output["error"] = optional_log_error_to_python(report.error);
  output["divergence"] = report.divergence.has_value()
                             ? py::object{replay_divergence_to_python(*report.divergence)}
                             : py::object{py::none{}};
  output["final_state_digest"] = report.final_state_digest.has_value()
                                     ? py::object{py::str{report.final_state_digest->hex()}}
                                     : py::object{py::none{}};
  return output;
}

[[nodiscard]] py::dict snapshot_recovery_report_to_python(
    const persistence::SnapshotRecoveryReport& report) {
  py::list skipped;
  for (const auto& item : report.skipped_snapshots) {
    py::dict value;
    value["path"] = py::cast(item.path);
    value["filename_sequence"] = item.filename_sequence.has_value()
                                     ? integer_object(item.filename_sequence->value())
                                     : py::object{py::none{}};
    value["error"] = snapshot_error_to_python(item.error);
    skipped.append(std::move(value));
  }

  py::dict output;
  output["kind"] = py::str{"snapshot"};
  output["recovery_source"] = py::str{persistence::to_string(report.recovery_source)};
  output["selected_snapshot"] = report.selected_snapshot.has_value()
                                    ? py::object{py::cast(*report.selected_snapshot)}
                                    : py::object{py::none{}};
  output["covered_sequence"] = report.covered_sequence.has_value()
                                   ? integer_object(report.covered_sequence->value())
                                   : py::object{py::none{}};
  output["covered_log_byte_offset"] = report.covered_log_byte_offset.has_value()
                                          ? integer_object(*report.covered_log_byte_offset)
                                          : py::object{py::none{}};
  output["snapshot_state_digest"] = report.snapshot_state_digest.has_value()
                                        ? py::object{py::str{report.snapshot_state_digest->hex()}}
                                        : py::object{py::none{}};
  output["skipped_snapshots"] = std::move(skipped);
  output["snapshot_error"] = optional_snapshot_error_to_python(report.snapshot_error);
  output["replay"] = replay_report_to_python(report.replay);
  return output;
}

[[nodiscard]] py::object recovery_report_to_python(const RecoveryReport& report) {
  return std::visit(
      [](const auto& value) -> py::object {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, std::monostate>) {
          return py::none{};
        } else if constexpr (std::is_same_v<Value, persistence::ReplayReport>) {
          return replay_report_to_python(value);
        } else {
          static_assert(std::is_same_v<Value, persistence::SnapshotRecoveryReport>);
          return snapshot_recovery_report_to_python(value);
        }
      },
      report);
}

[[nodiscard]] py::dict publication_to_python(
    const persistence::SnapshotPublicationResult& publication) {
  py::dict output;
  output["path"] =
      publication.path.empty() ? py::object{py::none{}} : py::object{py::cast(publication.path)};
  output["covered_sequence"] = integer_object(publication.covered_sequence.value());
  output["covered_log_byte_offset"] = integer_object(publication.covered_log_byte_offset);
  output["encoded_bytes"] = integer_object(publication.encoded_bytes);
  output["final_file_visible"] = publication.final_file_visible;
  output["error"] = optional_snapshot_error_to_python(publication.error);
  return output;
}

[[nodiscard]] py::dict empty_failure_details() {
  py::dict details;
  details["category"] = py::str{"none"};
  details["byte_offset"] = integer_object(std::uint64_t{0U});
  details["system_error_value"] = integer_object(0);
  details["system_error_message"] = py::str{};
  details["session_poisoned"] = false;
  details["prefix_batch"] = py::none{};
  details["recovery_report"] = py::none{};
  details["publication_report"] = py::none{};
  return details;
}

[[nodiscard]] py::dict log_failure_details(const persistence::LogError& error,
                                           bool session_poisoned,
                                           py::object prefix_batch = py::none{}) {
  auto details = empty_failure_details();
  details["category"] = py::str{persistence::to_string(error.category)};
  details["byte_offset"] = integer_object(error.byte_offset);
  details["system_error_value"] = integer_object(error.system_error.value());
  details["system_error_message"] = py::str{error.system_error.message()};
  details["session_poisoned"] = session_poisoned;
  details["prefix_batch"] = std::move(prefix_batch);
  return details;
}

[[nodiscard]] py::dict snapshot_failure_details(
    const persistence::SnapshotError& error,
    const std::optional<persistence::SnapshotPublicationResult>& publication = std::nullopt) {
  auto details = empty_failure_details();
  details["category"] = py::str{persistence::to_string(error.category)};
  details["byte_offset"] = integer_object(error.byte_offset);
  details["system_error_value"] = integer_object(error.system_error.value());
  details["system_error_message"] = py::str{error.system_error.message()};
  if (publication.has_value()) {
    details["publication_report"] = publication_to_python(*publication);
  }
  return details;
}

[[nodiscard]] py::dict recovery_failure_details(const RecoveryReport& report) {
  auto details = empty_failure_details();
  details["recovery_report"] = recovery_report_to_python(report);

  std::visit(
      [&details](const auto& value) {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, persistence::ReplayReport>) {
          if (value.error) {
            details["category"] = py::str{persistence::to_string(value.error.category)};
            details["byte_offset"] = integer_object(value.error.byte_offset);
            details["system_error_value"] = integer_object(value.error.system_error.value());
            details["system_error_message"] = py::str{value.error.system_error.message()};
          } else if (value.divergence.has_value()) {
            details["category"] =
                py::str{std::string{"divergence:"} +
                        std::string{persistence::to_string(value.divergence->category)}};
            details["byte_offset"] = integer_object(value.divergence->record_offset);
          }
        } else if constexpr (std::is_same_v<Value, persistence::SnapshotRecoveryReport>) {
          if (value.snapshot_error) {
            details["category"] = py::str{persistence::to_string(value.snapshot_error.category)};
            details["byte_offset"] = integer_object(value.snapshot_error.byte_offset);
            details["system_error_value"] =
                integer_object(value.snapshot_error.system_error.value());
            details["system_error_message"] = py::str{value.snapshot_error.system_error.message()};
          } else if (value.replay.error) {
            details["category"] = py::str{persistence::to_string(value.replay.error.category)};
            details["byte_offset"] = integer_object(value.replay.error.byte_offset);
            details["system_error_value"] = integer_object(value.replay.error.system_error.value());
            details["system_error_message"] = py::str{value.replay.error.system_error.message()};
          } else if (value.replay.divergence.has_value()) {
            details["category"] =
                py::str{std::string{"divergence:"} +
                        std::string{persistence::to_string(value.replay.divergence->category)}};
            details["byte_offset"] = integer_object(value.replay.divergence->record_offset);
          }
        }
      },
      report);
  return details;
}

[[nodiscard]] bool recovery_failure_is_operational(const RecoveryReport& report) noexcept {
  return std::visit(
      [](const auto& value) noexcept {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, persistence::ReplayReport>) {
          return value.error.category == persistence::LogErrorCategory::io_failure;
        } else if constexpr (std::is_same_v<Value, persistence::SnapshotRecoveryReport>) {
          return value.snapshot_error.category == persistence::SnapshotErrorCategory::io_failure ||
                 value.replay.error.category == persistence::LogErrorCategory::io_failure;
        } else {
          static_assert(std::is_same_v<Value, std::monostate>);
          return false;
        }
      },
      report);
}

[[nodiscard]] py::dict read_only_failure_details() {
  auto details = empty_failure_details();
  details["category"] = py::str{"read_only_recovery"};
  return details;
}

[[nodiscard]] py::dict submit_batch_to_python(const std::shared_ptr<NativeEngine>& self,
                                              py::handle commands_value, py::handle output_value,
                                              bool include_final_state_digest) {
  if (self->read_only()) {
    raise_native_error(
        native_read_only_error,
        "read-only valid-prefix recovery cannot submit; repair the torn tail with "
        "'atlas_inspect repair-tail <input> <new-output>' and strictly recover the new log",
        read_only_failure_details());
  }
  if (!include_final_state_digest && self->logged()) {
    throw std::logic_error{"measurement batches require a live in-memory engine"};
  }
  auto commands = convert_commands(commands_value);
  const auto output = parse_output_mode(output_value);
  auto executed = [&]() {
    py::gil_scoped_release release;
    return self->submit_batch(commands, output, include_final_state_digest);
  }();
  if (executed.persistence_failure.has_value()) {
    auto& failure = *executed.persistence_failure;
    auto prefix = batch_to_python(failure.prefix);
    raise_native_error(
        native_persistence_error,
        "command log write failed; this logged session may require recovery",
        log_failure_details(failure.error, failure.session_poisoned, std::move(prefix)));
  }
  return batch_to_python(executed.batch, include_final_state_digest);
}

[[nodiscard]] PyObject* define_exception(py::module_& module, const char* short_name,
                                         PyObject* base = PyExc_RuntimeError) {
  const auto qualified_name = std::string{"atlaslob._native_engine."} + std::string{short_name};
  auto* created = PyErr_NewException(qualified_name.c_str(), base, nullptr);
  if (created == nullptr) {
    throw py::error_already_set{};
  }
  auto exception = py::reinterpret_steal<py::object>(created);
  module.attr(short_name) = exception;
  // The binding's error paths use process-global raw pointers. Retain one
  // deliberate owning reference so deleting a private module attribute cannot
  // invalidate an exception type while native code may still raise it.
  Py_INCREF(exception.ptr());
  return exception.ptr();
}

}  // namespace
}  // namespace atlaslob::python

PYBIND11_MODULE(_native_engine, module) {
  using atlaslob::MultiInstrumentEngineConfig;
  using atlaslob::domain::InstrumentId;
  using atlaslob::python::BatchExecution;
  using atlaslob::python::NativeEngine;
  using atlaslob::python::OutputMode;
  using namespace atlaslob::python;

  module.doc() = "Private native AtlasLOB engine binding";
  module.attr("BINDING_ABI") = py::int_{binding_abi};
  module.attr("SEMANTICS_VERSION") = py::int_{atlaslob::atlaslob_semantics_version};

  native_persistence_error = define_exception(module, "NativePersistenceError");
  native_recovery_error = define_exception(module, "NativeRecoveryError");
  native_snapshot_error = define_exception(module, "NativeSnapshotError");
  native_read_only_error = define_exception(module, "NativeReadOnlyError");

  py::class_<NativeEngine, std::shared_ptr<NativeEngine>>(module, "NativeEngine",
                                                          py::release_gil_before_calling_cpp_dtor{})
      .def(
          py::init([](py::handle catalog_value, py::handle max_total_value) {
            auto catalog = convert_catalog(catalog_value);
            const auto config = MultiInstrumentEngineConfig{
                .max_total_active_orders = require_size(max_total_value, "max_total_active_orders"),
            };
            py::gil_scoped_release release;
            return NativeEngine::create_live(std::move(catalog), config);
          }),
          py::arg("catalog"), py::arg("max_total_active_orders"))
      .def_static(
          "create_logged",
          [](py::handle path_value, py::handle catalog_value, py::handle max_total_value,
             py::handle durability_value) {
            const auto path = require_path(path_value, "path");
            auto catalog = convert_catalog(catalog_value);
            const auto config = MultiInstrumentEngineConfig{
                .max_total_active_orders = require_size(max_total_value, "max_total_active_orders"),
            };
            const auto options = atlaslob::persistence::LoggedEngineOptions{
                .durability = parse_durability(durability_value),
                .codec_limits = {},
            };

            auto opened = [&]() {
              py::gil_scoped_release release;
              return NativeEngine::create_logged(path, std::move(catalog), config, options);
            }();
            if (!opened.engine) {
              raise_native_error(native_persistence_error,
                                 "unable to create logged AtlasLOB engine",
                                 log_failure_details(opened.error, false));
            }
            return std::move(opened.engine);
          },
          py::arg("path"), py::arg("catalog"), py::arg("max_total_active_orders"),
          py::arg("durability") = "sync_each_record")
      .def_static(
          "recover",
          [](py::handle log_path_value, py::object snapshot_path_value,
             py::object snapshot_directory_value, py::handle mode_value,
             py::handle tail_policy_value, py::handle durability_value) {
            const auto log_path = require_path(log_path_value, "log_path");
            std::optional<std::filesystem::path> snapshot_path;
            std::optional<std::filesystem::path> snapshot_directory;
            if (!snapshot_path_value.is_none()) {
              snapshot_path = require_path(snapshot_path_value, "snapshot_path");
            }
            if (!snapshot_directory_value.is_none()) {
              snapshot_directory = require_path(snapshot_directory_value, "snapshot_directory");
            }
            if (snapshot_path.has_value() && snapshot_directory.has_value()) {
              throw py::value_error{"snapshot_path and snapshot_directory are mutually exclusive"};
            }

            const auto replay_options = atlaslob::persistence::ReplayOptions{
                .mode = parse_replay_mode(mode_value),
                .tail_policy = parse_tail_policy(tail_policy_value),
                .codec_limits = {},
                .invariant_interval = 1024U,
            };
            const auto logged_options = atlaslob::persistence::LoggedEngineOptions{
                .durability = parse_durability(durability_value),
                .codec_limits = {},
            };

            auto recovered = [&]() {
              py::gil_scoped_release release;
              return NativeEngine::recover(log_path, snapshot_path, snapshot_directory,
                                           replay_options, logged_options);
            }();
            if (!recovered.engine) {
              if (!recovered.failure.has_value()) {
                throw std::logic_error{"native recovery returned neither engine nor failure"};
              }
              auto details = recovery_failure_details(recovered.failure->report);
              if (recovery_failure_is_operational(recovered.failure->report)) {
                raise_native_error(native_persistence_error,
                                   "operational I/O failure while recovering AtlasLOB engine",
                                   std::move(details));
              }
              raise_native_error(native_recovery_error, "unable to recover AtlasLOB engine",
                                 std::move(details));
            }
            return std::move(recovered.engine);
          },
          py::arg("log_path"), py::arg("snapshot_path") = py::none{},
          py::arg("snapshot_dir") = py::none{}, py::arg("mode") = "verify",
          py::arg("tail_policy") = "strict", py::arg("durability") = "sync_each_record")
      .def(
          "submit",
          [](const std::shared_ptr<NativeEngine>& self, py::handle command_value) {
            if (self->read_only()) {
              raise_native_error(
                  native_read_only_error,
                  "read-only valid-prefix recovery cannot submit; repair the torn tail with "
                  "'atlas_inspect repair-tail <input> <new-output>' and strictly recover the "
                  "new log",
                  read_only_failure_details());
            }
            std::vector<atlaslob::domain::Command> commands;
            commands.reserve(1U);
            commands.push_back(convert_command(command_value));
            auto executed = [&]() {
              py::gil_scoped_release release;
              return self->submit_batch(commands, OutputMode::objects);
            }();
            if (executed.persistence_failure.has_value()) {
              auto& failure = *executed.persistence_failure;
              auto prefix = batch_to_python(failure.prefix);
              raise_native_error(
                  native_persistence_error,
                  "command log write failed; this logged session may require recovery",
                  log_failure_details(failure.error, failure.session_poisoned, std::move(prefix)));
            }
            if (executed.batch.results.size() != 1U) {
              throw std::logic_error{"single submission did not publish exactly one result"};
            }
            return engine_result_to_python(executed.batch.results.front());
          },
          py::arg("command"))
      .def(
          "submit_batch",
          [](const std::shared_ptr<NativeEngine>& self, py::handle commands_value,
             py::handle output_value) {
            return submit_batch_to_python(self, commands_value, output_value, true);
          },
          py::arg("commands"), py::arg("output") = "objects")
      .def(
          "_submit_batch_for_measurement",
          [](const std::shared_ptr<NativeEngine>& self, py::handle commands_value,
             py::handle output_value) {
            return submit_batch_to_python(self, commands_value, output_value, false);
          },
          py::arg("commands"), py::arg("output") = "objects")
      .def(
          "top",
          [](const std::shared_ptr<NativeEngine>& self, py::handle instrument_id_value) {
            const auto instrument_id =
                InstrumentId{require_u32(instrument_id_value, "instrument_id")};
            auto top = [&]() {
              py::gil_scoped_release release;
              return self->top(instrument_id);
            }();
            return top_to_python(top);
          },
          py::arg("instrument_id"))
      .def(
          "snapshot",
          [](const std::shared_ptr<NativeEngine>& self,
             py::object instrument_id_value) -> py::object {
            if (instrument_id_value.is_none()) {
              auto snapshot = [&]() {
                py::gil_scoped_release release;
                return self->engine_snapshot();
              }();
              return engine_snapshot_to_python(snapshot);
            }
            const auto instrument_id =
                InstrumentId{require_u32(instrument_id_value, "instrument_id")};
            auto snapshot = [&]() {
              py::gil_scoped_release release;
              return self->instrument_snapshot(instrument_id);
            }();
            if (!snapshot.has_value()) {
              return py::none{};
            }
            return instrument_snapshot_to_python(*snapshot);
          },
          py::arg("instrument_id") = py::none{})
      .def("state_digest",
           [](const std::shared_ptr<NativeEngine>& self) {
             auto digest = [&]() {
               py::gil_scoped_release release;
               return self->state_digest();
             }();
             return digest.hex();
           })
      .def(
          "write_snapshot",
          [](const std::shared_ptr<NativeEngine>& self, py::handle directory_value) {
            if (self->read_only()) {
              raise_native_error(
                  native_read_only_error,
                  "read-only valid-prefix recovery cannot publish snapshots; repair the torn "
                  "tail and strictly recover the new log",
                  read_only_failure_details());
            }
            const auto directory = require_path(directory_value, "directory");
            auto publication = [&]() {
              py::gil_scoped_release release;
              return self->write_snapshot(directory);
            }();
            if (!publication) {
              raise_native_error(native_snapshot_error, "unable to publish AtlasLOB snapshot",
                                 snapshot_failure_details(publication.error, publication));
            }
            return publication_to_python(publication);
          },
          py::arg("directory"))
      .def_property_readonly(
          "logged", [](const std::shared_ptr<NativeEngine>& self) { return self->logged(); })
      .def_property_readonly(
          "read_only", [](const std::shared_ptr<NativeEngine>& self) { return self->read_only(); })
      .def_property_readonly("poisoned",
                             [](const std::shared_ptr<NativeEngine>& self) {
                               py::gil_scoped_release release;
                               return self->poisoned();
                             })
      .def_property_readonly("recovery_report", [](const std::shared_ptr<NativeEngine>& self) {
        return recovery_report_to_python(self->recovery_report());
      });
}
