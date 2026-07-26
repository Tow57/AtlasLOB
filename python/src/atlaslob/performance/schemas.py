"""Strict, canonical Phase 5 performance-evidence schemas.

This module is intentionally independent of :mod:`atlaslob.engine`.  Evidence
can therefore be inspected and analysed on hosts where the native extension is
not installed.
"""

from __future__ import annotations

import hashlib
import json
import re
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from decimal import Decimal, localcontext
from pathlib import Path, PurePosixPath
from typing import Final, TypeAlias, TypeVar, cast

from atlaslob.domain import U64_MAX

WORKLOAD_SCHEMA: Final = "ATLAS_BENCH_WORKLOAD_V1"
ENVIRONMENT_SCHEMA: Final = "ATLAS_BENCH_ENV_V1"
OBSERVATION_SCHEMA: Final = "ATLAS_BENCH_OBSERVATION_V1"
REPORT_SCHEMA: Final = "ATLAS_BENCH_REPORT_V1"
WORKLOAD_DOCUMENT_MAX_BYTES: Final = 1024 * 1024
ENVIRONMENT_DOCUMENT_MAX_BYTES: Final = 1024 * 1024
OBSERVATION_DOCUMENT_MAX_BYTES: Final = 16 * 1024 * 1024
REPORT_DOCUMENT_MAX_BYTES: Final = 64 * 1024 * 1024
MAX_CATALOG_ENTRIES: Final = 4_096
MAX_COMMANDS_PER_WORKLOAD: Final = 100_000_000
MAX_OPERATION_KINDS: Final = 64
MAX_PARAMETERS: Final = 64
MAX_LATENCY_SAMPLES: Final = 200_000
MAX_AFFINITY_CPUS: Final = 16_384
MAX_COMPILER_FLAGS: Final = 16_384
MAX_BUILD_TARGET_PROFILES: Final = 16_384
MAX_LIMITATIONS: Final = 1_024
MAX_REPORT_WORKLOADS: Final = 4_096
MAX_REPORT_ENVIRONMENTS: Final = 4_096
MAX_REPORT_OBSERVATIONS: Final = 500_000
MAX_REPORT_GROUPS: Final = 65_536
MAX_REPORT_COMPARISONS: Final = 32_768
MAX_REPORT_EXPERIMENTS: Final = 999
MAX_PUBLIC_TEXT_BYTES: Final = 4_096
BOUNDARIES: Final = frozenset(
    {
        "core_throughput",
        "core_latency",
        "core_allocation",
        "core_construction",
        "core_preload",
        "core_setup_allocation",
        "replay_fast",
        "replay_verify",
        "python_objects",
        "python_columns",
        "python_summary",
    }
)
TARGET_METRIC_BY_BOUNDARY: Final = {
    "core_throughput": ("commands_per_second", "higher_is_better"),
    "core_latency": ("p99_ns", "lower_is_better"),
    "core_allocation": ("allocation_count", "lower_is_better"),
    "core_construction": ("elapsed_ns", "lower_is_better"),
    "core_preload": ("commands_per_second", "higher_is_better"),
    "core_setup_allocation": ("allocation_count", "lower_is_better"),
    "replay_fast": ("commands_per_second", "higher_is_better"),
    "replay_verify": ("commands_per_second", "higher_is_better"),
    "python_objects": ("commands_per_second", "higher_is_better"),
    "python_columns": ("commands_per_second", "higher_is_better"),
    "python_summary": ("commands_per_second", "higher_is_better"),
}
BENCHMARK_WORKLOAD_IDS: Final = frozenset(
    tuple(f"W{index:02d}" for index in range(1, 11)) + ("W12",)
)

_DIGEST = re.compile(r"[0-9a-f]{64}\Z")
_IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\Z")
_HOST_ALIAS = re.compile(r"(?=.{1,64}\Z)[a-z0-9]+(?:-[a-z0-9]+)*\Z")
_MEASUREMENT_VALUE = re.compile(r"[A-Za-z0-9_.+,-]{1,256}\Z")
_DECIMAL = re.compile(r"(?:0|[1-9][0-9]*)\Z")
_RATE = re.compile(r"(?:0|[1-9][0-9]*)(?:\.[0-9]+)?\Z")
_IPV4 = re.compile(r"(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])")
_IPV6 = re.compile(r"(?<![0-9A-Fa-f])(?:[0-9A-Fa-f]{0,4}:){2,}[0-9A-Fa-f]{0,4}(?![0-9A-Fa-f])")
_WINDOWS_PATH = re.compile(r"(?:[A-Za-z]:[\\/]|\\\\)")
_UNIX_HOME = re.compile(r"/(?:home|Users)/[^/\s\"']+")

JsonValue: TypeAlias = None | bool | int | str | list["JsonValue"] | dict[str, "JsonValue"]
T = TypeVar("T")


@dataclass(frozen=True, slots=True)
class CatalogEntry:
    instrument_id: int
    max_order_quantity: int
    tick_increment: int
    max_active_orders: int

    def __post_init__(self) -> None:
        _require_uint("instrument_id", self.instrument_id, (1 << 32) - 1, nonzero=True)
        _require_uint("max_order_quantity", self.max_order_quantity, U64_MAX, nonzero=True)
        if (
            isinstance(self.tick_increment, bool)
            or not isinstance(self.tick_increment, int)
            or not 1 <= self.tick_increment <= (1 << 63) - 1
        ):
            raise ValueError("tick_increment must be a positive i64")
        _require_uint("max_active_orders", self.max_active_orders, U64_MAX)


@dataclass(frozen=True, slots=True)
class WorkloadManifest:
    workload_id: str
    generator_version: int
    seed: int
    catalog: tuple[CatalogEntry, ...]
    max_total_active_orders: int
    operation_distribution: tuple[tuple[str, int], ...]
    parameters: tuple[tuple[str, str], ...]
    preload_commands: int
    warmup_commands: int
    measured_commands: int
    after_preload_active_order_count: int
    measured_start_active_order_count: int
    final_active_order_count: int
    stream_file: str
    stream_sha256: str
    expected_events: int
    expected_committed: int
    expected_rejected: int
    expected_engine_errors: int
    expected_event_digest: str
    expected_final_digest: str
    expected_empty_state_digest: str
    expected_preload_events: int
    expected_preload_committed: int
    expected_preload_rejected: int
    expected_preload_engine_errors: int
    expected_preload_event_digest: str
    expected_preload_state_digest: str
    timed_input_file: str | None
    timed_input_kind: str
    timed_input_sha256: str | None
    timed_input_records: int
    cache_policy: str

    def __post_init__(self) -> None:
        _require_identifier("workload_id", self.workload_id)
        if self.workload_id not in BENCHMARK_WORKLOAD_IDS:
            raise ValueError("workload_id is outside the V1 benchmark catalog")
        _require_uint("generator_version", self.generator_version, U64_MAX, nonzero=True)
        _require_uint("seed", self.seed, U64_MAX)
        if not 1 <= len(self.catalog) <= MAX_CATALOG_ENTRIES:
            raise ValueError("catalog must contain between 1 and 4096 instruments")
        identifiers = tuple(entry.instrument_id for entry in self.catalog)
        if identifiers != tuple(sorted(identifiers)) or len(identifiers) != len(set(identifiers)):
            raise ValueError("catalog must have unique ascending instrument IDs")
        _require_uint("max_total_active_orders", self.max_total_active_orders, U64_MAX)
        names = tuple(name for name, _ in self.operation_distribution)
        if len(names) > MAX_OPERATION_KINDS:
            raise ValueError("operation_distribution exceeds its entry limit")
        if names != tuple(sorted(names)) or len(names) != len(set(names)):
            raise ValueError("operation_distribution must have unique sorted names")
        if any(not name or not name.isascii() or len(name) > 128 for name in names):
            raise ValueError("operation names must be nonempty ASCII")
        for name, count in self.operation_distribution:
            _require_uint(f"operation_distribution.{name}", count, U64_MAX)
        _require_parameter_pairs("parameters", self.parameters)
        for count_name, count_value in (
            ("preload_commands", self.preload_commands),
            ("warmup_commands", self.warmup_commands),
            ("measured_commands", self.measured_commands),
            ("after_preload_active_order_count", self.after_preload_active_order_count),
            ("measured_start_active_order_count", self.measured_start_active_order_count),
            ("final_active_order_count", self.final_active_order_count),
            ("expected_events", self.expected_events),
            ("expected_committed", self.expected_committed),
            ("expected_rejected", self.expected_rejected),
            ("expected_engine_errors", self.expected_engine_errors),
            ("expected_preload_events", self.expected_preload_events),
            ("expected_preload_committed", self.expected_preload_committed),
            ("expected_preload_rejected", self.expected_preload_rejected),
            ("expected_preload_engine_errors", self.expected_preload_engine_errors),
            ("timed_input_records", self.timed_input_records),
        ):
            _require_uint(count_name, count_value, U64_MAX)
        if self.measured_commands == 0:
            raise ValueError("measured_commands must be nonzero")
        if any(
            count > self.max_total_active_orders
            for count in (
                self.after_preload_active_order_count,
                self.measured_start_active_order_count,
                self.final_active_order_count,
            )
        ):
            raise ValueError("workload active-order evidence exceeds global capacity")
        total = self.preload_commands + self.warmup_commands + self.measured_commands
        if total > MAX_COMMANDS_PER_WORKLOAD:
            raise ValueError("workload command total exceeds the V1 study bound")
        if sum(count for _, count in self.operation_distribution) != self.measured_commands:
            raise ValueError("operation distribution does not cover the measured region")
        outcomes = self.expected_committed + self.expected_rejected + self.expected_engine_errors
        if outcomes != self.measured_commands:
            raise ValueError("expected outcomes do not cover the measured region")
        preload_outcomes = (
            self.expected_preload_committed
            + self.expected_preload_rejected
            + self.expected_preload_engine_errors
        )
        if preload_outcomes != self.preload_commands:
            raise ValueError("preload outcomes do not cover the preload region")
        if (
            not self.stream_file
            or Path(self.stream_file).name != self.stream_file
            or not self.stream_file.isascii()
        ):
            raise ValueError("stream_file must be an ASCII basename")
        _require_digest("stream_sha256", self.stream_sha256)
        _require_digest("expected_event_digest", self.expected_event_digest)
        _require_digest("expected_final_digest", self.expected_final_digest)
        _require_digest("expected_empty_state_digest", self.expected_empty_state_digest)
        _require_digest("expected_preload_event_digest", self.expected_preload_event_digest)
        _require_digest("expected_preload_state_digest", self.expected_preload_state_digest)
        if self.workload_id == "W10":
            if (
                self.preload_commands != 0
                or self.warmup_commands != 0
                or self.timed_input_file is None
                or Path(self.timed_input_file).name != self.timed_input_file
                or not self.timed_input_file.isascii()
                or not self.timed_input_file.endswith(".atlslg")
                or self.timed_input_kind != "atlslg01"
                or self.timed_input_sha256 is None
                or self.timed_input_records == 0
                or self.timed_input_records != self.measured_commands
                or self.cache_policy != "warm_page_cache"
            ):
                raise ValueError("W10 requires complete canonical replay input metadata")
            _require_digest("timed_input_sha256", self.timed_input_sha256)
        elif (
            self.timed_input_file is not None
            or self.timed_input_kind != "none"
            or self.timed_input_sha256 is not None
            or self.timed_input_records != 0
            or self.cache_policy != "none"
        ):
            raise ValueError("non-W10 workloads cannot carry replay input metadata")

    @property
    def command_count(self) -> int:
        return self.preload_commands + self.warmup_commands + self.measured_commands


@dataclass(frozen=True, slots=True)
class EnvironmentManifest:
    commit: str
    tag: str | None
    dirty: bool
    binary_sha256: str
    os: str
    kernel: str
    host_class: str
    cpu_model: str
    architecture: str
    physical_cores: int
    logical_cpus: int
    microcode: str
    memory_bytes: int
    compiler: str
    compiler_flags: tuple[str, ...]
    build_receipt_sha256: str
    build_target_profiles: tuple[tuple[str, str], ...]
    build_type: str
    optimization: str
    ndebug: bool
    frame_pointers: bool
    invariants: bool
    sanitizers: bool
    lto: bool
    benchmark_build: bool
    warnings_as_errors: bool
    debug_symbols: bool
    cxx20: bool
    release_flags_locked: bool
    affinity: tuple[int, ...]
    pinned_cpu: int | None
    smt_sibling_idle: bool | None
    numa_nodes: int
    numa_cpu_policy: str
    numa_memory_policy: str
    filesystem: str
    storage_class: str
    governor: str
    turbo: str
    smt: str
    virtualization: str
    perf_version: str | None
    runtime_kind: str
    python_implementation: str | None
    python_version: str | None
    python_cache_tag: str | None
    atlaslob_version: str | None
    interpreter_sha256: str | None
    wheel_sha256: str | None
    package_sha256: str | None
    wrapper_sha256: str | None
    harness_sha256: str | None
    classification: str
    host_context_sha256: str
    limitations: tuple[str, ...]

    def __post_init__(self) -> None:
        _require_identifier("commit", self.commit, allow_long=True)
        if self.tag is not None:
            _require_identifier("tag", self.tag)
        if not isinstance(self.dirty, bool):
            raise TypeError("dirty must be bool")
        _require_digest("binary_sha256", self.binary_sha256)
        for text_name, text_value in (
            ("os", self.os),
            ("kernel", self.kernel),
            ("host_class", self.host_class),
            ("cpu_model", self.cpu_model),
            ("architecture", self.architecture),
            ("microcode", self.microcode),
            ("compiler", self.compiler),
            ("numa_cpu_policy", self.numa_cpu_policy),
            ("numa_memory_policy", self.numa_memory_policy),
            ("filesystem", self.filesystem),
            ("storage_class", self.storage_class),
            ("governor", self.governor),
            ("turbo", self.turbo),
            ("smt", self.smt),
            ("virtualization", self.virtualization),
        ):
            _require_public_text(text_name, text_value)
        if _HOST_ALIAS.fullmatch(self.host_class) is None:
            raise ValueError("host_class must be a sanitized lowercase alias")
        if _HOST_ALIAS.fullmatch(self.storage_class) is None:
            raise ValueError("storage_class must be a sanitized lowercase alias")
        if self.virtualization not in {"none", "detected", "unknown"}:
            raise ValueError("virtualization is outside the V1 vocabulary")
        if self.perf_version is not None:
            _require_public_text("perf_version", self.perf_version)
        _require_digest("build_receipt_sha256", self.build_receipt_sha256)
        _require_profile_pairs("build_target_profiles", self.build_target_profiles)
        if self.runtime_kind not in {"native", "cpython"}:
            raise ValueError("runtime_kind is outside the V1 vocabulary")
        python_text = (
            self.python_implementation,
            self.python_version,
            self.python_cache_tag,
            self.atlaslob_version,
        )
        python_digests = (
            self.interpreter_sha256,
            self.wheel_sha256,
            self.package_sha256,
            self.wrapper_sha256,
            self.harness_sha256,
        )
        if self.runtime_kind == "native":
            if any(item is not None for item in (*python_text, *python_digests)):
                raise ValueError("native environments cannot carry CPython identity")
        else:
            if any(item is None for item in (*python_text, *python_digests)):
                raise ValueError("CPython environments require complete runtime identity")
            for name, item in zip(
                (
                    "python_implementation",
                    "python_version",
                    "python_cache_tag",
                    "atlaslob_version",
                ),
                python_text,
                strict=True,
            ):
                assert item is not None
                _require_public_text(name, item)
            for name, item in zip(
                (
                    "interpreter_sha256",
                    "wheel_sha256",
                    "package_sha256",
                    "wrapper_sha256",
                    "harness_sha256",
                ),
                python_digests,
                strict=True,
            ):
                assert item is not None
                _require_digest(name, item)
        if tuple(sorted(self.limitations)) != self.limitations or len(set(self.limitations)) != len(
            self.limitations
        ):
            raise ValueError("environment limitations must be unique and sorted")
        if len(self.limitations) > MAX_LIMITATIONS:
            raise ValueError("environment limitations exceed their entry limit")
        _require_uint("physical_cores", self.physical_cores, MAX_AFFINITY_CPUS, nonzero=True)
        _require_uint("logical_cpus", self.logical_cpus, MAX_AFFINITY_CPUS, nonzero=True)
        if self.physical_cores > self.logical_cpus:
            raise ValueError("physical_cores cannot exceed logical_cpus")
        _require_uint("memory_bytes", self.memory_bytes, U64_MAX, nonzero=True)
        _require_uint("numa_nodes", self.numa_nodes, (1 << 32) - 1, nonzero=True)
        if (
            not self.affinity
            or len(self.affinity) > MAX_AFFINITY_CPUS
            or tuple(sorted(self.affinity)) != self.affinity
            or len(set(self.affinity)) != len(self.affinity)
        ):
            raise ValueError("affinity must be a nonempty sorted set")
        for cpu in self.affinity:
            _require_uint("affinity CPU", cpu, (1 << 32) - 1)
            if cpu >= self.logical_cpus:
                raise ValueError("affinity CPU must be below logical_cpus")
        if len(self.compiler_flags) > MAX_COMPILER_FLAGS:
            raise ValueError("compiler_flags exceed their entry limit")
        for flag in self.compiler_flags:
            _require_public_text("compiler flag", flag)
        if self.build_type not in {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"}:
            raise ValueError("build_type is outside the supported vocabulary")
        _require_public_text("optimization", self.optimization)
        for boolean_name, boolean_value in (
            ("ndebug", self.ndebug),
            ("frame_pointers", self.frame_pointers),
            ("invariants", self.invariants),
            ("sanitizers", self.sanitizers),
            ("lto", self.lto),
            ("benchmark_build", self.benchmark_build),
            ("warnings_as_errors", self.warnings_as_errors),
            ("debug_symbols", self.debug_symbols),
            ("cxx20", self.cxx20),
            ("release_flags_locked", self.release_flags_locked),
        ):
            if not isinstance(boolean_value, bool):
                raise TypeError(f"{boolean_name} must be bool")
        if self.pinned_cpu is not None:
            _require_uint("pinned_cpu", self.pinned_cpu, (1 << 32) - 1, nonzero=True)
            if self.affinity != (self.pinned_cpu,):
                raise ValueError("pinned_cpu requires affinity to contain exactly that CPU")
        if self.smt_sibling_idle is not None and not isinstance(self.smt_sibling_idle, bool):
            raise TypeError("smt_sibling_idle must be bool or null")
        for limitation in self.limitations:
            _require_public_text("limitation", limitation)
        if self.classification not in {"official", "exploratory"}:
            raise ValueError("classification must be official or exploratory")
        expected_context = environment_context_digest(self)
        if self.host_context_sha256 == "":
            object.__setattr__(self, "host_context_sha256", expected_context)
        else:
            _require_digest("host_context_sha256", self.host_context_sha256)
        if self.host_context_sha256 != expected_context:
            raise ValueError("host_context_sha256 differs from canonical host/build context")
        if self.classification == "official":
            if self.dirty:
                raise ValueError("an official environment must have a clean checkout")
            if self.virtualization != "none":
                raise ValueError("an official environment must be bare metal")
            if self.architecture != "x86_64" or not self.os.startswith("Ubuntu 24.04"):
                raise ValueError("official results require Ubuntu 24.04 x86_64")
            if self.pinned_cpu is None or self.pinned_cpu == 0 or self.smt_sibling_idle is not True:
                raise ValueError("official results require one nonzero CPU with idle SMT sibling")
            if self.perf_version is None or any(
                value in {"", "unknown", "unavailable"}
                for value in (
                    self.kernel,
                    self.cpu_model,
                    self.microcode,
                    self.governor,
                    self.turbo,
                    self.smt,
                    self.numa_cpu_policy,
                    self.numa_memory_policy,
                    self.filesystem,
                    self.storage_class,
                )
            ):
                raise ValueError("official results require every frozen host fact")
            if (
                self.build_type != "Release"
                or self.optimization != "O3"
                or not self.ndebug
                or not self.frame_pointers
                or self.invariants
                or self.sanitizers
                or self.lto
                or not self.warnings_as_errors
                or not self.debug_symbols
                or not self.cxx20
                or not self.release_flags_locked
            ):
                raise ValueError("official results require a release C++20 runtime build")
            if self.runtime_kind == "native" and (not self.benchmark_build):
                raise ValueError("official native results require the frozen benchmark build")


@dataclass(frozen=True, slots=True)
class AllocationMetrics:
    allocation_count: int
    deallocation_count: int
    allocated_bytes: int
    live_bytes: int
    peak_live_bytes: int

    def __post_init__(self) -> None:
        for count_name, count_value in (
            ("allocation_count", self.allocation_count),
            ("deallocation_count", self.deallocation_count),
            ("allocated_bytes", self.allocated_bytes),
            ("live_bytes", self.live_bytes),
            ("peak_live_bytes", self.peak_live_bytes),
        ):
            _require_uint(count_name, count_value, U64_MAX)
        if self.live_bytes > self.peak_live_bytes:
            raise ValueError("live_bytes cannot exceed peak_live_bytes")


@dataclass(frozen=True, slots=True)
class Observation:
    boundary: str
    timed_input_kind: str
    timed_input_sha256: str | None
    measurement_parameters: tuple[tuple[str, str], ...]
    workload_id: str
    workload_sha256: str
    workload_manifest_sha256: str
    binary_sha256: str
    environment_sha256: str
    host_context_sha256: str
    suite_label: str
    run_label: str
    variant: str
    block_index: int
    block_position: int
    preload_commands: int
    warmup_commands: int
    commands: int
    events: int
    committed: int
    rejected: int
    engine_errors: int
    elapsed_ns: int
    rss_before_bytes: int
    rss_after_bytes: int
    peak_rss_bytes: int
    latency_ns: tuple[int, ...] | None
    allocations: AllocationMetrics | None
    event_digest: str
    final_digest: str
    valid: bool
    failure_reason: str | None

    def __post_init__(self) -> None:
        if self.boundary not in BOUNDARIES:
            raise ValueError("boundary is outside the V1 vocabulary")
        _validate_timed_input(self.boundary, self.timed_input_kind, self.timed_input_sha256)
        _require_parameter_pairs("measurement_parameters", self.measurement_parameters)
        _require_identifier("workload_id", self.workload_id)
        for digest_name, digest_value in (
            ("workload_sha256", self.workload_sha256),
            ("workload_manifest_sha256", self.workload_manifest_sha256),
            ("binary_sha256", self.binary_sha256),
            ("environment_sha256", self.environment_sha256),
            ("host_context_sha256", self.host_context_sha256),
            ("event_digest", self.event_digest),
            ("final_digest", self.final_digest),
        ):
            _require_digest(digest_name, digest_value)
        _require_identifier("run_label", self.run_label)
        _require_suite_label(self.suite_label)
        if self.variant not in {"standalone", "baseline", "candidate"}:
            raise ValueError("variant is outside the V1 vocabulary")
        for count_name, count_value in (
            ("block_index", self.block_index),
            ("block_position", self.block_position),
            ("preload_commands", self.preload_commands),
            ("warmup_commands", self.warmup_commands),
            ("commands", self.commands),
            ("events", self.events),
            ("committed", self.committed),
            ("rejected", self.rejected),
            ("engine_errors", self.engine_errors),
            ("elapsed_ns", self.elapsed_ns),
            ("rss_before_bytes", self.rss_before_bytes),
            ("rss_after_bytes", self.rss_after_bytes),
            ("peak_rss_bytes", self.peak_rss_bytes),
        ):
            _require_uint(count_name, count_value, U64_MAX)
        if self.variant == "standalone":
            if self.block_index != 0 or self.block_position != 0:
                raise ValueError("standalone observations must use block index/position zero")
        elif self.block_index == 0 or not 1 <= self.block_position <= 4:
            raise ValueError(
                "A/B observations require a nonzero block and positions one through four"
            )
        if self.committed + self.rejected + self.engine_errors != self.commands:
            raise ValueError("outcome counts do not cover commands")
        if self.boundary == "core_latency":
            if self.latency_ns is None or self.allocations is not None:
                raise ValueError("latency observations require only latency_ns")
            if len(self.latency_ns) > MAX_LATENCY_SAMPLES:
                raise ValueError("latency observations exceed the 200000-sample cap")
            if self.valid is True and not self.latency_ns:
                raise ValueError("valid latency observations require at least one sample")
            for sample in self.latency_ns:
                _require_uint("latency sample", sample, U64_MAX)
        elif self.boundary in {"core_allocation", "core_setup_allocation"}:
            if self.allocations is None or self.latency_ns is not None:
                raise ValueError("allocation observations require only allocations")
            if self.elapsed_ns != 0:
                raise ValueError("allocation observations are not timing evidence")
        elif self.latency_ns is not None or self.allocations is not None:
            raise ValueError("this boundary cannot contain latency or allocation payloads")
        if not isinstance(self.valid, bool):
            raise TypeError("valid must be bool")
        if self.valid != (self.failure_reason is None):
            raise ValueError("failure_reason must be null exactly when valid is true")
        if self.failure_reason is not None:
            _require_public_text("failure_reason", self.failure_reason)
        if self.valid:
            if (
                self.boundary not in {"core_allocation", "core_setup_allocation"}
                and self.elapsed_ns == 0
            ):
                raise ValueError("valid timing observations require nonzero elapsed_ns")
            if any(
                value != 0
                for value in (
                    self.rss_before_bytes,
                    self.rss_after_bytes,
                    self.peak_rss_bytes,
                )
            ) and self.peak_rss_bytes < max(self.rss_before_bytes, self.rss_after_bytes):
                raise ValueError("peak_rss_bytes cannot be below observed process RSS")


@dataclass(frozen=True, slots=True)
class ScalarStatistics:
    minimum: str
    maximum: str
    median: str
    mad: str
    iqr: str

    def __post_init__(self) -> None:
        _require_signed_rate("minimum", self.minimum)
        _require_signed_rate("maximum", self.maximum)
        _require_signed_rate("median", self.median)
        _require_rate("mad", self.mad)
        _require_rate("iqr", self.iqr)
        if Decimal(self.minimum) > Decimal(self.maximum):
            raise ValueError("scalar statistic minimum exceeds maximum")
        if not Decimal(self.minimum) <= Decimal(self.median) <= Decimal(self.maximum):
            raise ValueError("scalar statistic median is outside its extrema")


@dataclass(frozen=True, slots=True)
class GroupStatistics:
    boundary: str
    timed_input_kind: str
    timed_input_sha256: str | None
    measurement_parameters: tuple[tuple[str, str], ...]
    workload_id: str
    workload_sha256: str
    workload_manifest_sha256: str
    binary_sha256: str
    valid_observations: int
    invalid_observations: int
    commands: int
    resting_order_denominator: int
    minimum_elapsed_ns: int
    maximum_elapsed_ns: int
    median_elapsed_ns: str
    mad_elapsed_ns: str
    iqr_elapsed_ns: int
    minimum_commands_per_second: str | None
    maximum_commands_per_second: str | None
    median_commands_per_second: str | None
    mad_commands_per_second: str | None
    iqr_commands_per_second: str | None
    minimum_events_per_second: str | None
    maximum_events_per_second: str | None
    median_events_per_second: str | None
    mad_events_per_second: str | None
    iqr_events_per_second: str | None
    latency_sample_count: int
    minimum_latency_ns: int | None
    maximum_latency_ns: int | None
    latency_quantiles_ns: tuple[tuple[str, int], ...]
    peak_rss_bytes: ScalarStatistics | None
    process_rss_delta_bytes: ScalarStatistics | None
    process_rss_delta_bytes_per_command: ScalarStatistics | None
    process_rss_delta_bytes_per_resting_order: ScalarStatistics | None
    allocation_count: ScalarStatistics | None
    deallocation_count: ScalarStatistics | None
    allocated_bytes: ScalarStatistics | None
    live_bytes: ScalarStatistics | None
    peak_live_bytes: ScalarStatistics | None
    allocations_per_command: ScalarStatistics | None

    def __post_init__(self) -> None:
        if self.boundary not in BOUNDARIES:
            raise ValueError("boundary is outside the V1 vocabulary")
        _validate_timed_input(self.boundary, self.timed_input_kind, self.timed_input_sha256)
        _require_parameter_pairs("measurement_parameters", self.measurement_parameters)
        _require_identifier("workload_id", self.workload_id)
        _require_digest("workload_sha256", self.workload_sha256)
        _require_digest("workload_manifest_sha256", self.workload_manifest_sha256)
        _require_digest("binary_sha256", self.binary_sha256)
        for name, value in (
            ("valid_observations", self.valid_observations),
            ("invalid_observations", self.invalid_observations),
            ("commands", self.commands),
            ("resting_order_denominator", self.resting_order_denominator),
            ("minimum_elapsed_ns", self.minimum_elapsed_ns),
            ("maximum_elapsed_ns", self.maximum_elapsed_ns),
            ("iqr_elapsed_ns", self.iqr_elapsed_ns),
        ):
            _require_uint(name, value, U64_MAX)
        _require_rate("median_elapsed_ns", self.median_elapsed_ns)
        _require_rate("mad_elapsed_ns", self.mad_elapsed_ns)
        rate_fields = (
            self.minimum_commands_per_second,
            self.maximum_commands_per_second,
            self.median_commands_per_second,
            self.mad_commands_per_second,
            self.iqr_commands_per_second,
            self.minimum_events_per_second,
            self.maximum_events_per_second,
            self.median_events_per_second,
            self.mad_events_per_second,
            self.iqr_events_per_second,
        )
        for rate in rate_fields:
            if rate is not None:
                _require_rate("rate statistic", rate)
        if any(rate is None for rate in rate_fields) != all(rate is None for rate in rate_fields):
            raise ValueError("throughput rate statistics must be all present or all null")
        _require_uint("latency_sample_count", self.latency_sample_count, U64_MAX)
        if (self.minimum_latency_ns is None) != (self.maximum_latency_ns is None):
            raise ValueError("latency minimum and maximum must have matching presence")
        if self.minimum_latency_ns is not None:
            _require_uint("minimum_latency_ns", self.minimum_latency_ns, U64_MAX)
            assert self.maximum_latency_ns is not None
            _require_uint("maximum_latency_ns", self.maximum_latency_ns, U64_MAX)
            if self.latency_sample_count == 0:
                raise ValueError("latency extrema require samples")
        names = tuple(name for name, _ in self.latency_quantiles_ns)
        if names not in {(), ("p50", "p90", "p95", "p99", "p99.9")}:
            raise ValueError("latency quantiles must use the complete canonical vocabulary")
        for _, value in self.latency_quantiles_ns:
            _require_uint("latency quantile", value, U64_MAX)
        if self.latency_sample_count == 0:
            if (
                self.minimum_latency_ns is not None
                or self.maximum_latency_ns is not None
                or self.latency_quantiles_ns
            ):
                raise ValueError("zero-sample latency statistics must be empty")
        elif (
            self.minimum_latency_ns is None
            or self.maximum_latency_ns is None
            or not self.latency_quantiles_ns
        ):
            raise ValueError("nonzero latency samples require extrema and quantiles")
        if self.latency_quantiles_ns:
            assert self.minimum_latency_ns is not None
            assert self.maximum_latency_ns is not None
            ordered_latency = (
                self.minimum_latency_ns,
                *(value for _, value in self.latency_quantiles_ns),
                self.maximum_latency_ns,
            )
            if tuple(sorted(ordered_latency)) != ordered_latency:
                raise ValueError("latency statistics are not monotonically ordered")
        rss_fields = (
            self.peak_rss_bytes,
            self.process_rss_delta_bytes,
        )
        per_command_rss = self.process_rss_delta_bytes_per_command
        resting_rss = self.process_rss_delta_bytes_per_resting_order
        allocation_fields = (
            self.allocation_count,
            self.deallocation_count,
            self.allocated_bytes,
            self.live_bytes,
            self.peak_live_bytes,
            self.allocations_per_command,
        )
        for name, statistics in (
            ("peak_rss_bytes", self.peak_rss_bytes),
            ("allocation_count", self.allocation_count),
            ("deallocation_count", self.deallocation_count),
            ("allocated_bytes", self.allocated_bytes),
            ("live_bytes", self.live_bytes),
            ("peak_live_bytes", self.peak_live_bytes),
            ("allocations_per_command", self.allocations_per_command),
        ):
            if statistics is not None and Decimal(statistics.minimum) < 0:
                raise ValueError(f"{name} statistics cannot be negative")
        if self.valid_observations == 0:
            if any(
                item is not None
                for item in (*rss_fields, per_command_rss, resting_rss, *allocation_fields)
            ):
                raise ValueError("invalid-only groups cannot carry memory statistics")
        else:
            if self.commands == 0 and self.boundary != "core_construction":
                raise ValueError("valid groups must process at least one command")
            if any(item is None for item in rss_fields):
                raise ValueError("valid groups require complete process RSS statistics")
            if (self.commands == 0) != (per_command_rss is None):
                raise ValueError("per-command RSS presence must match a nonzero command count")
            if (self.resting_order_denominator == 0) != (resting_rss is None):
                raise ValueError(
                    "per-resting-order RSS requires a nonzero manifest-bound denominator"
                )
            if self.boundary in {"core_allocation", "core_setup_allocation"}:
                if any(item is None for item in allocation_fields):
                    raise ValueError("allocation groups require complete allocation statistics")
            elif any(item is not None for item in allocation_fields):
                raise ValueError("non-allocation groups cannot carry allocation statistics")
        _validate_group_relations(self, rate_fields)


@dataclass(frozen=True, slots=True)
class Comparison:
    comparison_id: str
    boundary: str
    timed_input_kind: str
    timed_input_sha256: str | None
    measurement_parameters: tuple[tuple[str, str], ...]
    workload_id: str
    workload_sha256: str
    workload_manifest_sha256: str
    host_context_sha256: str
    baseline_binary_sha256: str
    candidate_binary_sha256: str
    target_metric: str
    direction: str
    classification: str
    target_median_change_percent: str
    baseline_relative_mad_percent: str
    candidate_relative_mad_percent: str
    peak_rss_change_percent: str | None
    abba_blocks: int

    def __post_init__(self) -> None:
        if self.boundary not in BOUNDARIES:
            raise ValueError("boundary is outside the V1 vocabulary")
        _validate_timed_input(self.boundary, self.timed_input_kind, self.timed_input_sha256)
        _require_parameter_pairs("measurement_parameters", self.measurement_parameters)
        _require_identifier("workload_id", self.workload_id)
        _require_digest("workload_sha256", self.workload_sha256)
        _require_digest("workload_manifest_sha256", self.workload_manifest_sha256)
        _require_digest("host_context_sha256", self.host_context_sha256)
        _require_digest("baseline_binary_sha256", self.baseline_binary_sha256)
        _require_digest("candidate_binary_sha256", self.candidate_binary_sha256)
        if self.baseline_binary_sha256 == self.candidate_binary_sha256:
            raise ValueError("a comparison requires distinct baseline and candidate binaries")
        expected_metric = TARGET_METRIC_BY_BOUNDARY[self.boundary]
        if (self.target_metric, self.direction) != expected_metric:
            raise ValueError("comparison metric/direction differs from its boundary")
        expected_id = comparison_identity_digest(self)
        if self.comparison_id == "":
            object.__setattr__(self, "comparison_id", expected_id)
        else:
            _require_digest("comparison_id", self.comparison_id)
        if self.comparison_id != expected_id:
            raise ValueError("comparison_id differs from its canonical identity")
        if self.classification not in {"official", "exploratory"}:
            raise ValueError("comparison classification is outside the V1 vocabulary")
        _require_signed_rate("target_median_change_percent", self.target_median_change_percent)
        _require_rate("baseline_relative_mad_percent", self.baseline_relative_mad_percent)
        _require_rate("candidate_relative_mad_percent", self.candidate_relative_mad_percent)
        if self.peak_rss_change_percent is not None:
            _require_signed_rate("peak_rss_change_percent", self.peak_rss_change_percent)
        _require_uint("abba_blocks", self.abba_blocks, U64_MAX, nonzero=True)


@dataclass(frozen=True, slots=True)
class Experiment:
    experiment_id: str
    policy: str
    classification: str
    target_comparison_id: str | None
    control_comparison_ids: tuple[str, ...]
    threshold_result: str
    decision: str
    target_median_change_percent: str | None
    noise_gate_percent: str | None
    worst_control_change_percent: str | None
    worst_peak_rss_change_percent: str | None
    correctness_gate: str
    complexity_gate: str
    note_path: str
    note_sha256: str
    rationale: str

    def __post_init__(self) -> None:
        if re.fullmatch(r"EXP-[0-9]{3}", self.experiment_id) is None:
            raise ValueError("experiment_id must use EXP-###")
        if self.policy not in {"general", "capacity_reservation"}:
            raise ValueError("experiment policy is outside the V1 vocabulary")
        if self.classification not in {"official", "exploratory", "not_run"}:
            raise ValueError("experiment classification is outside the V1 vocabulary")
        if self.target_comparison_id is not None:
            _require_digest("target_comparison_id", self.target_comparison_id)
        if tuple(sorted(self.control_comparison_ids)) != self.control_comparison_ids or len(
            set(self.control_comparison_ids)
        ) != len(self.control_comparison_ids):
            raise ValueError("control comparison IDs must be unique and sorted")
        for comparison_id in self.control_comparison_ids:
            _require_digest("control comparison ID", comparison_id)
        if self.threshold_result not in {"passed", "failed", "not_run"}:
            raise ValueError("threshold_result is outside the V1 vocabulary")
        if self.decision not in {"accepted", "rejected", "neutral", "deferred"}:
            raise ValueError("experiment decision is outside the V1 vocabulary")
        for gate_name, gate in (
            ("correctness_gate", self.correctness_gate),
            ("complexity_gate", self.complexity_gate),
        ):
            if gate not in {"passed", "failed", "not_run"}:
                raise ValueError(f"{gate_name} is outside the V1 vocabulary")
        for name, value in (
            ("target_median_change_percent", self.target_median_change_percent),
            ("noise_gate_percent", self.noise_gate_percent),
            ("worst_control_change_percent", self.worst_control_change_percent),
            ("worst_peak_rss_change_percent", self.worst_peak_rss_change_percent),
        ):
            if value is not None:
                _require_signed_rate(name, value)
        _require_safe_relative_path("note_path", self.note_path)
        _require_digest("note_sha256", self.note_sha256)
        _require_public_text("rationale", self.rationale)


@dataclass(frozen=True, slots=True)
class BenchmarkReport:
    classification: str
    suite_label: str
    host_context_sha256: str
    workload_manifest_sha256s: tuple[str, ...]
    environment_sha256s: tuple[str, ...]
    source_observations: tuple[str, ...]
    groups: tuple[GroupStatistics, ...]
    comparisons: tuple[Comparison, ...]
    experiments: tuple[Experiment, ...]
    limitations: tuple[str, ...]

    def __post_init__(self) -> None:
        if self.classification not in {"official", "exploratory"}:
            raise ValueError("report classification is outside the V1 vocabulary")
        _require_suite_label(self.suite_label)
        _require_digest("host_context_sha256", self.host_context_sha256)
        if (
            not self.workload_manifest_sha256s
            or len(self.workload_manifest_sha256s) > MAX_REPORT_WORKLOADS
            or tuple(sorted(self.workload_manifest_sha256s)) != self.workload_manifest_sha256s
            or len(set(self.workload_manifest_sha256s)) != len(self.workload_manifest_sha256s)
        ):
            raise ValueError("workload manifest digests must be nonempty, unique, and sorted")
        for digest in self.workload_manifest_sha256s:
            _require_digest("workload manifest digest", digest)
        if (
            not self.environment_sha256s
            or len(self.environment_sha256s) > MAX_REPORT_ENVIRONMENTS
            or tuple(sorted(self.environment_sha256s)) != self.environment_sha256s
            or len(set(self.environment_sha256s)) != len(self.environment_sha256s)
        ):
            raise ValueError("environment digests must be nonempty, unique, and sorted")
        for digest in self.environment_sha256s:
            _require_digest("environment digest", digest)
        if not 1 <= len(self.source_observations) <= MAX_REPORT_OBSERVATIONS:
            raise ValueError("report observation references exceed their entry limit")
        if tuple(sorted(self.source_observations)) != self.source_observations or len(
            set(self.source_observations)
        ) != len(self.source_observations):
            raise ValueError("source observations must be unique and sorted")
        for digest in self.source_observations:
            _require_digest("source observation", digest)
        if len(self.groups) > MAX_REPORT_GROUPS:
            raise ValueError("report groups exceed their entry limit")
        group_keys = tuple(
            (
                item.boundary,
                item.timed_input_kind,
                "" if item.timed_input_sha256 is None else item.timed_input_sha256,
                item.measurement_parameters,
                item.workload_id,
                item.workload_sha256,
                item.workload_manifest_sha256,
                item.binary_sha256,
            )
            for item in self.groups
        )
        if group_keys != tuple(sorted(group_keys)) or len(group_keys) != len(set(group_keys)):
            raise ValueError("report groups must be uniquely sorted by canonical identity")
        if len(self.comparisons) > MAX_REPORT_COMPARISONS:
            raise ValueError("report comparisons exceed their entry limit")
        comparison_ids = tuple(item.comparison_id for item in self.comparisons)
        if comparison_ids != tuple(sorted(comparison_ids)) or len(comparison_ids) != len(
            set(comparison_ids)
        ):
            raise ValueError("report comparisons must be uniquely sorted by comparison_id")
        if any(item.host_context_sha256 != self.host_context_sha256 for item in self.comparisons):
            raise ValueError("report comparisons differ from the report host context")
        if self.classification == "official" and any(
            item.classification != "official" for item in self.comparisons
        ):
            raise ValueError("an official report cannot contain exploratory comparisons")
        if len(self.experiments) > MAX_REPORT_EXPERIMENTS:
            raise ValueError("report experiments exceed their entry limit")
        experiment_ids = tuple(item.experiment_id for item in self.experiments)
        if experiment_ids != tuple(sorted(experiment_ids)) or len(experiment_ids) != len(
            set(experiment_ids)
        ):
            raise ValueError("report experiments must have unique sorted IDs")
        _validate_report_experiments(self.comparisons, self.experiments)
        if tuple(sorted(self.limitations)) != self.limitations or len(set(self.limitations)) != len(
            self.limitations
        ):
            raise ValueError("report limitations must be unique and sorted")
        if len(self.limitations) > MAX_LIMITATIONS:
            raise ValueError("report limitations exceed their entry limit")
        for limitation in self.limitations:
            _require_public_text("limitation", limitation)


def _validate_group_relations(
    value: GroupStatistics,
    rate_fields: tuple[str | None, ...],
) -> None:
    elapsed = (
        Decimal(value.minimum_elapsed_ns),
        Decimal(value.median_elapsed_ns),
        Decimal(value.maximum_elapsed_ns),
    )
    if tuple(sorted(elapsed)) != elapsed:
        raise ValueError("elapsed statistics are not monotonically ordered")

    def validate_rate_group(name: str, statistics: tuple[str | None, ...]) -> None:
        if all(item is None for item in statistics):
            return
        if any(item is None for item in statistics):
            raise ValueError(f"{name} rate statistics have partial presence")
        minimum, maximum, center, mad, iqr = (
            Decimal(item) for item in statistics if item is not None
        )
        if not minimum <= center <= maximum or mad < 0 or iqr < 0:
            raise ValueError(f"{name} rate statistics are internally inconsistent")

    command_rates = rate_fields[:5]
    event_rates = rate_fields[5:]
    validate_rate_group("command", command_rates)
    validate_rate_group("event", event_rates)
    rates_present = command_rates[0] is not None
    latency_present = value.latency_sample_count != 0
    if value.valid_observations == 0:
        if rates_present or latency_present:
            raise ValueError("invalid-only groups cannot carry timing distributions")
        return
    if value.boundary == "core_latency":
        if rates_present:
            raise ValueError("latency groups cannot carry throughput rates")
        if not latency_present:
            raise ValueError("valid latency groups require latency statistics")
    elif value.boundary in {
        "core_allocation",
        "core_setup_allocation",
        "core_construction",
    }:
        if rates_present or latency_present:
            raise ValueError("setup/allocation groups cannot carry throughput distributions")
    else:
        if not rates_present:
            raise ValueError("timed throughput groups require complete rate statistics")
        if latency_present:
            raise ValueError("non-latency groups cannot carry latency statistics")


def comparison_identity_digest(value: Comparison) -> str:
    """Return the stable identity of one A/B measurement point."""

    return document_sha256(
        {
            "boundary": value.boundary,
            "measurement_parameters": dict(value.measurement_parameters),
            "workload_id": value.workload_id,
            "workload_sha256": value.workload_sha256,
            "workload_manifest_sha256": value.workload_manifest_sha256,
            "host_context_sha256": value.host_context_sha256,
            "baseline_binary_sha256": value.baseline_binary_sha256,
            "candidate_binary_sha256": value.candidate_binary_sha256,
        }
    )


def _validate_report_experiments(
    comparisons: tuple[Comparison, ...],
    experiments: tuple[Experiment, ...],
) -> None:
    by_id = {item.comparison_id: item for item in comparisons}
    note_paths: set[str] = set()
    for experiment in experiments:
        if experiment.note_path in note_paths:
            raise ValueError("experiment note paths must be unique")
        note_paths.add(experiment.note_path)
        numeric_values = (
            experiment.target_median_change_percent,
            experiment.noise_gate_percent,
            experiment.worst_control_change_percent,
            experiment.worst_peak_rss_change_percent,
        )
        if experiment.decision == "deferred":
            if (
                experiment.classification != "not_run"
                or experiment.target_comparison_id is not None
                or experiment.control_comparison_ids
                or experiment.threshold_result != "not_run"
                or any(value is not None for value in numeric_values)
                or experiment.correctness_gate != "not_run"
                or experiment.complexity_gate != "not_run"
            ):
                raise ValueError("deferred experiments cannot reference measured evidence")
            continue
        if (
            experiment.target_comparison_id is None
            or not experiment.control_comparison_ids
            or any(value is None for value in numeric_values)
            or experiment.correctness_gate == "not_run"
            or experiment.complexity_gate == "not_run"
        ):
            raise ValueError("measured experiments require complete comparison evidence")
        target = by_id.get(experiment.target_comparison_id)
        controls = tuple(by_id.get(item) for item in experiment.control_comparison_ids)
        if target is None or any(item is None for item in controls):
            raise ValueError("experiment references a missing comparison")
        typed_controls = tuple(item for item in controls if item is not None)
        if target.comparison_id in experiment.control_comparison_ids:
            raise ValueError("an experiment target cannot also be a control")
        if (
            target.boundary != "core_throughput"
            or target.target_metric != "commands_per_second"
            or target.direction != "higher_is_better"
        ):
            raise ValueError("experiment targets must be core-throughput comparisons")
        if any(
            item.boundary != "core_throughput"
            or item.target_metric != "commands_per_second"
            or item.direction != "higher_is_better"
            for item in typed_controls
        ):
            raise ValueError("experiment controls must be core-throughput comparisons")
        pair = (target.baseline_binary_sha256, target.candidate_binary_sha256)
        if any(
            (item.baseline_binary_sha256, item.candidate_binary_sha256) != pair
            or item.host_context_sha256 != target.host_context_sha256
            for item in typed_controls
        ):
            raise ValueError("experiment target and controls must share binaries and host context")
        with localcontext() as context:
            context.prec = 80
            target_change = Decimal(target.target_median_change_percent)
            noise = Decimal(2) * max(
                Decimal(target.baseline_relative_mad_percent),
                Decimal(target.candidate_relative_mad_percent),
            )
            worst_control = min(
                Decimal(item.target_median_change_percent) for item in typed_controls
            )
            rss_values = (target, *typed_controls)
            if any(item.peak_rss_change_percent is None for item in rss_values):
                raise ValueError("experiment comparisons require peak RSS changes")
            worst_rss = max(Decimal(item.peak_rss_change_percent or "0") for item in rss_values)
        assert experiment.target_median_change_percent is not None
        assert experiment.noise_gate_percent is not None
        assert experiment.worst_control_change_percent is not None
        assert experiment.worst_peak_rss_change_percent is not None
        if (
            Decimal(experiment.target_median_change_percent) != target_change
            or Decimal(experiment.noise_gate_percent) != noise
            or Decimal(experiment.worst_control_change_percent) != worst_control
            or Decimal(experiment.worst_peak_rss_change_percent) != worst_rss
        ):
            raise ValueError("experiment metrics differ from their referenced comparisons")
        rss_passed = (
            worst_rss <= Decimal(10)
            if experiment.policy == "general"
            else worst_rss <= Decimal(10)
            or (worst_rss <= Decimal(20) and target_change >= Decimal(10))
        )
        threshold_passed = (
            target_change >= Decimal(5)
            and target_change > noise
            and worst_control >= Decimal(-5)
            and rss_passed
        )
        if experiment.threshold_result != ("passed" if threshold_passed else "failed"):
            raise ValueError("experiment threshold result is not derived from its evidence")
        expected_classification = (
            "official"
            if all(item.classification == "official" for item in rss_values)
            else "exploratory"
        )
        if experiment.classification != expected_classification:
            raise ValueError("experiment classification differs from its comparisons")
        gates_passed = (
            experiment.correctness_gate == "passed" and experiment.complexity_gate == "passed"
        )
        if threshold_passed and expected_classification == "official" and gates_passed:
            expected_decision = "accepted"
        elif (
            target_change < 0
            or worst_control < Decimal(-5)
            or not rss_passed
            or experiment.correctness_gate == "failed"
            or experiment.complexity_gate == "failed"
        ):
            expected_decision = "rejected"
        else:
            expected_decision = "neutral"
        if experiment.decision != expected_decision:
            raise ValueError("experiment decision is not derived from its gates and evidence")


def validate_observation_against_workload(
    observation: Observation,
    workload: WorkloadManifest,
    *,
    manifest_sha256: str,
) -> None:
    """Bind valid result evidence to one frozen workload and sampling policy."""

    if (
        observation.workload_id != workload.workload_id
        or observation.workload_sha256 != workload.stream_sha256
        or observation.workload_manifest_sha256 != manifest_sha256
        or observation.preload_commands != workload.preload_commands
        or observation.warmup_commands != workload.warmup_commands
    ):
        raise ValueError("observation identity or regions differ from expected workload evidence")
    _require_digest("manifest_sha256", manifest_sha256)
    if observation.boundary in {"replay_fast", "replay_verify"}:
        if (
            observation.timed_input_kind != workload.timed_input_kind
            or observation.timed_input_sha256 != workload.timed_input_sha256
        ):
            raise ValueError("replay observation differs from W10 timed input identity")
    _validate_measurement_parameters(observation, workload)
    if not observation.valid:
        return
    empty_event_digest = hashlib.sha256(b"").hexdigest()
    if observation.boundary == "core_construction":
        expected_result = (
            0,
            0,
            0,
            0,
            0,
            empty_event_digest,
            workload.expected_empty_state_digest,
        )
    elif observation.boundary in {"core_preload", "core_setup_allocation"}:
        expected_result = (
            workload.preload_commands,
            workload.expected_preload_events,
            workload.expected_preload_committed,
            workload.expected_preload_rejected,
            workload.expected_preload_engine_errors,
            workload.expected_preload_event_digest,
            workload.expected_preload_state_digest,
        )
    else:
        expected_result = (
            workload.measured_commands,
            workload.expected_events,
            workload.expected_committed,
            workload.expected_rejected,
            workload.expected_engine_errors,
            workload.expected_event_digest,
            workload.expected_final_digest,
        )
    actual_result = (
        observation.commands,
        observation.events,
        observation.committed,
        observation.rejected,
        observation.engine_errors,
        observation.event_digest,
        observation.final_digest,
    )
    if actual_result != expected_result:
        raise ValueError("valid observation differs from expected workload evidence")
    if observation.boundary == "core_latency":
        parameters = dict(workload.parameters)
        if (
            parameters.get("latency_first_sample_index") != "31"
            or parameters.get("latency_sample_stride") != "32"
        ):
            raise ValueError("workload does not carry the frozen latency sampling policy")
        expected_samples = min(workload.measured_commands // 32, 200_000)
        assert observation.latency_ns is not None
        if len(observation.latency_ns) != expected_samples:
            raise ValueError("latency sample count differs from the frozen workload policy")


def workload_measurement_parameters(
    workload: WorkloadManifest,
) -> tuple[tuple[str, str], ...]:
    parameters = dict(workload.parameters)
    required = (
        "instrument_count",
        "sweep_depth",
    )
    if any(name not in parameters for name in required):
        raise ValueError("workload omits required measurement shape parameters")
    return (
        ("instrument_count", parameters["instrument_count"]),
        (
            "measured_start_active_order_count",
            str(workload.measured_start_active_order_count),
        ),
        ("sweep_depth", parameters["sweep_depth"]),
    )


def measurement_parameters_for_boundary(
    workload: WorkloadManifest,
    boundary: str,
    *,
    batch_size: int | None = None,
) -> tuple[tuple[str, str], ...]:
    """Return the exact grouping parameters for one frozen boundary."""

    if boundary not in BOUNDARIES:
        raise ValueError("boundary is outside the V1 vocabulary")
    if workload.workload_id == "W10" and boundary not in {
        "replay_fast",
        "replay_verify",
    }:
        raise ValueError("W10 permits only replay boundaries")
    common = dict(workload_measurement_parameters(workload))
    if boundary.startswith("python_"):
        if workload.workload_id != "W04":
            raise ValueError("Python batch boundaries require W04")
        if batch_size not in {1, 64, 1024, 65_536}:
            raise ValueError("Python batch size is outside the frozen catalog")
        common["batch_size"] = str(batch_size)
        common["output_mode"] = boundary.removeprefix("python_")
    elif boundary in {"replay_fast", "replay_verify"}:
        if workload.workload_id != "W10":
            raise ValueError("replay boundaries require W10")
        common["cache_policy"] = workload.cache_policy
        common["record_count"] = str(workload.timed_input_records)
        common["replay_mode"] = boundary.removeprefix("replay_")
        assert workload.timed_input_sha256 is not None
        common["timed_input_sha256"] = workload.timed_input_sha256
    elif batch_size is not None:
        raise ValueError("batch_size is only valid for Python boundaries")
    return tuple(sorted(common.items()))


def _validate_measurement_parameters(
    observation: Observation,
    workload: WorkloadManifest,
) -> None:
    common = dict(workload_measurement_parameters(workload))
    actual = dict(observation.measurement_parameters)
    if workload.workload_id == "W10" and observation.boundary not in {
        "replay_fast",
        "replay_verify",
    }:
        raise ValueError("W10 permits only replay boundaries")
    if observation.boundary.startswith("python_"):
        if workload.workload_id != "W04":
            raise ValueError("Python batch boundaries require W04")
        output_mode = observation.boundary.removeprefix("python_")
        expected_keys = {*common, "batch_size", "output_mode"}
        if set(actual) != expected_keys:
            raise ValueError("Python measurement parameters have unexpected fields")
        if actual["output_mode"] != output_mode or actual["batch_size"] not in {
            "1",
            "64",
            "1024",
            "65536",
        }:
            raise ValueError("Python batch measurement point is outside the frozen catalog")
    elif observation.boundary in {"replay_fast", "replay_verify"}:
        expected_keys = {
            *common,
            "cache_policy",
            "record_count",
            "replay_mode",
            "timed_input_sha256",
        }
        if set(actual) != expected_keys:
            raise ValueError("replay measurement parameters have unexpected fields")
        if (
            observation.workload_id != "W10"
            or actual["replay_mode"] != observation.boundary.removeprefix("replay_")
            or actual["record_count"] != str(workload.timed_input_records)
            or actual["cache_policy"] != "warm_page_cache"
            or actual["timed_input_sha256"] != workload.timed_input_sha256
        ):
            raise ValueError("replay measurement point differs from W10 timed input metadata")
    elif actual != common:
        raise ValueError("core measurement parameters differ from the workload shape")
    if any(actual.get(name) != value for name, value in common.items()):
        raise ValueError("measurement parameters differ from the common workload shape")


def canonical_json_bytes(value: Mapping[str, object]) -> bytes:
    """Encode one canonical, LF-terminated ASCII JSON document."""

    return (
        json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode("ascii")


def document_sha256(value: Mapping[str, object]) -> str:
    return hashlib.sha256(canonical_json_bytes(value)).hexdigest()


def environment_context_digest(value: EnvironmentManifest) -> str:
    """Hash only host/build facts that must match across A/B binaries."""

    context = {
        "schema": "ATLAS_BENCH_HOST_CONTEXT_V1",
        "os": value.os,
        "kernel": value.kernel,
        "host_class": value.host_class,
        "cpu_model": value.cpu_model,
        "architecture": value.architecture,
        "physical_cores": str(value.physical_cores),
        "logical_cpus": str(value.logical_cpus),
        "microcode": value.microcode,
        "memory_bytes": str(value.memory_bytes),
        "compiler": value.compiler,
        "compiler_flags": list(value.compiler_flags),
        "build_receipt_sha256": value.build_receipt_sha256,
        "build_target_profiles": dict(value.build_target_profiles),
        "build_type": value.build_type,
        "optimization": value.optimization,
        "ndebug": value.ndebug,
        "frame_pointers": value.frame_pointers,
        "invariants": value.invariants,
        "sanitizers": value.sanitizers,
        "lto": value.lto,
        "benchmark_build": value.benchmark_build,
        "warnings_as_errors": value.warnings_as_errors,
        "debug_symbols": value.debug_symbols,
        "cxx20": value.cxx20,
        "release_flags_locked": value.release_flags_locked,
        "affinity": [str(cpu) for cpu in value.affinity],
        "pinned_cpu": None if value.pinned_cpu is None else str(value.pinned_cpu),
        "smt_sibling_idle": value.smt_sibling_idle,
        "numa_nodes": str(value.numa_nodes),
        "numa_cpu_policy": value.numa_cpu_policy,
        "numa_memory_policy": value.numa_memory_policy,
        "filesystem": value.filesystem,
        "storage_class": value.storage_class,
        "governor": value.governor,
        "turbo": value.turbo,
        "smt": value.smt,
        "virtualization": value.virtualization,
        "perf_version": value.perf_version,
        "runtime_kind": value.runtime_kind,
        "python_implementation": value.python_implementation,
        "python_version": value.python_version,
        "python_cache_tag": value.python_cache_tag,
        "interpreter_sha256": value.interpreter_sha256,
    }
    return document_sha256(context)


def workload_to_dict(value: WorkloadManifest) -> dict[str, object]:
    return {
        "schema": WORKLOAD_SCHEMA,
        "workload_id": value.workload_id,
        "generator_version": str(value.generator_version),
        "seed": str(value.seed),
        "catalog": [
            {
                "instrument_id": str(entry.instrument_id),
                "max_order_quantity": str(entry.max_order_quantity),
                "tick_increment": str(entry.tick_increment),
                "max_active_orders": str(entry.max_active_orders),
            }
            for entry in value.catalog
        ],
        "max_total_active_orders": str(value.max_total_active_orders),
        "operation_distribution": {
            name: str(count) for name, count in value.operation_distribution
        },
        "parameters": {name: parameter for name, parameter in value.parameters},
        "preload_commands": str(value.preload_commands),
        "warmup_commands": str(value.warmup_commands),
        "measured_commands": str(value.measured_commands),
        "after_preload_active_order_count": str(value.after_preload_active_order_count),
        "measured_start_active_order_count": str(value.measured_start_active_order_count),
        "final_active_order_count": str(value.final_active_order_count),
        "stream_file": value.stream_file,
        "stream_sha256": value.stream_sha256,
        "expected_events": str(value.expected_events),
        "expected_committed": str(value.expected_committed),
        "expected_rejected": str(value.expected_rejected),
        "expected_engine_errors": str(value.expected_engine_errors),
        "expected_event_digest": value.expected_event_digest,
        "expected_final_digest": value.expected_final_digest,
        "expected_empty_state_digest": value.expected_empty_state_digest,
        "expected_preload_events": str(value.expected_preload_events),
        "expected_preload_committed": str(value.expected_preload_committed),
        "expected_preload_rejected": str(value.expected_preload_rejected),
        "expected_preload_engine_errors": str(value.expected_preload_engine_errors),
        "expected_preload_event_digest": value.expected_preload_event_digest,
        "expected_preload_state_digest": value.expected_preload_state_digest,
        "timed_input_file": value.timed_input_file,
        "timed_input_kind": value.timed_input_kind,
        "timed_input_sha256": value.timed_input_sha256,
        "timed_input_records": str(value.timed_input_records),
        "cache_policy": value.cache_policy,
    }


def workload_from_dict(value: Mapping[str, object]) -> WorkloadManifest:
    _require_keys(
        value,
        {
            "schema",
            "workload_id",
            "generator_version",
            "seed",
            "catalog",
            "max_total_active_orders",
            "operation_distribution",
            "parameters",
            "preload_commands",
            "warmup_commands",
            "measured_commands",
            "after_preload_active_order_count",
            "measured_start_active_order_count",
            "final_active_order_count",
            "stream_file",
            "stream_sha256",
            "expected_events",
            "expected_committed",
            "expected_rejected",
            "expected_engine_errors",
            "expected_event_digest",
            "expected_final_digest",
            "expected_empty_state_digest",
            "expected_preload_events",
            "expected_preload_committed",
            "expected_preload_rejected",
            "expected_preload_engine_errors",
            "expected_preload_event_digest",
            "expected_preload_state_digest",
            "timed_input_file",
            "timed_input_kind",
            "timed_input_sha256",
            "timed_input_records",
            "cache_policy",
        },
    )
    if value["schema"] != WORKLOAD_SCHEMA:
        raise ValueError("unsupported benchmark workload schema")
    raw_catalog = _array(value["catalog"], "catalog")
    catalog: list[CatalogEntry] = []
    for index, raw in enumerate(raw_catalog):
        entry = _mapping(raw, f"catalog[{index}]")
        _require_keys(
            entry,
            {
                "instrument_id",
                "max_order_quantity",
                "tick_increment",
                "max_active_orders",
            },
        )
        catalog.append(
            CatalogEntry(
                _decimal(entry["instrument_id"], "instrument_id", (1 << 32) - 1),
                _decimal(entry["max_order_quantity"], "max_order_quantity", U64_MAX),
                _signed_decimal(entry["tick_increment"], "tick_increment"),
                _decimal(entry["max_active_orders"], "max_active_orders", U64_MAX),
            )
        )
    raw_distribution = _mapping(value["operation_distribution"], "operation_distribution")
    distribution = tuple(
        sorted(
            (
                name,
                _decimal(count, f"operation_distribution.{name}", U64_MAX),
            )
            for name, count in raw_distribution.items()
        )
    )
    raw_parameters = _mapping(value["parameters"], "parameters")
    raw_timed_file = value["timed_input_file"]
    raw_timed_digest = value["timed_input_sha256"]
    parameters = tuple(
        sorted(
            (name, _string(parameter, f"parameters.{name}"))
            for name, parameter in raw_parameters.items()
        )
    )
    return WorkloadManifest(
        workload_id=_string(value["workload_id"], "workload_id"),
        generator_version=_decimal(value["generator_version"], "generator_version", U64_MAX),
        seed=_decimal(value["seed"], "seed", U64_MAX),
        catalog=tuple(catalog),
        max_total_active_orders=_decimal(
            value["max_total_active_orders"], "max_total_active_orders", U64_MAX
        ),
        operation_distribution=distribution,
        parameters=parameters,
        preload_commands=_decimal(value["preload_commands"], "preload_commands", U64_MAX),
        warmup_commands=_decimal(value["warmup_commands"], "warmup_commands", U64_MAX),
        measured_commands=_decimal(value["measured_commands"], "measured_commands", U64_MAX),
        after_preload_active_order_count=_decimal(
            value["after_preload_active_order_count"],
            "after_preload_active_order_count",
            U64_MAX,
        ),
        measured_start_active_order_count=_decimal(
            value["measured_start_active_order_count"],
            "measured_start_active_order_count",
            U64_MAX,
        ),
        final_active_order_count=_decimal(
            value["final_active_order_count"], "final_active_order_count", U64_MAX
        ),
        stream_file=_string(value["stream_file"], "stream_file"),
        stream_sha256=_string(value["stream_sha256"], "stream_sha256"),
        expected_events=_decimal(value["expected_events"], "expected_events", U64_MAX),
        expected_committed=_decimal(value["expected_committed"], "expected_committed", U64_MAX),
        expected_rejected=_decimal(value["expected_rejected"], "expected_rejected", U64_MAX),
        expected_engine_errors=_decimal(
            value["expected_engine_errors"], "expected_engine_errors", U64_MAX
        ),
        expected_event_digest=_string(value["expected_event_digest"], "expected_event_digest"),
        expected_final_digest=_string(value["expected_final_digest"], "expected_final_digest"),
        expected_empty_state_digest=_string(
            value["expected_empty_state_digest"], "expected_empty_state_digest"
        ),
        expected_preload_events=_decimal(
            value["expected_preload_events"], "expected_preload_events", U64_MAX
        ),
        expected_preload_committed=_decimal(
            value["expected_preload_committed"], "expected_preload_committed", U64_MAX
        ),
        expected_preload_rejected=_decimal(
            value["expected_preload_rejected"], "expected_preload_rejected", U64_MAX
        ),
        expected_preload_engine_errors=_decimal(
            value["expected_preload_engine_errors"],
            "expected_preload_engine_errors",
            U64_MAX,
        ),
        expected_preload_event_digest=_string(
            value["expected_preload_event_digest"], "expected_preload_event_digest"
        ),
        expected_preload_state_digest=_string(
            value["expected_preload_state_digest"], "expected_preload_state_digest"
        ),
        timed_input_file=(
            None if raw_timed_file is None else _string(raw_timed_file, "timed_input_file")
        ),
        timed_input_kind=_string(value["timed_input_kind"], "timed_input_kind"),
        timed_input_sha256=(
            None if raw_timed_digest is None else _string(raw_timed_digest, "timed_input_sha256")
        ),
        timed_input_records=_decimal(value["timed_input_records"], "timed_input_records", U64_MAX),
        cache_policy=_string(value["cache_policy"], "cache_policy"),
    )


def environment_to_dict(value: EnvironmentManifest) -> dict[str, object]:
    return {
        "schema": ENVIRONMENT_SCHEMA,
        "commit": value.commit,
        "tag": value.tag,
        "dirty": value.dirty,
        "binary_sha256": value.binary_sha256,
        "os": value.os,
        "kernel": value.kernel,
        "host_class": value.host_class,
        "cpu_model": value.cpu_model,
        "architecture": value.architecture,
        "physical_cores": str(value.physical_cores),
        "logical_cpus": str(value.logical_cpus),
        "microcode": value.microcode,
        "memory_bytes": str(value.memory_bytes),
        "compiler": value.compiler,
        "compiler_flags": list(value.compiler_flags),
        "build_receipt_sha256": value.build_receipt_sha256,
        "build_target_profiles": dict(value.build_target_profiles),
        "build_type": value.build_type,
        "optimization": value.optimization,
        "ndebug": value.ndebug,
        "frame_pointers": value.frame_pointers,
        "invariants": value.invariants,
        "sanitizers": value.sanitizers,
        "lto": value.lto,
        "benchmark_build": value.benchmark_build,
        "warnings_as_errors": value.warnings_as_errors,
        "debug_symbols": value.debug_symbols,
        "cxx20": value.cxx20,
        "release_flags_locked": value.release_flags_locked,
        "affinity": [str(cpu) for cpu in value.affinity],
        "pinned_cpu": None if value.pinned_cpu is None else str(value.pinned_cpu),
        "smt_sibling_idle": value.smt_sibling_idle,
        "numa_nodes": str(value.numa_nodes),
        "numa_cpu_policy": value.numa_cpu_policy,
        "numa_memory_policy": value.numa_memory_policy,
        "filesystem": value.filesystem,
        "storage_class": value.storage_class,
        "governor": value.governor,
        "turbo": value.turbo,
        "smt": value.smt,
        "virtualization": value.virtualization,
        "perf_version": value.perf_version,
        "runtime_kind": value.runtime_kind,
        "python_implementation": value.python_implementation,
        "python_version": value.python_version,
        "python_cache_tag": value.python_cache_tag,
        "atlaslob_version": value.atlaslob_version,
        "interpreter_sha256": value.interpreter_sha256,
        "wheel_sha256": value.wheel_sha256,
        "package_sha256": value.package_sha256,
        "wrapper_sha256": value.wrapper_sha256,
        "harness_sha256": value.harness_sha256,
        "classification": value.classification,
        "host_context_sha256": value.host_context_sha256,
        "limitations": list(value.limitations),
    }


def environment_from_dict(value: Mapping[str, object]) -> EnvironmentManifest:
    expected = {
        "schema",
        "commit",
        "tag",
        "dirty",
        "binary_sha256",
        "os",
        "kernel",
        "host_class",
        "cpu_model",
        "architecture",
        "physical_cores",
        "logical_cpus",
        "microcode",
        "memory_bytes",
        "compiler",
        "compiler_flags",
        "build_receipt_sha256",
        "build_target_profiles",
        "build_type",
        "optimization",
        "ndebug",
        "frame_pointers",
        "invariants",
        "sanitizers",
        "lto",
        "benchmark_build",
        "warnings_as_errors",
        "debug_symbols",
        "cxx20",
        "release_flags_locked",
        "affinity",
        "pinned_cpu",
        "smt_sibling_idle",
        "numa_nodes",
        "numa_cpu_policy",
        "numa_memory_policy",
        "filesystem",
        "storage_class",
        "governor",
        "turbo",
        "smt",
        "virtualization",
        "perf_version",
        "runtime_kind",
        "python_implementation",
        "python_version",
        "python_cache_tag",
        "atlaslob_version",
        "interpreter_sha256",
        "wheel_sha256",
        "package_sha256",
        "wrapper_sha256",
        "harness_sha256",
        "classification",
        "host_context_sha256",
        "limitations",
    }
    _require_keys(value, expected)
    if value["schema"] != ENVIRONMENT_SCHEMA:
        raise ValueError("unsupported benchmark environment schema")
    raw_tag = value["tag"]
    raw_perf = value["perf_version"]
    raw_python_implementation = value["python_implementation"]
    raw_python_version = value["python_version"]
    raw_python_cache_tag = value["python_cache_tag"]
    raw_atlaslob_version = value["atlaslob_version"]
    raw_interpreter_digest = value["interpreter_sha256"]
    raw_wheel_digest = value["wheel_sha256"]
    raw_package_digest = value["package_sha256"]
    raw_wrapper_digest = value["wrapper_sha256"]
    raw_harness_digest = value["harness_sha256"]
    dirty = value["dirty"]
    if not isinstance(dirty, bool):
        raise ValueError("dirty must be a JSON boolean")
    booleans = {}
    for name in (
        "ndebug",
        "frame_pointers",
        "invariants",
        "sanitizers",
        "lto",
        "benchmark_build",
        "warnings_as_errors",
        "debug_symbols",
        "cxx20",
        "release_flags_locked",
    ):
        raw_boolean = value[name]
        if not isinstance(raw_boolean, bool):
            raise ValueError(f"{name} must be a JSON boolean")
        booleans[name] = raw_boolean
    raw_pinned = value["pinned_cpu"]
    raw_sibling = value["smt_sibling_idle"]
    if raw_sibling is not None and not isinstance(raw_sibling, bool):
        raise ValueError("smt_sibling_idle must be a JSON boolean or null")
    return EnvironmentManifest(
        commit=_string(value["commit"], "commit"),
        tag=None if raw_tag is None else _string(raw_tag, "tag"),
        dirty=dirty,
        binary_sha256=_string(value["binary_sha256"], "binary_sha256"),
        os=_string(value["os"], "os"),
        kernel=_string(value["kernel"], "kernel"),
        host_class=_string(value["host_class"], "host_class"),
        cpu_model=_string(value["cpu_model"], "cpu_model"),
        architecture=_string(value["architecture"], "architecture"),
        physical_cores=_decimal(value["physical_cores"], "physical_cores", (1 << 32) - 1),
        logical_cpus=_decimal(value["logical_cpus"], "logical_cpus", (1 << 32) - 1),
        microcode=_string(value["microcode"], "microcode"),
        memory_bytes=_decimal(value["memory_bytes"], "memory_bytes", U64_MAX),
        compiler=_string(value["compiler"], "compiler"),
        compiler_flags=tuple(
            _string(item, "compiler flag")
            for item in _array(value["compiler_flags"], "compiler_flags")
        ),
        build_receipt_sha256=_string(value["build_receipt_sha256"], "build_receipt_sha256"),
        build_target_profiles=tuple(
            (
                _string(name, "build target profile name"),
                _string(profile, f"build target profile {name}"),
            )
            for name, profile in _mapping(
                value["build_target_profiles"], "build_target_profiles"
            ).items()
        ),
        build_type=_string(value["build_type"], "build_type"),
        optimization=_string(value["optimization"], "optimization"),
        ndebug=booleans["ndebug"],
        frame_pointers=booleans["frame_pointers"],
        invariants=booleans["invariants"],
        sanitizers=booleans["sanitizers"],
        lto=booleans["lto"],
        benchmark_build=booleans["benchmark_build"],
        warnings_as_errors=booleans["warnings_as_errors"],
        debug_symbols=booleans["debug_symbols"],
        cxx20=booleans["cxx20"],
        release_flags_locked=booleans["release_flags_locked"],
        affinity=tuple(
            _decimal(item, "affinity CPU", (1 << 32) - 1)
            for item in _array(value["affinity"], "affinity")
        ),
        pinned_cpu=(
            None if raw_pinned is None else _decimal(raw_pinned, "pinned_cpu", (1 << 32) - 1)
        ),
        smt_sibling_idle=raw_sibling,
        numa_nodes=_decimal(value["numa_nodes"], "numa_nodes", (1 << 32) - 1),
        numa_cpu_policy=_string(value["numa_cpu_policy"], "numa_cpu_policy"),
        numa_memory_policy=_string(value["numa_memory_policy"], "numa_memory_policy"),
        filesystem=_string(value["filesystem"], "filesystem"),
        storage_class=_string(value["storage_class"], "storage_class"),
        governor=_string(value["governor"], "governor"),
        turbo=_string(value["turbo"], "turbo"),
        smt=_string(value["smt"], "smt"),
        virtualization=_string(value["virtualization"], "virtualization"),
        perf_version=None if raw_perf is None else _string(raw_perf, "perf_version"),
        runtime_kind=_string(value["runtime_kind"], "runtime_kind"),
        python_implementation=(
            None
            if raw_python_implementation is None
            else _string(raw_python_implementation, "python_implementation")
        ),
        python_version=(
            None if raw_python_version is None else _string(raw_python_version, "python_version")
        ),
        python_cache_tag=(
            None
            if raw_python_cache_tag is None
            else _string(raw_python_cache_tag, "python_cache_tag")
        ),
        atlaslob_version=(
            None
            if raw_atlaslob_version is None
            else _string(raw_atlaslob_version, "atlaslob_version")
        ),
        interpreter_sha256=(
            None
            if raw_interpreter_digest is None
            else _string(raw_interpreter_digest, "interpreter_sha256")
        ),
        wheel_sha256=(
            None if raw_wheel_digest is None else _string(raw_wheel_digest, "wheel_sha256")
        ),
        package_sha256=(
            None if raw_package_digest is None else _string(raw_package_digest, "package_sha256")
        ),
        wrapper_sha256=(
            None if raw_wrapper_digest is None else _string(raw_wrapper_digest, "wrapper_sha256")
        ),
        harness_sha256=(
            None if raw_harness_digest is None else _string(raw_harness_digest, "harness_sha256")
        ),
        classification=_string(value["classification"], "classification"),
        host_context_sha256=_string(value["host_context_sha256"], "host_context_sha256"),
        limitations=tuple(
            _string(item, "limitation") for item in _array(value["limitations"], "limitations")
        ),
    )


def allocation_to_dict(value: AllocationMetrics) -> dict[str, object]:
    return {
        "allocation_count": str(value.allocation_count),
        "deallocation_count": str(value.deallocation_count),
        "allocated_bytes": str(value.allocated_bytes),
        "live_bytes": str(value.live_bytes),
        "peak_live_bytes": str(value.peak_live_bytes),
    }


def observation_to_dict(value: Observation) -> dict[str, object]:
    return {
        "schema": OBSERVATION_SCHEMA,
        "boundary": value.boundary,
        "timed_input_kind": value.timed_input_kind,
        "timed_input_sha256": value.timed_input_sha256,
        "measurement_parameters": dict(value.measurement_parameters),
        "workload_id": value.workload_id,
        "workload_sha256": value.workload_sha256,
        "workload_manifest_sha256": value.workload_manifest_sha256,
        "binary_sha256": value.binary_sha256,
        "environment_sha256": value.environment_sha256,
        "host_context_sha256": value.host_context_sha256,
        "suite_label": value.suite_label,
        "run_label": value.run_label,
        "variant": value.variant,
        "block_index": str(value.block_index),
        "block_position": str(value.block_position),
        "preload_commands": str(value.preload_commands),
        "warmup_commands": str(value.warmup_commands),
        "commands": str(value.commands),
        "events": str(value.events),
        "committed": str(value.committed),
        "rejected": str(value.rejected),
        "engine_errors": str(value.engine_errors),
        "elapsed_ns": str(value.elapsed_ns),
        "rss_before_bytes": str(value.rss_before_bytes),
        "rss_after_bytes": str(value.rss_after_bytes),
        "peak_rss_bytes": str(value.peak_rss_bytes),
        "latency_ns": (
            None if value.latency_ns is None else [str(sample) for sample in value.latency_ns]
        ),
        "allocations": (
            None if value.allocations is None else allocation_to_dict(value.allocations)
        ),
        "event_digest": value.event_digest,
        "final_digest": value.final_digest,
        "valid": value.valid,
        "failure_reason": value.failure_reason,
    }


def observation_from_dict(value: Mapping[str, object]) -> Observation:
    _require_keys(
        value,
        {
            "schema",
            "boundary",
            "timed_input_kind",
            "timed_input_sha256",
            "measurement_parameters",
            "workload_id",
            "workload_sha256",
            "workload_manifest_sha256",
            "binary_sha256",
            "environment_sha256",
            "host_context_sha256",
            "suite_label",
            "run_label",
            "variant",
            "block_index",
            "block_position",
            "preload_commands",
            "warmup_commands",
            "commands",
            "events",
            "committed",
            "rejected",
            "engine_errors",
            "elapsed_ns",
            "rss_before_bytes",
            "rss_after_bytes",
            "peak_rss_bytes",
            "latency_ns",
            "allocations",
            "event_digest",
            "final_digest",
            "valid",
            "failure_reason",
        },
    )
    if value["schema"] != OBSERVATION_SCHEMA:
        raise ValueError("unsupported benchmark observation schema")
    raw_latency = value["latency_ns"]
    latency = (
        None
        if raw_latency is None
        else tuple(
            _decimal(item, "latency sample", U64_MAX) for item in _array(raw_latency, "latency_ns")
        )
    )
    raw_allocations = value["allocations"]
    allocations: AllocationMetrics | None = None
    if raw_allocations is not None:
        mapping = _mapping(raw_allocations, "allocations")
        _require_keys(
            mapping,
            {
                "allocation_count",
                "deallocation_count",
                "allocated_bytes",
                "live_bytes",
                "peak_live_bytes",
            },
        )
        allocations = AllocationMetrics(
            allocation_count=_decimal(mapping["allocation_count"], "allocation_count", U64_MAX),
            deallocation_count=_decimal(
                mapping["deallocation_count"], "deallocation_count", U64_MAX
            ),
            allocated_bytes=_decimal(mapping["allocated_bytes"], "allocated_bytes", U64_MAX),
            live_bytes=_decimal(mapping["live_bytes"], "live_bytes", U64_MAX),
            peak_live_bytes=_decimal(mapping["peak_live_bytes"], "peak_live_bytes", U64_MAX),
        )
    raw_valid = value["valid"]
    if not isinstance(raw_valid, bool):
        raise ValueError("valid must be a JSON boolean")
    raw_reason = value["failure_reason"]
    return Observation(
        boundary=_string(value["boundary"], "boundary"),
        timed_input_kind=_string(value["timed_input_kind"], "timed_input_kind"),
        timed_input_sha256=(
            None
            if value["timed_input_sha256"] is None
            else _string(value["timed_input_sha256"], "timed_input_sha256")
        ),
        measurement_parameters=tuple(
            (
                _string(name, "measurement parameter name"),
                _string(parameter, f"measurement parameter {name}"),
            )
            for name, parameter in _mapping(
                value["measurement_parameters"], "measurement_parameters"
            ).items()
        ),
        workload_id=_string(value["workload_id"], "workload_id"),
        workload_sha256=_string(value["workload_sha256"], "workload_sha256"),
        workload_manifest_sha256=_string(
            value["workload_manifest_sha256"], "workload_manifest_sha256"
        ),
        binary_sha256=_string(value["binary_sha256"], "binary_sha256"),
        environment_sha256=_string(value["environment_sha256"], "environment_sha256"),
        host_context_sha256=_string(value["host_context_sha256"], "host_context_sha256"),
        suite_label=_string(value["suite_label"], "suite_label"),
        run_label=_string(value["run_label"], "run_label"),
        variant=_string(value["variant"], "variant"),
        block_index=_decimal(value["block_index"], "block_index", U64_MAX),
        block_position=_decimal(value["block_position"], "block_position", U64_MAX),
        preload_commands=_decimal(value["preload_commands"], "preload_commands", U64_MAX),
        warmup_commands=_decimal(value["warmup_commands"], "warmup_commands", U64_MAX),
        commands=_decimal(value["commands"], "commands", U64_MAX),
        events=_decimal(value["events"], "events", U64_MAX),
        committed=_decimal(value["committed"], "committed", U64_MAX),
        rejected=_decimal(value["rejected"], "rejected", U64_MAX),
        engine_errors=_decimal(value["engine_errors"], "engine_errors", U64_MAX),
        elapsed_ns=_decimal(value["elapsed_ns"], "elapsed_ns", U64_MAX),
        rss_before_bytes=_decimal(value["rss_before_bytes"], "rss_before_bytes", U64_MAX),
        rss_after_bytes=_decimal(value["rss_after_bytes"], "rss_after_bytes", U64_MAX),
        peak_rss_bytes=_decimal(value["peak_rss_bytes"], "peak_rss_bytes", U64_MAX),
        latency_ns=latency,
        allocations=allocations,
        event_digest=_string(value["event_digest"], "event_digest"),
        final_digest=_string(value["final_digest"], "final_digest"),
        valid=raw_valid,
        failure_reason=None if raw_reason is None else _string(raw_reason, "failure_reason"),
    )


def scalar_statistics_to_dict(value: ScalarStatistics) -> dict[str, object]:
    return {
        "minimum": value.minimum,
        "maximum": value.maximum,
        "median": value.median,
        "mad": value.mad,
        "iqr": value.iqr,
    }


def _optional_scalar_statistics_to_dict(
    value: ScalarStatistics | None,
) -> dict[str, object] | None:
    return None if value is None else scalar_statistics_to_dict(value)


def statistics_to_dict(value: GroupStatistics) -> dict[str, object]:
    return {
        "boundary": value.boundary,
        "timed_input_kind": value.timed_input_kind,
        "timed_input_sha256": value.timed_input_sha256,
        "measurement_parameters": dict(value.measurement_parameters),
        "workload_id": value.workload_id,
        "workload_sha256": value.workload_sha256,
        "workload_manifest_sha256": value.workload_manifest_sha256,
        "binary_sha256": value.binary_sha256,
        "valid_observations": str(value.valid_observations),
        "invalid_observations": str(value.invalid_observations),
        "commands": str(value.commands),
        "resting_order_denominator": str(value.resting_order_denominator),
        "minimum_elapsed_ns": str(value.minimum_elapsed_ns),
        "maximum_elapsed_ns": str(value.maximum_elapsed_ns),
        "median_elapsed_ns": value.median_elapsed_ns,
        "mad_elapsed_ns": value.mad_elapsed_ns,
        "iqr_elapsed_ns": str(value.iqr_elapsed_ns),
        "minimum_commands_per_second": value.minimum_commands_per_second,
        "maximum_commands_per_second": value.maximum_commands_per_second,
        "median_commands_per_second": value.median_commands_per_second,
        "mad_commands_per_second": value.mad_commands_per_second,
        "iqr_commands_per_second": value.iqr_commands_per_second,
        "minimum_events_per_second": value.minimum_events_per_second,
        "maximum_events_per_second": value.maximum_events_per_second,
        "median_events_per_second": value.median_events_per_second,
        "mad_events_per_second": value.mad_events_per_second,
        "iqr_events_per_second": value.iqr_events_per_second,
        "latency_sample_count": str(value.latency_sample_count),
        "minimum_latency_ns": (
            None if value.minimum_latency_ns is None else str(value.minimum_latency_ns)
        ),
        "maximum_latency_ns": (
            None if value.maximum_latency_ns is None else str(value.maximum_latency_ns)
        ),
        "latency_quantiles_ns": {name: str(sample) for name, sample in value.latency_quantiles_ns},
        "peak_rss_bytes": _optional_scalar_statistics_to_dict(value.peak_rss_bytes),
        "process_rss_delta_bytes": _optional_scalar_statistics_to_dict(
            value.process_rss_delta_bytes
        ),
        "process_rss_delta_bytes_per_command": _optional_scalar_statistics_to_dict(
            value.process_rss_delta_bytes_per_command
        ),
        "process_rss_delta_bytes_per_resting_order": _optional_scalar_statistics_to_dict(
            value.process_rss_delta_bytes_per_resting_order
        ),
        "allocation_count": _optional_scalar_statistics_to_dict(value.allocation_count),
        "deallocation_count": _optional_scalar_statistics_to_dict(value.deallocation_count),
        "allocated_bytes": _optional_scalar_statistics_to_dict(value.allocated_bytes),
        "live_bytes": _optional_scalar_statistics_to_dict(value.live_bytes),
        "peak_live_bytes": _optional_scalar_statistics_to_dict(value.peak_live_bytes),
        "allocations_per_command": _optional_scalar_statistics_to_dict(
            value.allocations_per_command
        ),
    }


def comparison_to_dict(value: Comparison) -> dict[str, object]:
    return {
        "comparison_id": value.comparison_id,
        "boundary": value.boundary,
        "timed_input_kind": value.timed_input_kind,
        "timed_input_sha256": value.timed_input_sha256,
        "measurement_parameters": dict(value.measurement_parameters),
        "workload_id": value.workload_id,
        "workload_sha256": value.workload_sha256,
        "workload_manifest_sha256": value.workload_manifest_sha256,
        "host_context_sha256": value.host_context_sha256,
        "baseline_binary_sha256": value.baseline_binary_sha256,
        "candidate_binary_sha256": value.candidate_binary_sha256,
        "target_metric": value.target_metric,
        "direction": value.direction,
        "classification": value.classification,
        "target_median_change_percent": value.target_median_change_percent,
        "baseline_relative_mad_percent": value.baseline_relative_mad_percent,
        "candidate_relative_mad_percent": value.candidate_relative_mad_percent,
        "peak_rss_change_percent": value.peak_rss_change_percent,
        "abba_blocks": str(value.abba_blocks),
    }


def experiment_to_dict(value: Experiment) -> dict[str, object]:
    return {
        "experiment_id": value.experiment_id,
        "policy": value.policy,
        "classification": value.classification,
        "target_comparison_id": value.target_comparison_id,
        "control_comparison_ids": list(value.control_comparison_ids),
        "threshold_result": value.threshold_result,
        "decision": value.decision,
        "target_median_change_percent": value.target_median_change_percent,
        "noise_gate_percent": value.noise_gate_percent,
        "worst_control_change_percent": value.worst_control_change_percent,
        "worst_peak_rss_change_percent": value.worst_peak_rss_change_percent,
        "correctness_gate": value.correctness_gate,
        "complexity_gate": value.complexity_gate,
        "note_path": value.note_path,
        "note_sha256": value.note_sha256,
        "rationale": value.rationale,
    }


def report_to_dict(value: BenchmarkReport) -> dict[str, object]:
    return {
        "schema": REPORT_SCHEMA,
        "classification": value.classification,
        "suite_label": value.suite_label,
        "host_context_sha256": value.host_context_sha256,
        "workload_manifest_sha256s": list(value.workload_manifest_sha256s),
        "environment_sha256s": list(value.environment_sha256s),
        "source_observations": list(value.source_observations),
        "groups": [statistics_to_dict(group) for group in value.groups],
        "comparisons": [comparison_to_dict(comparison) for comparison in value.comparisons],
        "experiments": [experiment_to_dict(experiment) for experiment in value.experiments],
        "limitations": list(value.limitations),
    }


def report_from_dict(value: Mapping[str, object]) -> BenchmarkReport:
    _require_keys(
        value,
        {
            "schema",
            "classification",
            "suite_label",
            "host_context_sha256",
            "workload_manifest_sha256s",
            "environment_sha256s",
            "source_observations",
            "groups",
            "comparisons",
            "experiments",
            "limitations",
        },
    )
    if value["schema"] != REPORT_SCHEMA:
        raise ValueError("unsupported benchmark report schema")
    groups: list[GroupStatistics] = []
    for raw in _array(value["groups"], "groups"):
        mapping = _mapping(raw, "group")
        _require_keys(
            mapping,
            {
                "boundary",
                "timed_input_kind",
                "timed_input_sha256",
                "measurement_parameters",
                "workload_id",
                "workload_sha256",
                "workload_manifest_sha256",
                "binary_sha256",
                "valid_observations",
                "invalid_observations",
                "commands",
                "resting_order_denominator",
                "minimum_elapsed_ns",
                "maximum_elapsed_ns",
                "median_elapsed_ns",
                "mad_elapsed_ns",
                "iqr_elapsed_ns",
                "minimum_commands_per_second",
                "maximum_commands_per_second",
                "median_commands_per_second",
                "mad_commands_per_second",
                "iqr_commands_per_second",
                "minimum_events_per_second",
                "maximum_events_per_second",
                "median_events_per_second",
                "mad_events_per_second",
                "iqr_events_per_second",
                "latency_sample_count",
                "minimum_latency_ns",
                "maximum_latency_ns",
                "latency_quantiles_ns",
                "peak_rss_bytes",
                "process_rss_delta_bytes",
                "process_rss_delta_bytes_per_command",
                "process_rss_delta_bytes_per_resting_order",
                "allocation_count",
                "deallocation_count",
                "allocated_bytes",
                "live_bytes",
                "peak_live_bytes",
                "allocations_per_command",
            },
        )
        raw_quantiles = _mapping(mapping["latency_quantiles_ns"], "latency_quantiles_ns")
        raw_minimum_latency = mapping["minimum_latency_ns"]
        raw_maximum_latency = mapping["maximum_latency_ns"]
        groups.append(
            GroupStatistics(
                boundary=_string(mapping["boundary"], "boundary"),
                timed_input_kind=_string(mapping["timed_input_kind"], "timed_input_kind"),
                timed_input_sha256=(
                    None
                    if mapping["timed_input_sha256"] is None
                    else _string(mapping["timed_input_sha256"], "timed_input_sha256")
                ),
                measurement_parameters=tuple(
                    (
                        _string(name, "measurement parameter name"),
                        _string(parameter, f"measurement parameter {name}"),
                    )
                    for name, parameter in _mapping(
                        mapping["measurement_parameters"], "measurement_parameters"
                    ).items()
                ),
                workload_id=_string(mapping["workload_id"], "workload_id"),
                workload_sha256=_string(mapping["workload_sha256"], "workload_sha256"),
                workload_manifest_sha256=_string(
                    mapping["workload_manifest_sha256"], "workload_manifest_sha256"
                ),
                binary_sha256=_string(mapping["binary_sha256"], "binary_sha256"),
                valid_observations=_decimal(
                    mapping["valid_observations"], "valid_observations", U64_MAX
                ),
                invalid_observations=_decimal(
                    mapping["invalid_observations"], "invalid_observations", U64_MAX
                ),
                commands=_decimal(mapping["commands"], "commands", U64_MAX),
                resting_order_denominator=_decimal(
                    mapping["resting_order_denominator"],
                    "resting_order_denominator",
                    U64_MAX,
                ),
                minimum_elapsed_ns=_decimal(
                    mapping["minimum_elapsed_ns"], "minimum_elapsed_ns", U64_MAX
                ),
                maximum_elapsed_ns=_decimal(
                    mapping["maximum_elapsed_ns"], "maximum_elapsed_ns", U64_MAX
                ),
                median_elapsed_ns=_rate(mapping["median_elapsed_ns"], "median_elapsed_ns"),
                mad_elapsed_ns=_rate(mapping["mad_elapsed_ns"], "mad_elapsed_ns"),
                iqr_elapsed_ns=_decimal(mapping["iqr_elapsed_ns"], "iqr_elapsed_ns", U64_MAX),
                minimum_commands_per_second=_optional_rate(
                    mapping["minimum_commands_per_second"], "minimum_commands_per_second"
                ),
                maximum_commands_per_second=_optional_rate(
                    mapping["maximum_commands_per_second"], "maximum_commands_per_second"
                ),
                median_commands_per_second=_optional_rate(
                    mapping["median_commands_per_second"], "median_commands_per_second"
                ),
                mad_commands_per_second=_optional_rate(
                    mapping["mad_commands_per_second"], "mad_commands_per_second"
                ),
                iqr_commands_per_second=_optional_rate(
                    mapping["iqr_commands_per_second"], "iqr_commands_per_second"
                ),
                minimum_events_per_second=_optional_rate(
                    mapping["minimum_events_per_second"], "minimum_events_per_second"
                ),
                maximum_events_per_second=_optional_rate(
                    mapping["maximum_events_per_second"], "maximum_events_per_second"
                ),
                median_events_per_second=_optional_rate(
                    mapping["median_events_per_second"], "median_events_per_second"
                ),
                mad_events_per_second=_optional_rate(
                    mapping["mad_events_per_second"], "mad_events_per_second"
                ),
                iqr_events_per_second=_optional_rate(
                    mapping["iqr_events_per_second"], "iqr_events_per_second"
                ),
                latency_sample_count=_decimal(
                    mapping["latency_sample_count"], "latency_sample_count", U64_MAX
                ),
                minimum_latency_ns=(
                    None
                    if raw_minimum_latency is None
                    else _decimal(raw_minimum_latency, "minimum_latency_ns", U64_MAX)
                ),
                maximum_latency_ns=(
                    None
                    if raw_maximum_latency is None
                    else _decimal(raw_maximum_latency, "maximum_latency_ns", U64_MAX)
                ),
                latency_quantiles_ns=tuple(
                    (name, _decimal(sample, f"quantile {name}", U64_MAX))
                    for name, sample in raw_quantiles.items()
                ),
                peak_rss_bytes=_optional_scalar_statistics(
                    mapping["peak_rss_bytes"],
                    "peak_rss_bytes",
                ),
                process_rss_delta_bytes=_optional_scalar_statistics(
                    mapping["process_rss_delta_bytes"],
                    "process_rss_delta_bytes",
                ),
                process_rss_delta_bytes_per_command=_optional_scalar_statistics(
                    mapping["process_rss_delta_bytes_per_command"],
                    "process_rss_delta_bytes_per_command",
                ),
                process_rss_delta_bytes_per_resting_order=_optional_scalar_statistics(
                    mapping["process_rss_delta_bytes_per_resting_order"],
                    "process_rss_delta_bytes_per_resting_order",
                ),
                allocation_count=_optional_scalar_statistics(
                    mapping["allocation_count"], "allocation_count"
                ),
                deallocation_count=_optional_scalar_statistics(
                    mapping["deallocation_count"], "deallocation_count"
                ),
                allocated_bytes=_optional_scalar_statistics(
                    mapping["allocated_bytes"], "allocated_bytes"
                ),
                live_bytes=_optional_scalar_statistics(mapping["live_bytes"], "live_bytes"),
                peak_live_bytes=_optional_scalar_statistics(
                    mapping["peak_live_bytes"], "peak_live_bytes"
                ),
                allocations_per_command=_optional_scalar_statistics(
                    mapping["allocations_per_command"], "allocations_per_command"
                ),
            )
        )
    comparisons: list[Comparison] = []
    for raw in _array(value["comparisons"], "comparisons"):
        mapping = _mapping(raw, "comparison")
        _require_keys(
            mapping,
            {
                "comparison_id",
                "boundary",
                "timed_input_kind",
                "timed_input_sha256",
                "measurement_parameters",
                "workload_id",
                "workload_sha256",
                "workload_manifest_sha256",
                "host_context_sha256",
                "baseline_binary_sha256",
                "candidate_binary_sha256",
                "target_metric",
                "direction",
                "classification",
                "target_median_change_percent",
                "baseline_relative_mad_percent",
                "candidate_relative_mad_percent",
                "peak_rss_change_percent",
                "abba_blocks",
            },
        )
        comparisons.append(
            Comparison(
                comparison_id=_string(mapping["comparison_id"], "comparison_id"),
                boundary=_string(mapping["boundary"], "boundary"),
                timed_input_kind=_string(mapping["timed_input_kind"], "timed_input_kind"),
                timed_input_sha256=(
                    None
                    if mapping["timed_input_sha256"] is None
                    else _string(mapping["timed_input_sha256"], "timed_input_sha256")
                ),
                measurement_parameters=tuple(
                    (
                        _string(name, "measurement parameter name"),
                        _string(parameter, f"measurement parameter {name}"),
                    )
                    for name, parameter in _mapping(
                        mapping["measurement_parameters"], "measurement_parameters"
                    ).items()
                ),
                workload_id=_string(mapping["workload_id"], "workload_id"),
                workload_sha256=_string(mapping["workload_sha256"], "workload_sha256"),
                workload_manifest_sha256=_string(
                    mapping["workload_manifest_sha256"], "workload_manifest_sha256"
                ),
                host_context_sha256=_string(mapping["host_context_sha256"], "host_context_sha256"),
                baseline_binary_sha256=_string(
                    mapping["baseline_binary_sha256"], "baseline_binary_sha256"
                ),
                candidate_binary_sha256=_string(
                    mapping["candidate_binary_sha256"], "candidate_binary_sha256"
                ),
                target_metric=_string(mapping["target_metric"], "target_metric"),
                direction=_string(mapping["direction"], "direction"),
                classification=_string(mapping["classification"], "classification"),
                target_median_change_percent=_signed_rate(
                    mapping["target_median_change_percent"],
                    "target_median_change_percent",
                ),
                baseline_relative_mad_percent=_rate(
                    mapping["baseline_relative_mad_percent"],
                    "baseline_relative_mad_percent",
                ),
                candidate_relative_mad_percent=_rate(
                    mapping["candidate_relative_mad_percent"],
                    "candidate_relative_mad_percent",
                ),
                peak_rss_change_percent=(
                    None
                    if mapping["peak_rss_change_percent"] is None
                    else _signed_rate(
                        mapping["peak_rss_change_percent"],
                        "peak_rss_change_percent",
                    )
                ),
                abba_blocks=_decimal(mapping["abba_blocks"], "abba_blocks", U64_MAX),
            )
        )
    experiments: list[Experiment] = []
    for raw in _array(value["experiments"], "experiments"):
        mapping = _mapping(raw, "experiment")
        _require_keys(
            mapping,
            {
                "experiment_id",
                "policy",
                "classification",
                "target_comparison_id",
                "control_comparison_ids",
                "threshold_result",
                "decision",
                "target_median_change_percent",
                "noise_gate_percent",
                "worst_control_change_percent",
                "worst_peak_rss_change_percent",
                "correctness_gate",
                "complexity_gate",
                "note_path",
                "note_sha256",
                "rationale",
            },
        )
        target_id = mapping["target_comparison_id"]
        experiments.append(
            Experiment(
                experiment_id=_string(mapping["experiment_id"], "experiment_id"),
                policy=_string(mapping["policy"], "policy"),
                classification=_string(mapping["classification"], "classification"),
                target_comparison_id=(
                    None if target_id is None else _string(target_id, "target_comparison_id")
                ),
                control_comparison_ids=tuple(
                    _string(item, "control comparison ID")
                    for item in _array(mapping["control_comparison_ids"], "control_comparison_ids")
                ),
                threshold_result=_string(mapping["threshold_result"], "threshold_result"),
                decision=_string(mapping["decision"], "decision"),
                target_median_change_percent=_optional_signed_rate(
                    mapping["target_median_change_percent"],
                    "target_median_change_percent",
                ),
                noise_gate_percent=_optional_signed_rate(
                    mapping["noise_gate_percent"], "noise_gate_percent"
                ),
                worst_control_change_percent=_optional_signed_rate(
                    mapping["worst_control_change_percent"],
                    "worst_control_change_percent",
                ),
                worst_peak_rss_change_percent=_optional_signed_rate(
                    mapping["worst_peak_rss_change_percent"],
                    "worst_peak_rss_change_percent",
                ),
                correctness_gate=_string(mapping["correctness_gate"], "correctness_gate"),
                complexity_gate=_string(mapping["complexity_gate"], "complexity_gate"),
                note_path=_string(mapping["note_path"], "note_path"),
                note_sha256=_string(mapping["note_sha256"], "note_sha256"),
                rationale=_string(mapping["rationale"], "rationale"),
            )
        )
    return BenchmarkReport(
        classification=_string(value["classification"], "classification"),
        suite_label=_string(value["suite_label"], "suite_label"),
        host_context_sha256=_string(value["host_context_sha256"], "host_context_sha256"),
        workload_manifest_sha256s=tuple(
            _string(item, "workload manifest digest")
            for item in _array(value["workload_manifest_sha256s"], "workload_manifest_sha256s")
        ),
        environment_sha256s=tuple(
            _string(item, "environment digest")
            for item in _array(value["environment_sha256s"], "environment_sha256s")
        ),
        source_observations=tuple(
            _string(item, "source observation")
            for item in _array(value["source_observations"], "source_observations")
        ),
        groups=tuple(groups),
        comparisons=tuple(comparisons),
        experiments=tuple(experiments),
        limitations=tuple(
            _string(item, "limitation") for item in _array(value["limitations"], "limitations")
        ),
    )


def parse_canonical_document(
    data: bytes,
    decoder: Callable[[Mapping[str, object]], T],
) -> T:
    """Parse a canonical schema document and reject duplicate/non-ASCII forms."""

    try:
        text = data.decode("ascii", errors="strict")
    except UnicodeDecodeError as exc:
        raise ValueError("evidence must be ASCII") from exc
    if not text.endswith("\n") or "\r" in text or text.count("\n") != 1:
        raise ValueError("evidence must be one LF-terminated JSON record")
    try:
        raw = json.loads(
            text,
            object_pairs_hook=_unique_object,
            parse_int=_reject_number,
            parse_float=_reject_number,
            parse_constant=_reject_number,
        )
    except json.JSONDecodeError as exc:
        raise ValueError("evidence is not valid JSON") from exc
    mapping = _mapping(raw, "evidence")
    decoded = decoder(mapping)
    encoder = _encoder_for(decoded)
    if data != canonical_json_bytes(encoder(decoded)):
        raise ValueError("evidence is not canonical")
    return decoded


def read_canonical_document(path: Path, decoder: Callable[[Mapping[str, object]], T]) -> T:
    limits: dict[object, int] = {
        workload_from_dict: WORKLOAD_DOCUMENT_MAX_BYTES,
        environment_from_dict: ENVIRONMENT_DOCUMENT_MAX_BYTES,
        observation_from_dict: OBSERVATION_DOCUMENT_MAX_BYTES,
        report_from_dict: REPORT_DOCUMENT_MAX_BYTES,
    }
    limit = limits.get(decoder)
    if limit is None:
        raise ValueError("decoder does not name a bounded evidence schema")
    return parse_canonical_document(_bounded_file_bytes(path, limit), decoder)


def write_canonical_document(path: Path, value: object) -> str:
    mapping = _encoder_for(value)(value)
    encoded = canonical_json_bytes(mapping)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_bytes(encoded)
    temporary.replace(path)
    return hashlib.sha256(encoded).hexdigest()


def file_sha256(path: Path) -> str:
    hasher = hashlib.sha256()
    try:
        with path.open("rb") as source:
            while block := source.read(1024 * 1024):
                hasher.update(block)
    except OSError as exc:
        raise ValueError(f"cannot hash file: {path}") from exc
    return hasher.hexdigest()


def _encoder_for(value: object) -> Callable[[object], Mapping[str, object]]:
    if isinstance(value, WorkloadManifest):
        return cast(Callable[[object], Mapping[str, object]], workload_to_dict)
    if isinstance(value, EnvironmentManifest):
        return cast(Callable[[object], Mapping[str, object]], environment_to_dict)
    if isinstance(value, Observation):
        return cast(Callable[[object], Mapping[str, object]], observation_to_dict)
    if isinstance(value, BenchmarkReport):
        return cast(Callable[[object], Mapping[str, object]], report_to_dict)
    raise TypeError(f"unsupported performance evidence type: {type(value)!r}")


def decoder_for_schema(schema: object) -> Callable[[Mapping[str, object]], object]:
    if schema == WORKLOAD_SCHEMA:
        return workload_from_dict
    if schema == ENVIRONMENT_SCHEMA:
        return environment_from_dict
    if schema == OBSERVATION_SCHEMA:
        return observation_from_dict
    if schema == REPORT_SCHEMA:
        return report_from_dict
    raise ValueError("unknown performance evidence schema")


def read_any_canonical_document(path: Path) -> object:
    data = _bounded_file_bytes(path, REPORT_DOCUMENT_MAX_BYTES)
    try:
        raw = json.loads(
            data.decode("ascii"),
            object_pairs_hook=_unique_object,
            parse_int=_reject_number,
            parse_float=_reject_number,
            parse_constant=_reject_number,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot identify evidence schema: {path}") from exc
    mapping = _mapping(raw, "evidence")
    decoder = decoder_for_schema(mapping.get("schema"))
    limits: dict[object, int] = {
        workload_from_dict: WORKLOAD_DOCUMENT_MAX_BYTES,
        environment_from_dict: ENVIRONMENT_DOCUMENT_MAX_BYTES,
        observation_from_dict: OBSERVATION_DOCUMENT_MAX_BYTES,
        report_from_dict: REPORT_DOCUMENT_MAX_BYTES,
    }
    if len(data) > limits[decoder]:
        raise ValueError("evidence document exceeds its schema bound")
    return parse_canonical_document(data, decoder)


def _bounded_file_bytes(path: Path, maximum: int) -> bytes:
    try:
        size = path.stat().st_size
        if size < 0 or size > maximum:
            raise ValueError("evidence document exceeds its schema bound")
        with path.open("rb") as source:
            data = source.read(maximum + 1)
            if len(data) != size or len(data) > maximum:
                raise ValueError("evidence document changed or exceeds its schema bound")
            return data
    except OSError as exc:
        raise ValueError(f"cannot read evidence document: {path}") from exc


def _unique_object(pairs: list[tuple[str, JsonValue]]) -> dict[str, JsonValue]:
    result: dict[str, JsonValue] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _reject_number(value: str) -> float:
    raise ValueError(f"non-integer JSON number is forbidden: {value}")


def _require_keys(value: Mapping[str, object], expected: set[str]) -> None:
    if set(value) != expected:
        raise ValueError("evidence mapping has unexpected or missing fields")


def _mapping(value: object, name: str) -> Mapping[str, object]:
    if not isinstance(value, dict) or any(not isinstance(key, str) for key in value):
        raise ValueError(f"{name} must be an object")
    return value


def _array(value: object, name: str) -> Sequence[object]:
    if not isinstance(value, list):
        raise ValueError(f"{name} must be an array")
    return value


def _string(value: object, name: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{name} must be a string")
    return value


def _decimal(value: object, name: str, maximum: int) -> int:
    text = _string(value, name)
    if _DECIMAL.fullmatch(text) is None:
        raise ValueError(f"{name} must be canonical unsigned decimal")
    parsed = int(text)
    if parsed > maximum:
        raise ValueError(f"{name} exceeds its representation")
    return parsed


def _signed_decimal(value: object, name: str) -> int:
    text = _string(value, name)
    if text == "-0" or re.fullmatch(r"(?:0|-[1-9][0-9]*|[1-9][0-9]*)", text) is None:
        raise ValueError(f"{name} must be canonical signed decimal")
    parsed = int(text)
    if not -(1 << 63) <= parsed <= (1 << 63) - 1:
        raise ValueError(f"{name} exceeds i64")
    return parsed


def _rate(value: object, name: str) -> str:
    text = _string(value, name)
    _require_rate(name, text)
    return text


def _optional_rate(value: object, name: str) -> str | None:
    return None if value is None else _rate(value, name)


def _optional_scalar_statistics(value: object, name: str) -> ScalarStatistics | None:
    if value is None:
        return None
    mapping = _mapping(value, name)
    _require_keys(mapping, {"minimum", "maximum", "median", "mad", "iqr"})
    return ScalarStatistics(
        minimum=_signed_rate(mapping["minimum"], f"{name}.minimum"),
        maximum=_signed_rate(mapping["maximum"], f"{name}.maximum"),
        median=_signed_rate(mapping["median"], f"{name}.median"),
        mad=_rate(mapping["mad"], f"{name}.mad"),
        iqr=_rate(mapping["iqr"], f"{name}.iqr"),
    )


def _signed_rate(value: object, name: str) -> str:
    text = _string(value, name)
    _require_signed_rate(name, text)
    return text


def _optional_signed_rate(value: object, name: str) -> str | None:
    return None if value is None else _signed_rate(value, name)


def _require_uint(name: str, value: int, maximum: int, *, nonzero: bool = False) -> None:
    minimum = int(nonzero)
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise ValueError(f"{name} must be an integer in [{minimum}, {maximum}]")


def _require_digest(name: str, value: str) -> None:
    if not isinstance(value, str) or _DIGEST.fullmatch(value) is None:
        raise ValueError(f"{name} must be lowercase SHA-256 hex")


def _require_identifier(name: str, value: str, *, allow_long: bool = False) -> None:
    if not isinstance(value, str) or not value.isascii():
        raise ValueError(f"{name} must be ASCII")
    if allow_long:
        if not value or len(value) > 256 or re.fullmatch(r"[A-Za-z0-9_.+-]+", value) is None:
            raise ValueError(f"{name} is not a safe identifier")
    elif _IDENTIFIER.fullmatch(value) is None:
        raise ValueError(f"{name} is not a safe identifier")


def _require_suite_label(value: str) -> None:
    _require_identifier("suite_label", value)
    if len(value) > 32:
        raise ValueError("suite_label must contain at most 32 characters")


def _require_parameter_pairs(name: str, value: tuple[tuple[str, str], ...]) -> None:
    if len(value) > MAX_PARAMETERS:
        raise ValueError(f"{name} exceeds its entry limit")
    parameter_names = tuple(parameter_name for parameter_name, _ in value)
    if parameter_names != tuple(sorted(parameter_names)) or len(parameter_names) != len(
        set(parameter_names)
    ):
        raise ValueError(f"{name} must have unique sorted names")
    for parameter_name, parameter_value in value:
        _require_identifier(f"{name} name", parameter_name)
        if (
            not isinstance(parameter_value, str)
            or _MEASUREMENT_VALUE.fullmatch(parameter_value) is None
        ):
            raise ValueError(f"{name}.{parameter_name} is not a safe measurement value")


def _require_safe_relative_path(name: str, value: str) -> None:
    if (
        not isinstance(value, str)
        or not value
        or len(value) > 512
        or not value.isascii()
        or "\\" in value
        or ":" in value
        or value.startswith("/")
        or PurePosixPath(value).as_posix() != value
        or any(part in {"", ".", ".."} for part in PurePosixPath(value).parts)
    ):
        raise ValueError(f"{name} must be a safe relative POSIX path")


def _validate_timed_input(boundary: str, kind: str, digest: str | None) -> None:
    if kind not in {"none", "atlslg01"}:
        raise ValueError("timed_input_kind is outside the V1 vocabulary")
    if kind == "none":
        if digest is not None:
            raise ValueError("timed_input_sha256 must be null when timed_input_kind is none")
    else:
        if digest is None:
            raise ValueError("replay timing requires timed_input_sha256")
        _require_digest("timed_input_sha256", digest)
    if boundary in {"replay_fast", "replay_verify"}:
        if kind != "atlslg01":
            raise ValueError("replay boundaries require an ATLSLG01 timed input")
    elif kind != "none":
        raise ValueError("non-replay boundaries cannot carry a timed input")


def _require_profile_pairs(name: str, value: tuple[tuple[str, str], ...]) -> None:
    if not value:
        raise ValueError(f"{name} must not be empty")
    if len(value) > MAX_BUILD_TARGET_PROFILES:
        raise ValueError(f"{name} exceeds its entry limit")
    profile_names = tuple(profile_name for profile_name, _ in value)
    if profile_names != tuple(sorted(profile_names)) or len(profile_names) != len(
        set(profile_names)
    ):
        raise ValueError(f"{name} must have unique sorted names")
    for profile_name, profile_value in value:
        _require_identifier(f"{name} name", profile_name)
        _require_public_text(f"{name}.{profile_name}", profile_value)


def _require_rate(name: str, value: str) -> None:
    if (
        not isinstance(value, str)
        or len(value) > 256
        or _RATE.fullmatch(value) is None
        or (
            "." in value
            and (value.endswith("0") or value.startswith("0") and not value.startswith("0."))
        )
    ):
        raise ValueError(f"{name} must be canonical nonnegative decimal")


def _require_signed_rate(name: str, value: str) -> None:
    if not isinstance(value, str) or len(value) > 256:
        raise ValueError(f"{name} must be canonical signed decimal")
    unsigned = value[1:] if value.startswith("-") else value
    if value == "-0" or value.startswith("+") or _RATE.fullmatch(unsigned) is None:
        raise ValueError(f"{name} must be canonical signed decimal")
    if "." in unsigned and unsigned.endswith("0"):
        raise ValueError(f"{name} must not contain trailing fractional zeroes")


def _require_public_text(name: str, value: str) -> None:
    if (
        not isinstance(value, str)
        or not value
        or len(value) > MAX_PUBLIC_TEXT_BYTES
        or not value.isascii()
        or "\n" in value
        or "\r" in value
    ):
        raise ValueError(f"{name} must be nonempty single-line ASCII")
    if (
        "@" in value
        or _IPV4.search(value)
        or _IPV6.search(value)
        or _WINDOWS_PATH.search(value)
        or _UNIX_HOME.search(value)
    ):
        raise ValueError(f"{name} contains private host or path information")
