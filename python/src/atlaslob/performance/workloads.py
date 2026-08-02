"""Frozen benchmark workload catalog and deterministic materialization."""

from __future__ import annotations

import hashlib
import json
import os
import secrets
import subprocess
from bisect import bisect_right
from collections import deque
from collections.abc import Iterator
from dataclasses import dataclass, replace
from pathlib import Path
from typing import BinaryIO, Final, Protocol

from atlaslob.canonical import event_digest
from atlaslob.domain import (
    U64_MAX,
    CancelOrder,
    Command,
    InstrumentConfig,
    MatchingConfig,
    MultiInstrumentEngineConfig,
    NewOrder,
    OrderType,
    ReplaceOrder,
    Side,
    TimeInForce,
)
from atlaslob.generation import (
    OperationWeights,
    SplitMix64,
    WorkloadProfile,
    WorkloadSpec,
    resolve_workload_spec,
)
from atlaslob.multi_generation import (
    MULTI_GENERATOR_VERSION,
    MultiWorkloadSpec,
)
from atlaslob.multi_native import (
    MultiNativeInput,
    encode_multi_command,
    encode_multi_header,
    encode_multi_instrument,
)
from atlaslob.performance.schemas import (
    BOUNDARIES,
    MAX_WORKLOAD_COUNT_VECTOR_CHARS,
    CatalogEntry,
    WorkloadManifest,
    canonical_json_bytes,
    file_sha256,
    read_canonical_document,
    validate_workload_parameters,
    workload_from_dict,
    workload_to_dict,
)
from atlaslob.router import ReferenceRouter

DEFERRED_WORKLOADS: Final = {"W11": "gateway fragmentation is deferred to Phase 6"}
WORKLOAD_IDS: Final = tuple(f"W{index:02d}" for index in range(1, 11)) + ("W12",)
BENCHMARK_GENERATOR_VERSION: Final = 1
MAX_BENCHMARK_COMMANDS: Final = 100_000_000
MAX_BENCHMARK_INSTRUMENTS: Final = 4_096
MAX_BENCHMARK_PLAN_BYTES: Final = 1 << 20
MAX_BENCHMARK_STREAM_LINE_BYTES: Final = 1_024
W12_CYCLE_COMMANDS: Final = 100
BENCHMARK_PLAN_SCHEMA: Final = "ATLAS_BENCH_PLAN_V1"
LOG_MATERIALIZATION_SCHEMA: Final = "ATLAS_BENCH_LOG_MATERIALIZATION_V1"

_PLAN_TIERS: Final = frozenset({"smoke", "study", "headline", "memory", "python", "replay"})
_PYTHON_OUTPUT_MODES: Final = frozenset({"objects", "columns", "summary"})
_PYTHON_BATCH_SIZES: Final = (1, 64, 1_024, 65_536)
_LATENCY_SAMPLE_STRIDE: Final = 32
_LOG_RECEIPT_KEYS: Final = {
    "committed",
    "event_digest",
    "events",
    "final_digest",
    "log_id",
    "log_sha256",
    "records",
    "rejected",
    "schema",
    "workload_sha256",
}


@dataclass(frozen=True, slots=True)
class BenchmarkPlanPoint:
    point_id: str
    tier: str
    workload_id: str
    seed: int
    preload_commands: int
    warmup_commands: int
    measured_commands: int
    active_order_target: int
    instrument_count: int
    sweep_depth: int
    boundaries: tuple[str, ...]
    python_batch_sizes: tuple[int, ...] = ()
    python_output_modes: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        _require_plan_identifier("point_id", self.point_id)
        if self.tier not in _PLAN_TIERS:
            raise ValueError("benchmark plan tier is outside the V1 vocabulary")
        for name, value in (
            ("seed", self.seed),
            ("preload_commands", self.preload_commands),
            ("warmup_commands", self.warmup_commands),
            ("measured_commands", self.measured_commands),
            ("active_order_target", self.active_order_target),
            ("instrument_count", self.instrument_count),
            ("sweep_depth", self.sweep_depth),
        ):
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                raise ValueError(f"benchmark plan {name} must be a canonical uint")
        if self.measured_commands == 0:
            raise ValueError("benchmark plan measured_commands must be nonzero")
        if (
            self.preload_commands + self.warmup_commands + self.measured_commands
            > MAX_BENCHMARK_COMMANDS
        ):
            raise ValueError("benchmark plan point exceeds the shared command limit")
        build_workload_spec(
            self.workload_id,
            command_count=(self.preload_commands + self.warmup_commands + self.measured_commands),
            instrument_count=self.instrument_count,
            active_order_target=self.active_order_target,
            sweep_depth=self.sweep_depth,
        )
        _validate_region_shape(
            self.workload_id,
            preload_commands=self.preload_commands,
            warmup_commands=self.warmup_commands,
            measured_commands=self.measured_commands,
            active_order_target=self.active_order_target,
            sweep_depth=self.sweep_depth,
        )
        if (
            not self.boundaries
            or self.boundaries != tuple(sorted(set(self.boundaries)))
            or any(boundary not in BOUNDARIES for boundary in self.boundaries)
        ):
            raise ValueError("benchmark plan boundaries must be nonempty, unique, and sorted")
        if "core_latency" in self.boundaries and self.measured_commands < _LATENCY_SAMPLE_STRIDE:
            raise ValueError("latency plan points must produce at least one frozen-stride sample")
        if self.workload_id == "W10":
            if any(not boundary.startswith("replay_") for boundary in self.boundaries):
                raise ValueError("W10 plan points permit only replay boundaries")
        elif any(boundary.startswith("replay_") for boundary in self.boundaries):
            raise ValueError("replay boundaries require W10")
        python_boundaries = tuple(
            boundary for boundary in self.boundaries if boundary.startswith("python_")
        )
        if python_boundaries:
            if self.workload_id != "W04":
                raise ValueError("Python batch study points require W04")
            if self.python_batch_sizes != _PYTHON_BATCH_SIZES:
                raise ValueError("Python batch sizes must be exactly 1, 64, 1024, and 65536")
            if (
                not self.python_output_modes
                or self.python_output_modes != tuple(sorted(set(self.python_output_modes)))
                or any(mode not in _PYTHON_OUTPUT_MODES for mode in self.python_output_modes)
            ):
                raise ValueError("Python output modes must be nonempty, unique, and sorted")
            expected_boundaries = tuple(f"python_{mode}" for mode in self.python_output_modes)
            if python_boundaries != expected_boundaries:
                raise ValueError("Python boundaries and output modes must agree")
        elif self.python_batch_sizes or self.python_output_modes:
            raise ValueError("non-Python points cannot carry Python dimensions")


@dataclass(frozen=True, slots=True)
class VerifiedWorkload:
    """Immutable proof that exact workload bytes passed semantic verification."""

    manifest_path: Path
    manifest_sha256: str
    manifest: WorkloadManifest
    stream_path: Path
    stream_sha256: str
    timed_input_path: Path | None
    timed_input_sha256: str | None


@dataclass(frozen=True, slots=True)
class BenchmarkPlan:
    plan_id: str
    points: tuple[BenchmarkPlanPoint, ...]

    def __post_init__(self) -> None:
        _require_plan_identifier("plan_id", self.plan_id)
        identifiers = tuple(point.point_id for point in self.points)
        if not identifiers or identifiers != tuple(sorted(set(identifiers))):
            raise ValueError("benchmark plan point IDs must be nonempty, unique, and sorted")


_PROFILE_BY_ID: Final = {
    "W01": WorkloadProfile.UNIFORM_SYNTHETIC,
    "W02": WorkloadProfile.HOT_LEVEL_CONTENTION,
    "W03": WorkloadProfile.SPARSE_WIDE,
    "W04": WorkloadProfile.CLUSTERED_MID,
    "W05": WorkloadProfile.SWEEP_HEAVY,
    "W06": WorkloadProfile.CANCEL_HEAVY,
    "W07": WorkloadProfile.REPLACE_HEAVY,
    "W08": WorkloadProfile.SWEEP_HEAVY,
    "W09": WorkloadProfile.TRACE_DRIVEN_SYNTHETIC,
    "W10": WorkloadProfile.CLUSTERED_MID,
    "W12": WorkloadProfile.ADVERSARIAL_BOUNDARY,
}

_WEIGHTS_BY_ID: Final = {
    "W01": OperationWeights(100, 0, 0),
    "W02": OperationWeights(55, 45, 0),
    "W03": OperationWeights(70, 15, 15),
    "W04": OperationWeights(55, 35, 10),
    "W05": OperationWeights(80, 10, 10),
    "W06": OperationWeights(35, 60, 5),
    "W07": OperationWeights(35, 10, 55),
    "W08": OperationWeights(90, 5, 5),
    "W09": OperationWeights(55, 35, 10),
    "W10": OperationWeights(55, 35, 10),
    "W12": OperationWeights(55, 20, 25),
}


class _ByteHasher(Protocol):
    def update(self, data: bytes) -> object: ...


def workload_description(workload_id: str) -> str:
    descriptions = {
        "W01": "resting inserts",
        "W02": "hot-level FIFO",
        "W03": "wide sparse book",
        "W04": "balanced market",
        "W05": "crossing sweep",
        "W06": "cancel storm",
        "W07": "replace storm",
        "W08": "IOC flow",
        "W09": "multi-instrument routing",
        "W10": "replay source",
        "W12": "adversarial legal",
    }
    if workload_id in DEFERRED_WORKLOADS:
        return DEFERRED_WORKLOADS[workload_id]
    try:
        return descriptions[workload_id]
    except KeyError as exc:
        raise ValueError(f"unknown benchmark workload: {workload_id}") from exc


def benchmark_plan_to_dict(plan: BenchmarkPlan) -> dict[str, object]:
    return {
        "schema": BENCHMARK_PLAN_SCHEMA,
        "plan_id": plan.plan_id,
        "points": [
            {
                "point_id": point.point_id,
                "tier": point.tier,
                "workload_id": point.workload_id,
                "seed": str(point.seed),
                "preload_commands": str(point.preload_commands),
                "warmup_commands": str(point.warmup_commands),
                "measured_commands": str(point.measured_commands),
                "active_order_target": str(point.active_order_target),
                "instrument_count": str(point.instrument_count),
                "sweep_depth": str(point.sweep_depth),
                "boundaries": list(point.boundaries),
                "python_batch_sizes": [str(size) for size in point.python_batch_sizes],
                "python_output_modes": list(point.python_output_modes),
            }
            for point in plan.points
        ],
    }


def load_benchmark_plan(path: Path) -> BenchmarkPlan:
    """Read a strict canonical ASCII ATLAS_BENCH_PLAN_V1 input."""

    try:
        with path.open("rb") as source:
            data = source.read(MAX_BENCHMARK_PLAN_BYTES + 1)
    except OSError as exc:
        raise ValueError("benchmark plan is not readable") from exc
    if len(data) > MAX_BENCHMARK_PLAN_BYTES:
        raise ValueError("benchmark plan exceeds the 1 MiB input limit")
    try:
        text = data.decode("ascii", errors="strict")
    except UnicodeDecodeError as exc:
        raise ValueError("benchmark plan must be ASCII") from exc
    if not text.endswith("\n") or "\r" in text or text.count("\n") != 1:
        raise ValueError("benchmark plan must be one LF-terminated JSON record")
    try:
        raw = json.loads(
            text,
            object_pairs_hook=_unique_json_object,
            parse_int=_reject_json_number,
            parse_float=_reject_json_number,
            parse_constant=_reject_json_number,
        )
    except json.JSONDecodeError as exc:
        raise ValueError("benchmark plan is not valid JSON") from exc
    mapping = _plan_mapping(raw, "benchmark plan")
    _require_plan_keys(mapping, {"schema", "plan_id", "points"}, "benchmark plan")
    if mapping["schema"] != BENCHMARK_PLAN_SCHEMA:
        raise ValueError("unsupported benchmark plan schema")
    raw_points = _plan_array(mapping["points"], "points")
    points: list[BenchmarkPlanPoint] = []
    point_keys = {
        "point_id",
        "tier",
        "workload_id",
        "seed",
        "preload_commands",
        "warmup_commands",
        "measured_commands",
        "active_order_target",
        "instrument_count",
        "sweep_depth",
        "boundaries",
        "python_batch_sizes",
        "python_output_modes",
    }
    for index, raw_point in enumerate(raw_points):
        point = _plan_mapping(raw_point, f"points[{index}]")
        _require_plan_keys(point, point_keys, f"points[{index}]")
        points.append(
            BenchmarkPlanPoint(
                point_id=_plan_string(point["point_id"], "point_id"),
                tier=_plan_string(point["tier"], "tier"),
                workload_id=_plan_string(point["workload_id"], "workload_id"),
                seed=_plan_uint(point["seed"], "seed"),
                preload_commands=_plan_uint(point["preload_commands"], "preload_commands"),
                warmup_commands=_plan_uint(point["warmup_commands"], "warmup_commands"),
                measured_commands=_plan_uint(point["measured_commands"], "measured_commands"),
                active_order_target=_plan_uint(point["active_order_target"], "active_order_target"),
                instrument_count=_plan_uint(point["instrument_count"], "instrument_count"),
                sweep_depth=_plan_uint(point["sweep_depth"], "sweep_depth"),
                boundaries=tuple(
                    _plan_string(value, "boundary")
                    for value in _plan_array(point["boundaries"], "boundaries")
                ),
                python_batch_sizes=tuple(
                    _plan_uint(value, "python_batch_size")
                    for value in _plan_array(point["python_batch_sizes"], "python_batch_sizes")
                ),
                python_output_modes=tuple(
                    _plan_string(value, "python_output_mode")
                    for value in _plan_array(point["python_output_modes"], "python_output_modes")
                ),
            )
        )
    plan = BenchmarkPlan(
        plan_id=_plan_string(mapping["plan_id"], "plan_id"),
        points=tuple(points),
    )
    if data != canonical_json_bytes(benchmark_plan_to_dict(plan)):
        raise ValueError("benchmark plan is not canonical")
    return plan


def preflight_benchmark_plan(plan: BenchmarkPlan | Path) -> None:
    """Validate every unique resolved manifest shape without generating streams."""

    resolved = load_benchmark_plan(plan) if isinstance(plan, Path) else plan
    seen: set[tuple[str, int, int, int, int, int, int, int]] = set()
    for point in resolved.points:
        key = _plan_point_shape_key(point)
        if key in seen:
            continue
        seen.add(key)
        command_count = point.preload_commands + point.warmup_commands + point.measured_commands
        spec = build_workload_spec(
            point.workload_id,
            command_count=command_count,
            instrument_count=point.instrument_count,
            active_order_target=point.active_order_target,
            sweep_depth=point.sweep_depth,
        )
        maximum_counts = _maximal_count_vector(point.instrument_count, command_count)
        parameters = _resolved_parameters(
            point.workload_id,
            spec,
            active_order_target=point.active_order_target,
            instrument_count=point.instrument_count,
            sweep_depth=point.sweep_depth,
            preload_commands=point.preload_commands,
            warmup_commands=point.warmup_commands,
            measured_commands=point.measured_commands,
            after_preload_active_order_count=point.active_order_target,
            measured_start_active_order_count=point.active_order_target,
            actual_instrument_command_counts=maximum_counts,
        )
        validate_workload_parameters(parameters)


def _maximal_count_vector(entry_count: int, total_count: int) -> tuple[int, ...]:
    """Return a bounded count vector with the longest possible decimal CSV."""

    if not 1 <= entry_count <= MAX_BENCHMARK_INSTRUMENTS:
        raise ValueError("count-vector entry count is outside the benchmark bound")
    if not 0 <= total_count <= MAX_BENCHMARK_COMMANDS:
        raise ValueError("count-vector total is outside the benchmark bound")
    counts = [0] * entry_count
    remaining = total_count
    lower_threshold = 0
    upper_threshold = 10
    while remaining:
        increment = upper_threshold - lower_threshold
        upgraded = min(entry_count, remaining // increment)
        for index in range(upgraded):
            counts[index] = upper_threshold
        remaining -= upgraded * increment
        if upgraded < entry_count:
            counts[0] += remaining
            remaining = 0
        else:
            lower_threshold = upper_threshold
            upper_threshold *= 10
    result = tuple(counts)
    if len(",".join(str(count) for count in result)) > MAX_WORKLOAD_COUNT_VECTOR_CHARS:
        raise ValueError("resolved count vector exceeds the workload schema bound")
    return result


def materialize_benchmark_plan(
    plan: BenchmarkPlan | Path,
    output_directory: Path,
    *,
    log_materializer: Path | None = None,
    eager_invariant_checks: bool = False,
) -> tuple[tuple[Path, WorkloadManifest], ...]:
    """Materialize every resolved plan point, reusing identical stream shapes.

    Bulk materialization checks full oracle invariants at region boundaries by
    default. Passing eager_invariant_checks=True is intended for equivalence
    tests and retains the ordinary per-command oracle policy.
    """

    if not isinstance(eager_invariant_checks, bool):
        raise TypeError("eager_invariant_checks must be a bool")

    resolved = load_benchmark_plan(plan) if isinstance(plan, Path) else plan
    replay_required = any(point.workload_id == "W10" for point in resolved.points)
    if replay_required != (log_materializer is not None):
        raise ValueError("a native log materializer is required exactly when the plan contains W10")
    if log_materializer is not None:
        _require_log_materializer(log_materializer)

    preflight_benchmark_plan(resolved)

    artifacts: dict[
        tuple[str, int, int, int, int, int, int, int],
        tuple[Path, WorkloadManifest],
    ] = {}
    keys: list[tuple[str, int, int, int, int, int, int, int]] = []
    points_by_key: dict[tuple[str, int, int, int, int, int, int, int], BenchmarkPlanPoint] = {}
    planned_paths: set[Path] = set()
    for point in resolved.points:
        key = _plan_point_shape_key(point)
        keys.append(key)
        if key in points_by_key:
            continue
        points_by_key[key] = point
        for path in _final_artifact_paths(
            output_directory,
            workload_id=point.workload_id,
            seed=point.seed,
            preload_commands=point.preload_commands,
            warmup_commands=point.warmup_commands,
            measured_commands=point.measured_commands,
            instrument_count=point.instrument_count,
            active_order_target=point.active_order_target,
            sweep_depth=point.sweep_depth,
        ):
            if path in planned_paths:
                raise ValueError("benchmark plan resolves distinct points to one artifact")
            planned_paths.add(path)
    collisions = tuple(sorted(path.name for path in planned_paths if path.exists()))
    if collisions:
        raise FileExistsError(
            "benchmark plan materialization never overwrites: " + ", ".join(collisions)
        )

    ordered: list[tuple[Path, WorkloadManifest]] = []
    created: list[Path] = []
    try:
        for point, key in zip(resolved.points, keys, strict=True):
            artifact = artifacts.get(key)
            if artifact is None:
                artifact = materialize_workload(
                    point.workload_id,
                    output_directory,
                    seed=point.seed,
                    preload_commands=point.preload_commands,
                    warmup_commands=point.warmup_commands,
                    measured_commands=point.measured_commands,
                    instrument_count=point.instrument_count,
                    active_order_target=point.active_order_target,
                    sweep_depth=point.sweep_depth,
                    log_materializer=(log_materializer if point.workload_id == "W10" else None),
                    eager_invariant_checks=eager_invariant_checks,
                )
                artifacts[key] = artifact
                created.extend(_published_manifest_paths(*artifact))
            ordered.append(artifact)
    except Exception:
        _remove_paths(created)
        raise
    return tuple(ordered)


def build_workload_spec(
    workload_id: str,
    *,
    command_count: int,
    instrument_count: int = 1,
    active_order_target: int = 64,
    sweep_depth: int = 16,
) -> MultiWorkloadSpec:
    """Build a resolved V2 spec without changing Generator V1/V2 algorithms."""

    if workload_id in DEFERRED_WORKLOADS:
        raise ValueError(DEFERRED_WORKLOADS[workload_id])
    if workload_id not in WORKLOAD_IDS:
        raise ValueError(f"unknown benchmark workload: {workload_id}")
    if (
        isinstance(command_count, bool)
        or not isinstance(command_count, int)
        or not 1 <= command_count <= MAX_BENCHMARK_COMMANDS
    ):
        raise ValueError(f"command_count must be between 1 and {MAX_BENCHMARK_COMMANDS}")
    if (
        isinstance(instrument_count, bool)
        or not isinstance(instrument_count, int)
        or not 1 <= instrument_count <= MAX_BENCHMARK_INSTRUMENTS
    ):
        raise ValueError(f"instrument_count must be between 1 and {MAX_BENCHMARK_INSTRUMENTS}")
    if workload_id != "W09" and instrument_count != 1:
        raise ValueError("only W09 accepts multiple instruments")
    if command_count < instrument_count:
        raise ValueError("command_count must be at least instrument_count")
    if isinstance(active_order_target, bool) or not 1 <= active_order_target <= U64_MAX:
        raise ValueError("active_order_target is outside the supported range")
    if workload_id == "W01" and active_order_target < 64:
        raise ValueError("W01 requires at least one resting order at each of 64 levels")
    if workload_id == "W02" and (active_order_target < 16 or active_order_target % 8 != 0):
        raise ValueError(
            "W02 requires an active target divisible by eight with at least two orders per level"
        )
    if workload_id == "W04" and active_order_target < 20:
        raise ValueError("W04 requires an active target of at least 20 orders")
    if workload_id == "W09" and active_order_target > U64_MAX // 2:
        raise ValueError("W09 total active target cannot be represented as a finite capacity")
    if workload_id == "W09" and active_order_target < instrument_count:
        raise ValueError("W09 needs at least one active order per instrument")
    if workload_id in {"W08", "W12"} and active_order_target < 2:
        raise ValueError(f"{workload_id} requires protected bid and ask background orders")
    if sweep_depth not in {1, 8, 16, 32, 64}:
        raise ValueError("sweep_depth must be one of 1, 8, 16, 32, or 64")

    counts = (
        _skewed_partition(command_count, instrument_count)
        if workload_id == "W09"
        else _partition(command_count, instrument_count)
    )
    active_targets = (
        _skewed_partition(active_order_target, instrument_count)
        if workload_id == "W09"
        else (active_order_target,) * instrument_count
    )
    streams = tuple(
        _customize_spec(
            workload_id,
            resolve_workload_spec(
                _PROFILE_BY_ID[workload_id],
                command_count=count,
                instrument_id=index + 1,
                engine=MatchingConfig(
                    max_order_quantity=1_000_000,
                    tick_increment=1,
                    max_active_orders=max(128, active_targets[index] * 2),
                ),
                invalid_basis_points=0,
                active_order_target=active_targets[index],
                snapshot_interval=max(1, count),
            ),
        )
        for index, count in enumerate(counts)
    )
    return MultiWorkloadSpec(
        streams,
        MultiInstrumentEngineConfig(
            active_order_target * 2
            if workload_id == "W09"
            else max(128, active_order_target * 2) * instrument_count
        ),
    )


def _validate_region_shape(
    workload_id: str,
    *,
    preload_commands: int,
    warmup_commands: int,
    measured_commands: int,
    active_order_target: int,
    sweep_depth: int,
) -> None:
    if workload_id == "W10":
        if preload_commands != 0 or warmup_commands != 0:
            raise ValueError("W10 starts empty and permits only a measured replay-source region")
        if measured_commands % 4 != 0:
            raise ValueError("W10 measured commands must contain complete four-command cycles")
        return
    if preload_commands != active_order_target:
        raise ValueError(f"{workload_id} requires preload=active target")
    cycle_commands = {
        "W01": 2,
        "W02": 2,
        "W03": 2,
        "W04": 20,
        "W05": sweep_depth + 1,
        "W06": 2,
        "W07": 1,
        "W08": 2,
        "W09": 2,
        "W12": W12_CYCLE_COMMANDS,
    }[workload_id]
    if warmup_commands % cycle_commands != 0 or measured_commands % cycle_commands != 0:
        raise ValueError(
            f"{workload_id} warm/measured regions must contain complete "
            f"{cycle_commands}-command cycles"
        )


def materialize_workload(
    workload_id: str,
    output_directory: Path,
    *,
    seed: int,
    preload_commands: int,
    warmup_commands: int,
    measured_commands: int,
    instrument_count: int = 1,
    active_order_target: int = 64,
    sweep_depth: int = 16,
    log_materializer: Path | None = None,
    eager_invariant_checks: bool = False,
) -> tuple[Path, WorkloadManifest]:
    """Write one canonical ATLAS_DIFF_V2 stream and its resolved manifest.

    The default bulk path performs full invariant validation at initialization,
    after preload, at measured-region entry, and during final digest production.
    Set eager_invariant_checks=True to additionally validate after every
    command; generated artifact bytes are identical under both policies.
    """

    if not isinstance(eager_invariant_checks, bool):
        raise TypeError("eager_invariant_checks must be a bool")

    for name, value in (
        ("seed", seed),
        ("preload_commands", preload_commands),
        ("warmup_commands", warmup_commands),
        ("measured_commands", measured_commands),
    ):
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= U64_MAX:
            raise ValueError(f"{name} must be a u64")
    if measured_commands == 0:
        raise ValueError("measured_commands must be nonzero")
    _validate_region_shape(
        workload_id,
        preload_commands=preload_commands,
        warmup_commands=warmup_commands,
        measured_commands=measured_commands,
        active_order_target=active_order_target,
        sweep_depth=sweep_depth,
    )
    if workload_id == "W10":
        if log_materializer is None:
            raise ValueError("W10 requires a native ATLSLG01 log materializer")
        _require_log_materializer(log_materializer)
    elif log_materializer is not None:
        raise ValueError("only W10 accepts a native log materializer")
    command_count = preload_commands + warmup_commands + measured_commands
    if command_count > MAX_BENCHMARK_COMMANDS:
        raise ValueError(f"total command count exceeds the {MAX_BENCHMARK_COMMANDS} command limit")

    spec = build_workload_spec(
        workload_id,
        command_count=command_count,
        instrument_count=instrument_count,
        active_order_target=active_order_target,
        sweep_depth=sweep_depth,
    )
    output_directory.mkdir(parents=True, exist_ok=True)
    stem = _artifact_stem(
        workload_id=workload_id,
        seed=seed,
        preload_commands=preload_commands,
        warmup_commands=warmup_commands,
        measured_commands=measured_commands,
        instrument_count=instrument_count,
        active_order_target=active_order_target,
        sweep_depth=sweep_depth,
    )
    stream_path = output_directory / f"{stem}.atlas"
    manifest_path = output_directory / f"{stem}.json"
    log_path = output_directory / f"{stem}.atlslg" if workload_id == "W10" else None
    final_paths = (stream_path, manifest_path) + (() if log_path is None else (log_path,))
    existing = tuple(path for path in final_paths if path.exists())
    if existing:
        names = ", ".join(path.name for path in existing)
        raise FileExistsError(f"workload materialization never overwrites: {names}")

    commands = _iter_benchmark_commands(
        workload_id,
        spec,
        seed,
        active_order_target=active_order_target,
        sweep_depth=sweep_depth,
        preload_commands=preload_commands,
        warmup_commands=warmup_commands,
        measured_commands=measured_commands,
    )
    router = ReferenceRouter(
        spec.catalog,
        spec.engine,
        check_invariants=eager_invariant_checks,
    )
    native_input = MultiNativeInput(spec.catalog, spec.engine)
    expected_empty_state_digest = router.state_digest()
    preload_event_hasher = hashlib.sha256()
    event_hasher = hashlib.sha256()
    expected_preload_events = 0
    preload_committed = 0
    preload_rejected = 0
    preload_engine_errors = 0
    expected_events = 0
    committed = 0
    rejected = 0
    engine_errors = 0
    measured_operations: dict[str, int] = {}
    actual_instrument_command_counts = {entry.instrument_id: 0 for entry in native_input.catalog}
    after_preload_active_order_count = router.active_order_count if preload_commands == 0 else 0
    measured_start_active_order_count = (
        router.active_order_count if preload_commands + warmup_commands == 0 else 0
    )
    expected_preload_state_digest = expected_empty_state_digest if preload_commands == 0 else ""
    emitted_commands = 0

    temporary_stream = _new_temporary_path(output_directory, stream_path.name)
    temporary_manifest = _new_temporary_path(output_directory, manifest_path.name)
    temporary_log = (
        _new_temporary_path(output_directory, log_path.name) if log_path is not None else None
    )
    staged_paths = [temporary_stream, temporary_manifest]
    if temporary_log is not None:
        staged_paths.append(temporary_log)
    try:
        with temporary_stream.open("xb") as output:
            _write_line(output, encode_multi_header(native_input, command_count))
            for entry in native_input.catalog:
                _write_line(output, encode_multi_instrument(entry))
            for command_index, command in enumerate(commands):
                if command_index >= command_count:
                    raise AssertionError("benchmark composer emitted too many commands")
                emitted_commands = command_index + 1
                _write_line(output, encode_multi_command(command))
                actual_instrument_command_counts[command.instrument_id] += 1
                result = router.execute(command)
                completed_commands = command_index + 1
                if completed_commands == preload_commands:
                    after_preload_active_order_count = router.active_order_count
                    expected_preload_state_digest = router.state_digest()
                if completed_commands == preload_commands + warmup_commands:
                    router.assert_invariants()
                    measured_start_active_order_count = router.active_order_count
                if command_index < preload_commands:
                    if result.error is not None:
                        preload_engine_errors += 1
                        continue
                    batch = result.batch
                    if batch is None:  # pragma: no cover - protected by ReferenceResult
                        raise AssertionError("reference result has neither batch nor error")
                    expected_preload_events += len(batch.events)
                    preload_event_hasher.update(bytes.fromhex(event_digest(batch)))
                    if result.committed:
                        preload_committed += 1
                    else:
                        preload_rejected += 1
                    continue
                if command_index < preload_commands + warmup_commands:
                    continue
                operation = _operation_category(workload_id, command)
                measured_operations[operation] = measured_operations.get(operation, 0) + 1
                if result.error is not None:
                    engine_errors += 1
                    continue
                batch = result.batch
                if batch is None:  # pragma: no cover - protected by ReferenceResult
                    raise AssertionError("reference result has neither batch nor error")
                expected_events += len(batch.events)
                event_hasher.update(bytes.fromhex(event_digest(batch)))
                if result.committed:
                    committed += 1
                else:
                    rejected += 1
            if emitted_commands != command_count:
                raise AssertionError(
                    f"benchmark composer emitted {emitted_commands} of {command_count} commands"
                )
            output.flush()
            os.fsync(output.fileno())
        stream_sha256 = file_sha256(temporary_stream)
        timed_input_file: str | None = None
        timed_input_kind = "none"
        timed_input_sha256: str | None = None
        timed_input_records = 0
        cache_policy = "none"
        expected_final_digest = router.state_digest()
        expected_event_digest = event_hasher.hexdigest()
        if workload_id == "W10":
            if log_materializer is None:  # pragma: no cover - validated above
                raise AssertionError("W10 log materializer disappeared")
            if log_path is None or temporary_log is None:  # pragma: no cover
                raise AssertionError("W10 replay path disappeared")
            timed_input_sha256 = _materialize_replay_log(
                log_materializer,
                workload=temporary_stream,
                workload_sha256=stream_sha256,
                output=temporary_log,
                expected_records=measured_commands,
                expected_events=expected_events,
                expected_committed=committed,
                expected_rejected=rejected,
                expected_event_digest=expected_event_digest,
                expected_final_digest=expected_final_digest,
            )
            timed_input_file = log_path.name
            timed_input_kind = "atlslg01"
            timed_input_records = measured_commands
            cache_policy = "warm_page_cache"

        manifest = WorkloadManifest(
            workload_id=workload_id,
            generator_version=BENCHMARK_GENERATOR_VERSION,
            seed=seed,
            catalog=tuple(
                CatalogEntry(
                    entry.instrument_id,
                    entry.matching.max_order_quantity,
                    entry.matching.tick_increment,
                    entry.matching.max_active_orders,
                )
                for entry in spec.catalog
            ),
            max_total_active_orders=spec.engine.max_total_active_orders,
            operation_distribution=tuple(sorted(measured_operations.items())),
            parameters=_resolved_parameters(
                workload_id,
                spec,
                active_order_target=active_order_target,
                instrument_count=instrument_count,
                sweep_depth=sweep_depth,
                preload_commands=preload_commands,
                warmup_commands=warmup_commands,
                measured_commands=measured_commands,
                after_preload_active_order_count=after_preload_active_order_count,
                measured_start_active_order_count=measured_start_active_order_count,
                actual_instrument_command_counts=tuple(
                    actual_instrument_command_counts[entry.instrument_id]
                    for entry in native_input.catalog
                ),
            ),
            preload_commands=preload_commands,
            warmup_commands=warmup_commands,
            measured_commands=measured_commands,
            after_preload_active_order_count=after_preload_active_order_count,
            measured_start_active_order_count=measured_start_active_order_count,
            final_active_order_count=router.active_order_count,
            stream_file=stream_path.name,
            stream_sha256=stream_sha256,
            expected_events=expected_events,
            expected_committed=committed,
            expected_rejected=rejected,
            expected_engine_errors=engine_errors,
            expected_event_digest=expected_event_digest,
            expected_final_digest=expected_final_digest,
            expected_empty_state_digest=expected_empty_state_digest,
            expected_preload_events=expected_preload_events,
            expected_preload_committed=preload_committed,
            expected_preload_rejected=preload_rejected,
            expected_preload_engine_errors=preload_engine_errors,
            expected_preload_event_digest=preload_event_hasher.hexdigest(),
            expected_preload_state_digest=expected_preload_state_digest,
            timed_input_file=timed_input_file,
            timed_input_kind=timed_input_kind,
            timed_input_sha256=timed_input_sha256,
            timed_input_records=timed_input_records,
            cache_policy=cache_policy,
        )
        _write_staged_bytes(
            temporary_manifest,
            canonical_json_bytes(workload_to_dict(manifest)),
        )
        publications = [(temporary_stream, stream_path)]
        if temporary_log is not None and log_path is not None:
            publications.append((temporary_log, log_path))
        # The manifest is the discoverable commit marker and is published last,
        # only after every file it names is already visible.
        publications.append((temporary_manifest, manifest_path))
        _publish_staged_group(publications)
    except Exception:
        _remove_paths(staged_paths)
        raise
    return manifest_path, manifest


def verify_workload_manifest(
    path: Path,
    *,
    eager_invariant_checks: bool = True,
) -> WorkloadManifest:
    """Deeply reproduce a workload, with eager oracle checks by default."""

    if not isinstance(eager_invariant_checks, bool):
        raise TypeError("eager_invariant_checks must be a bool")
    manifest = read_canonical_document(path, workload_from_dict)
    if manifest.generator_version != BENCHMARK_GENERATOR_VERSION:
        raise ValueError("unsupported benchmark workload generator version")
    parameters = dict(manifest.parameters)
    instrument_count = _parameter_uint(parameters, "instrument_count")
    active_order_target = _parameter_uint(parameters, "active_order_target")
    encoded_sweep_depth = _parameter_uint(parameters, "sweep_depth")
    sweep_depth = 16 if encoded_sweep_depth == 0 else encoded_sweep_depth
    _validate_region_shape(
        manifest.workload_id,
        preload_commands=manifest.preload_commands,
        warmup_commands=manifest.warmup_commands,
        measured_commands=manifest.measured_commands,
        active_order_target=active_order_target,
        sweep_depth=sweep_depth,
    )
    spec = build_workload_spec(
        manifest.workload_id,
        command_count=manifest.command_count,
        instrument_count=instrument_count,
        active_order_target=active_order_target,
        sweep_depth=sweep_depth,
    )
    resolved_catalog = tuple(
        CatalogEntry(
            entry.instrument_id,
            entry.matching.max_order_quantity,
            entry.matching.tick_increment,
            entry.matching.max_active_orders,
        )
        for entry in spec.catalog
    )
    if (
        manifest.catalog != resolved_catalog
        or manifest.max_total_active_orders != spec.engine.max_total_active_orders
    ):
        raise ValueError("workload catalog differs from its resolved parameters")
    stream = path.parent / manifest.stream_file
    native_input = MultiNativeInput(
        tuple(_instrument_config(entry) for entry in manifest.catalog),
        MultiInstrumentEngineConfig(manifest.max_total_active_orders),
    )
    commands = _iter_benchmark_commands(
        manifest.workload_id,
        spec,
        manifest.seed,
        active_order_target=active_order_target,
        sweep_depth=sweep_depth,
        preload_commands=manifest.preload_commands,
        warmup_commands=manifest.warmup_commands,
        measured_commands=manifest.measured_commands,
    )
    router = ReferenceRouter(
        spec.catalog,
        spec.engine,
        check_invariants=eager_invariant_checks,
    )
    expected_empty_state_digest = router.state_digest()
    preload_event_hasher = hashlib.sha256()
    event_hasher = hashlib.sha256()
    preload_events = preload_committed = preload_rejected = preload_engine_errors = 0
    events = committed = rejected = engine_errors = 0
    measured_operations: dict[str, int] = {}
    stream_hasher = hashlib.sha256()
    actual_instrument_command_counts = {entry.instrument_id: 0 for entry in native_input.catalog}
    after_preload_active_order_count = (
        router.active_order_count if manifest.preload_commands == 0 else 0
    )
    measured_start_active_order_count = (
        router.active_order_count
        if manifest.preload_commands + manifest.warmup_commands == 0
        else 0
    )
    expected_preload_state_digest = (
        expected_empty_state_digest if manifest.preload_commands == 0 else ""
    )
    emitted_commands = 0
    try:
        with stream.open("rb") as source:
            _verify_stream_line(
                source,
                stream_hasher,
                encode_multi_header(native_input, manifest.command_count),
            )
            for entry in native_input.catalog:
                _verify_stream_line(
                    source,
                    stream_hasher,
                    encode_multi_instrument(entry),
                )
            for command_index, command in enumerate(commands):
                if command_index >= manifest.command_count:
                    raise ValueError("workload composer emitted too many commands")
                emitted_commands = command_index + 1
                _verify_stream_line(
                    source,
                    stream_hasher,
                    encode_multi_command(command),
                )
                actual_instrument_command_counts[command.instrument_id] += 1
                result = router.execute(command)
                completed_commands = command_index + 1
                if completed_commands == manifest.preload_commands:
                    after_preload_active_order_count = router.active_order_count
                    expected_preload_state_digest = router.state_digest()
                if completed_commands == manifest.preload_commands + manifest.warmup_commands:
                    router.assert_invariants()
                    measured_start_active_order_count = router.active_order_count
                if command_index < manifest.preload_commands:
                    if result.error is not None:
                        preload_engine_errors += 1
                        continue
                    batch = result.batch
                    if batch is None:  # pragma: no cover - protected by ReferenceResult
                        raise AssertionError("reference result has neither batch nor error")
                    preload_events += len(batch.events)
                    preload_event_hasher.update(bytes.fromhex(event_digest(batch)))
                    if result.committed:
                        preload_committed += 1
                    else:
                        preload_rejected += 1
                    continue
                if command_index < manifest.preload_commands + manifest.warmup_commands:
                    continue
                operation = _operation_category(manifest.workload_id, command)
                measured_operations[operation] = measured_operations.get(operation, 0) + 1
                if result.error is not None:
                    engine_errors += 1
                    continue
                batch = result.batch
                if batch is None:  # pragma: no cover - protected by ReferenceResult
                    raise AssertionError("reference result has neither batch nor error")
                events += len(batch.events)
                event_hasher.update(bytes.fromhex(event_digest(batch)))
                if result.committed:
                    committed += 1
                else:
                    rejected += 1
            if source.read(1):
                raise ValueError("workload stream contains trailing records or bytes")
    except OSError as exc:
        raise ValueError("workload stream is not readable") from exc
    if emitted_commands != manifest.command_count:
        raise ValueError("workload composer emitted fewer commands than declared")
    if manifest.parameters != _resolved_parameters(
        manifest.workload_id,
        spec,
        active_order_target=active_order_target,
        instrument_count=instrument_count,
        sweep_depth=sweep_depth,
        preload_commands=manifest.preload_commands,
        warmup_commands=manifest.warmup_commands,
        measured_commands=manifest.measured_commands,
        after_preload_active_order_count=after_preload_active_order_count,
        measured_start_active_order_count=measured_start_active_order_count,
        actual_instrument_command_counts=tuple(
            actual_instrument_command_counts[entry.instrument_id] for entry in native_input.catalog
        ),
    ):
        raise ValueError("workload resolved parameters are not reproducible")
    _verify_timed_input(path.parent, manifest)
    if (
        stream_hasher.hexdigest() != manifest.stream_sha256
        or manifest.operation_distribution != tuple(sorted(measured_operations.items()))
        or manifest.after_preload_active_order_count != after_preload_active_order_count
        or manifest.measured_start_active_order_count != measured_start_active_order_count
        or manifest.final_active_order_count != router.active_order_count
        or manifest.expected_events != events
        or manifest.expected_committed != committed
        or manifest.expected_rejected != rejected
        or manifest.expected_engine_errors != engine_errors
        or manifest.expected_event_digest != event_hasher.hexdigest()
        or manifest.expected_final_digest != router.state_digest()
        or manifest.expected_empty_state_digest != expected_empty_state_digest
        or manifest.expected_preload_events != preload_events
        or manifest.expected_preload_committed != preload_committed
        or manifest.expected_preload_rejected != preload_rejected
        or manifest.expected_preload_engine_errors != preload_engine_errors
        or manifest.expected_preload_event_digest != preload_event_hasher.hexdigest()
        or manifest.expected_preload_state_digest != expected_preload_state_digest
    ):
        raise ValueError("workload manifest evidence does not reproduce")
    return manifest


def verify_workload_manifest_boundary_checked(path: Path) -> WorkloadManifest:
    """Deeply reproduce a workload with full checks at semantic boundaries."""

    return verify_workload_manifest(path, eager_invariant_checks=False)


def verify_campaign_workload(path: Path) -> VerifiedWorkload:
    """Deeply verify exact workload bytes for reuse within one campaign process."""

    manifest = verify_workload_manifest_boundary_checked(path)
    return _verified_workload_from_manifest(path, manifest)


def revalidate_verified_workload(workload: VerifiedWorkload) -> None:
    """Cheaply prove that a verified capability still names the same bytes."""

    manifest_path = workload.manifest_path
    if not manifest_path.is_absolute() or manifest_path.resolve(strict=True) != manifest_path:
        raise ValueError("verified workload manifest path is no longer exact")
    expected_stream = manifest_path.parent / workload.manifest.stream_file
    if workload.stream_path != expected_stream:
        raise ValueError("verified workload stream path differs from its manifest")
    if workload.stream_sha256 != workload.manifest.stream_sha256:
        raise ValueError("verified workload stream digest differs from its manifest")
    timed_name = workload.manifest.timed_input_file
    expected_timed = None if timed_name is None else manifest_path.parent / timed_name
    if workload.timed_input_path != expected_timed:
        raise ValueError("verified workload timed-input path differs from its manifest")
    if workload.timed_input_sha256 != workload.manifest.timed_input_sha256:
        raise ValueError("verified workload timed-input digest differs from its manifest")
    if file_sha256(manifest_path) != workload.manifest_sha256:
        raise ValueError("verified workload manifest bytes changed")
    if file_sha256(workload.stream_path) != workload.stream_sha256:
        raise ValueError("verified workload stream bytes changed")
    if workload.timed_input_path is not None:
        if file_sha256(workload.timed_input_path) != workload.timed_input_sha256:
            raise ValueError("verified workload timed-input bytes changed")


def _verified_workload_from_manifest(
    path: Path,
    manifest: WorkloadManifest,
) -> VerifiedWorkload:
    """Bind byte identity to a manifest already returned by a deep verifier."""

    manifest_path = path.resolve(strict=True)
    if read_canonical_document(manifest_path, workload_from_dict) != manifest:
        raise ValueError("workload manifest changed after semantic verification")
    stream_path = manifest_path.parent / manifest.stream_file
    timed_input_path = (
        None
        if manifest.timed_input_file is None
        else manifest_path.parent / manifest.timed_input_file
    )
    workload = VerifiedWorkload(
        manifest_path=manifest_path,
        manifest_sha256=file_sha256(manifest_path),
        manifest=manifest,
        stream_path=stream_path,
        stream_sha256=file_sha256(stream_path),
        timed_input_path=timed_input_path,
        timed_input_sha256=(None if timed_input_path is None else file_sha256(timed_input_path)),
    )
    revalidate_verified_workload(workload)
    return workload


def _customize_spec(workload_id: str, spec: WorkloadSpec) -> WorkloadSpec:
    aggressive = spec.aggressive_basis_points
    market = spec.market_basis_points
    price_span = spec.price_span_ticks
    if workload_id in {"W01", "W02", "W03", "W06"}:
        aggressive = 0
        market = 0
    elif workload_id == "W04":
        aggressive = 1_818
        market = 0
    elif workload_id == "W05":
        aggressive = 8_000
        market = 6_000
    elif workload_id == "W08":
        aggressive = 10_000
        market = 5_000
    if workload_id == "W02":
        price_span = 4
    elif workload_id == "W03":
        price_span = 1_000_000
    return replace(
        spec,
        operation_weights=_WEIGHTS_BY_ID[workload_id],
        aggressive_basis_points=aggressive,
        market_basis_points=market,
        price_span_ticks=price_span,
    )


def _partition(total: int, parts: int) -> tuple[int, ...]:
    quotient, remainder = divmod(total, parts)
    return tuple(quotient + int(index < remainder) for index in range(parts))


def _skewed_partition(total: int, parts: int) -> tuple[int, ...]:
    if parts == 1:
        return (total,)
    if total < parts:
        raise ValueError("skewed workload needs at least one command per instrument")
    primary = max(1, total * 7 // 10)
    primary = min(primary, total - (parts - 1))
    tail = _partition(total - primary, parts - 1)
    return (primary, *tail)


def _iter_benchmark_commands(
    workload_id: str,
    spec: MultiWorkloadSpec,
    seed: int,
    *,
    active_order_target: int,
    sweep_depth: int,
    preload_commands: int,
    warmup_commands: int,
    measured_commands: int,
) -> Iterator[Command]:
    if workload_id == "W01":
        return _iter_w01(
            active_order_target,
            warmup_commands + measured_commands,
            seed,
        )
    if workload_id == "W02":
        return _iter_w02(
            active_order_target,
            warmup_commands + measured_commands,
            seed,
        )
    if workload_id == "W03":
        return _iter_unique_level_churn(
            active_order_target,
            warmup_commands + measured_commands,
            seed,
            sparse=True,
        )
    if workload_id == "W05":
        return _iter_w05(
            active_order_target,
            warmup_commands + measured_commands,
            sweep_depth,
            seed,
        )
    if workload_id == "W04":
        return _iter_w04(
            active_order_target,
            warmup_commands + measured_commands,
            seed,
        )
    if workload_id == "W06":
        return _iter_unique_level_churn(
            active_order_target,
            warmup_commands + measured_commands,
            seed,
            sparse=False,
        )
    if workload_id == "W07":
        return _iter_w07(
            active_order_target,
            warmup_commands + measured_commands,
            seed,
        )
    if workload_id == "W08":
        return _iter_w08(
            active_order_target,
            warmup_commands + measured_commands,
            seed,
        )
    if workload_id == "W09":
        return _iter_w09(
            spec,
            active_order_target,
            warmup_commands + measured_commands,
            seed,
        )
    if workload_id == "W10":
        return _iter_w10(measured_commands, seed)
    if workload_id == "W12":
        return _iter_w12(
            active_order_target,
            warmup_commands + measured_commands,
            seed,
        )
    raise AssertionError(f"missing benchmark composer for {workload_id}")


def _iter_w01(
    active_target: int,
    post_preload_commands: int,
    seed: int,
) -> Iterator[Command]:
    levels = 64
    offset = SplitMix64(seed).randbelow(levels)
    active: deque[tuple[int, int]] = deque()
    next_order_id = 1
    for _index in range(active_target):
        side = Side.BUY if next_order_id % 2 else Side.SELL
        level = (next_order_id + offset) % levels
        price = 10_000 - level if side == Side.BUY else 20_000 + level
        client = 1 + next_order_id % 16
        active.append((next_order_id, client))
        yield NewOrder(
            client_id=client,
            order_id=next_order_id,
            instrument_id=1,
            side=side,
            order_type=OrderType.LIMIT,
            time_in_force=TimeInForce.GTC,
            limit_price=price,
            quantity=1,
        )
        next_order_id += 1
    for _ in range(post_preload_commands // 2):
        order_id, client = active.popleft()
        yield CancelOrder(client, order_id, 1)
        side = Side.BUY if next_order_id % 2 else Side.SELL
        level = (next_order_id + offset) % levels
        price = 10_000 - level if side == Side.BUY else 20_000 + level
        client = 1 + next_order_id % 16
        active.append((next_order_id, client))
        yield NewOrder(
            client_id=client,
            order_id=next_order_id,
            instrument_id=1,
            side=side,
            order_type=OrderType.LIMIT,
            time_in_force=TimeInForce.GTC,
            limit_price=price,
            quantity=1,
        )
        next_order_id += 1


def _iter_w02(
    active_target: int,
    post_preload_commands: int,
    seed: int,
) -> Iterator[Command]:
    active: tuple[deque[tuple[int, int]], ...] = tuple(deque() for _ in range(8))
    offset = SplitMix64(seed).randbelow(8)
    next_order_id = 1
    for index in range(active_target):
        level = (index + offset) % 8
        queue = active[level]
        side = Side.BUY if level < 4 else Side.SELL
        price = 10_000 - level if side == Side.BUY else 20_000 + (level - 4)
        client = 1 + level
        queue.append((next_order_id, client))
        yield NewOrder(
            client,
            next_order_id,
            1,
            side,
            OrderType.LIMIT,
            TimeInForce.GTC,
            price,
            1,
        )
        next_order_id += 1
    for churn_level in range(post_preload_commands // 2):
        level = (churn_level + offset) % 8
        order_id, client = active[level].popleft()
        yield CancelOrder(client, order_id, 1)
        side = Side.BUY if level < 4 else Side.SELL
        price = 10_000 - level if side == Side.BUY else 20_000 + (level - 4)
        active[level].append((next_order_id, client))
        yield NewOrder(
            client,
            next_order_id,
            1,
            side,
            OrderType.LIMIT,
            TimeInForce.GTC,
            price,
            1,
        )
        next_order_id += 1


def _iter_unique_level_churn(
    active_target: int,
    post_preload_commands: int,
    seed: int,
    *,
    sparse: bool,
) -> Iterator[Command]:
    """Keep one order per level while cancel/add pairs replace the level itself."""

    center = 5_000_000 + SplitMix64(seed).randbelow(1_000)
    spacing = 4_096 if sparse else 1
    active: deque[tuple[int, int, Side]] = deque()
    side_ordinals = {Side.BUY: 0, Side.SELL: 0}
    next_order_id = 1

    def next_price(side: Side) -> int:
        ordinal = side_ordinals[side]
        side_ordinals[side] += 1
        distance = 1_000_000 + ordinal * spacing
        return center - distance if side == Side.BUY else center + distance

    for index in range(active_target):
        side = Side.BUY if index % 2 == 0 else Side.SELL
        client = 1 + index % 16
        price = next_price(side)
        active.append((next_order_id, client, side))
        yield NewOrder(
            client,
            next_order_id,
            1,
            side,
            OrderType.LIMIT,
            TimeInForce.GTC,
            price,
            1,
        )
        next_order_id += 1
    for _ in range(post_preload_commands // 2):
        old_order_id, client, side = active.popleft()
        yield CancelOrder(client, old_order_id, 1)
        price = next_price(side)
        active.append((next_order_id, client, side))
        yield NewOrder(
            client,
            next_order_id,
            1,
            side,
            OrderType.LIMIT,
            TimeInForce.GTC,
            price,
            1,
        )
        next_order_id += 1


def _iter_w05(
    active_target: int,
    post_preload_commands: int,
    depth: int,
    seed: int,
) -> Iterator[Command]:
    next_order_id = 1
    base_price = 10_000 + SplitMix64(seed).randbelow(1_000)
    for index in range(active_target):
        yield NewOrder(
            1 + index % 16,
            next_order_id,
            1,
            Side.BUY,
            OrderType.LIMIT,
            TimeInForce.GTC,
            1_000 - index % 64,
            1,
        )
        next_order_id += 1
    for _ in range(post_preload_commands // (depth + 1)):
        for level in range(depth):
            yield NewOrder(
                1,
                next_order_id,
                1,
                Side.SELL,
                OrderType.LIMIT,
                TimeInForce.GTC,
                base_price + level,
                1,
            )
            next_order_id += 1
        yield NewOrder(
            2,
            next_order_id,
            1,
            Side.BUY,
            OrderType.MARKET,
            TimeInForce.IOC,
            None,
            depth,
        )
        next_order_id += 1


def _iter_w04(
    active_target: int,
    post_preload_commands: int,
    seed: int,
) -> Iterator[Command]:
    """Emit exact 45/35/10/10 add/cancel/replace/market cycles."""

    next_order_id = 1
    base_price = 10_000 + SplitMix64(seed).randbelow(1_000)
    active_buys: deque[tuple[int, int]] = deque()
    active_sells: deque[tuple[int, int]] = deque()
    for index in range(active_target):
        client = 1 + next_order_id % 16
        side = Side.BUY if index % 2 == 0 else Side.SELL
        active = active_buys if side == Side.BUY else active_sells
        active.append((next_order_id, client))
        yield NewOrder(
            client,
            next_order_id,
            1,
            side,
            OrderType.LIMIT,
            TimeInForce.GTC,
            base_price - 100 if side == Side.BUY else base_price + 100,
            1,
        )
        next_order_id += 1
    for _ in range(post_preload_commands // 20):
        for side, count in ((Side.BUY, 5), (Side.SELL, 4)):
            active = active_buys if side == Side.BUY else active_sells
            price = base_price - 100 if side == Side.BUY else base_price + 100
            for _ in range(count):
                client = 1 + next_order_id % 16
                active.append((next_order_id, client))
                yield NewOrder(
                    client,
                    next_order_id,
                    1,
                    side,
                    OrderType.LIMIT,
                    TimeInForce.GTC,
                    price,
                    1,
                )
                next_order_id += 1
        for active, count in ((active_buys, 4), (active_sells, 3)):
            for _ in range(count):
                order_id, client = active.popleft()
                yield CancelOrder(client, order_id, 1)
        for side, active in ((Side.BUY, active_buys), (Side.SELL, active_sells)):
            old_order_id, client = active.popleft()
            active.append((next_order_id, client))
            yield ReplaceOrder(
                client,
                old_order_id,
                next_order_id,
                1,
                base_price - 100 if side == Side.BUY else base_price + 100,
                1,
            )
            next_order_id += 1
        active_sells.popleft()
        yield NewOrder(
            1,
            next_order_id,
            1,
            Side.BUY,
            OrderType.MARKET,
            TimeInForce.IOC,
            None,
            1,
        )
        next_order_id += 1
        active_buys.popleft()
        yield NewOrder(
            1,
            next_order_id,
            1,
            Side.SELL,
            OrderType.MARKET,
            TimeInForce.IOC,
            None,
            1,
        )
        next_order_id += 1


def _iter_w07(
    active_target: int,
    post_preload_commands: int,
    seed: int,
) -> Iterator[Command]:
    """Move every selected order to a new price and a new priority sequence."""

    center = 5_000_000 + SplitMix64(seed).randbelow(1_000)
    active: deque[tuple[int, int, Side]] = deque()
    side_ordinals = {Side.BUY: 0, Side.SELL: 0}
    next_order_id = 1

    def next_price(side: Side) -> int:
        ordinal = side_ordinals[side]
        side_ordinals[side] += 1
        distance = 1_000_000 + ordinal
        return center - distance if side == Side.BUY else center + distance

    for index in range(active_target):
        side = Side.BUY if index % 2 == 0 else Side.SELL
        client = 1 + index % 16
        price = next_price(side)
        active.append((next_order_id, client, side))
        yield NewOrder(
            client,
            next_order_id,
            1,
            side,
            OrderType.LIMIT,
            TimeInForce.GTC,
            price,
            1,
        )
        next_order_id += 1
    for _ in range(post_preload_commands):
        old_order_id, client, side = active.popleft()
        active.append((next_order_id, client, side))
        yield ReplaceOrder(
            client,
            old_order_id,
            next_order_id,
            1,
            next_price(side),
            1,
        )
        next_order_id += 1


def _iter_w08(
    active_target: int,
    post_preload_commands: int,
    seed: int,
) -> Iterator[Command]:
    """Exercise one-lot IOC fills and non-resting residuals over protected state."""

    center = 5_000_000 + SplitMix64(seed).randbelow(1_000)
    next_order_id = 1
    for index in range(active_target):
        side = Side.BUY if index % 2 == 0 else Side.SELL
        ordinal = index // 2
        price = 1_000_000 - ordinal if side == Side.BUY else 9_000_000 + ordinal
        yield NewOrder(
            1 + index % 16,
            next_order_id,
            1,
            side,
            OrderType.LIMIT,
            TimeInForce.GTC,
            price,
            1,
        )
        next_order_id += 1
    buy_cycle_first = SplitMix64(seed).randbelow(2) == 0
    for cycle in range(post_preload_commands // 2):
        aggressor_side = Side.BUY if (cycle % 2 == 0) == buy_cycle_first else Side.SELL
        passive_side = Side.SELL if aggressor_side == Side.BUY else Side.BUY
        price = center + 1 if passive_side == Side.SELL else center - 1
        yield NewOrder(
            101,
            next_order_id,
            1,
            passive_side,
            OrderType.LIMIT,
            TimeInForce.GTC,
            price,
            1,
        )
        next_order_id += 1
        yield NewOrder(
            102,
            next_order_id,
            1,
            aggressor_side,
            OrderType.LIMIT,
            TimeInForce.IOC,
            price,
            2,
        )
        next_order_id += 1


def _iter_w09(
    spec: MultiWorkloadSpec,
    active_target: int,
    post_preload_commands: int,
    seed: int,
) -> Iterator[Command]:
    instrument_count = len(spec.streams)
    active_counts = _skewed_partition(active_target, instrument_count)
    cumulative_active_counts: list[int] = []
    cumulative = 0
    for count in active_counts:
        cumulative += count
        cumulative_active_counts.append(cumulative)
    active: tuple[deque[tuple[int, int]], ...] = tuple(deque() for _ in range(instrument_count))
    next_order_id = 1
    for index, count in enumerate(active_counts):
        instrument_id = spec.streams[index].instrument_id
        for local_index in range(count):
            client = 1 + local_index % 16
            active[index].append((next_order_id, client))
            yield NewOrder(
                client,
                next_order_id,
                instrument_id,
                Side.BUY,
                OrderType.LIMIT,
                TimeInForce.GTC,
                10_000 - local_index % 64,
                1,
            )
            next_order_id += 1
    rng = SplitMix64(seed)
    for _ in range(post_preload_commands // 2):
        selected = rng.randbelow(active_target)
        instrument_index = bisect_right(cumulative_active_counts, selected)
        instrument_id = spec.streams[instrument_index].instrument_id
        old_order_id, client = active[instrument_index].popleft()
        yield CancelOrder(client, old_order_id, instrument_id)
        active[instrument_index].append((next_order_id, client))
        yield NewOrder(
            client,
            next_order_id,
            instrument_id,
            Side.BUY,
            OrderType.LIMIT,
            TimeInForce.GTC,
            10_000 - next_order_id % 64,
            1,
        )
        next_order_id += 1


def _iter_w10(measured_commands: int, seed: int) -> Iterator[Command]:
    """Generate a bounded legal replay source from an empty engine."""

    center = 5_000_000 + SplitMix64(seed).randbelow(1_000)
    next_order_id = 1
    for _ in range(measured_commands // 4):
        client = 1 + next_order_id % 16
        yield NewOrder(
            client,
            next_order_id,
            1,
            Side.BUY,
            OrderType.LIMIT,
            TimeInForce.GTC,
            center - 100,
            1,
        )
        yield CancelOrder(client, next_order_id, 1)
        next_order_id += 1
        yield NewOrder(
            client,
            next_order_id,
            1,
            Side.SELL,
            OrderType.LIMIT,
            TimeInForce.GTC,
            center + 100,
            1,
        )
        next_order_id += 1
        yield NewOrder(
            client,
            next_order_id,
            1,
            Side.BUY,
            OrderType.LIMIT,
            TimeInForce.IOC,
            center + 100,
            1,
        )
        next_order_id += 1


def _iter_w12(
    active_target: int,
    post_preload_commands: int,
    seed: int,
) -> Iterator[Command]:
    """Emit exact 100-command adversarial cycles over protected background state."""

    center = 5_000_000 + SplitMix64(seed).randbelow(1_000)
    next_order_id = 1
    for index in range(active_target):
        side = Side.BUY if index % 2 == 0 else Side.SELL
        ordinal = index // 2
        price = 1_000_000 - ordinal if side == Side.BUY else 9_000_000 + ordinal
        yield NewOrder(
            1 + index % 16,
            next_order_id,
            1,
            side,
            OrderType.LIMIT,
            TimeInForce.GTC,
            price,
            1,
        )
        next_order_id += 1

    buy_sweep_first = SplitMix64(seed).randbelow(2) == 0
    for cycle in range(post_preload_commands // W12_CYCLE_COMMANDS):
        aggressor_side = Side.BUY if (cycle % 2 == 0) == buy_sweep_first else Side.SELL
        passive_side = Side.SELL if aggressor_side == Side.BUY else Side.BUY
        for level in range(64):
            price = center + 100 + level if passive_side == Side.SELL else center - 100 - level
            yield NewOrder(
                201,
                next_order_id,
                1,
                passive_side,
                OrderType.LIMIT,
                TimeInForce.GTC,
                price,
                1,
            )
            next_order_id += 1
        worst_inserted_price = center + 163 if aggressor_side == Side.BUY else center - 163
        yield NewOrder(
            202,
            next_order_id,
            1,
            aggressor_side,
            OrderType.LIMIT,
            TimeInForce.IOC,
            worst_inserted_price,
            64,
        )
        next_order_id += 1

        priority_price = center - 500
        old_order_id = next_order_id
        yield NewOrder(
            203,
            old_order_id,
            1,
            Side.BUY,
            OrderType.LIMIT,
            TimeInForce.GTC,
            priority_price,
            1,
        )
        next_order_id += 1
        second_order_id = next_order_id
        yield NewOrder(
            204,
            second_order_id,
            1,
            Side.BUY,
            OrderType.LIMIT,
            TimeInForce.GTC,
            priority_price,
            1,
        )
        next_order_id += 1
        replacement_order_id = next_order_id
        yield ReplaceOrder(
            203,
            old_order_id,
            replacement_order_id,
            1,
            priority_price,
            1,
        )
        next_order_id += 1
        yield CancelOrder(204, second_order_id, 1)
        yield CancelOrder(203, replacement_order_id, 1)

        reusable_order_id = next_order_id
        yield NewOrder(
            205,
            reusable_order_id,
            1,
            Side.BUY,
            OrderType.LIMIT,
            TimeInForce.IOC,
            center + 1_000,
            1_000_000,
        )
        yield NewOrder(
            205,
            reusable_order_id,
            1,
            Side.SELL,
            OrderType.LIMIT,
            TimeInForce.IOC,
            center - 1_000,
            1_000_000,
        )
        next_order_id += 1

        for churn_index in range(14):
            side = Side.BUY if churn_index % 2 == 0 else Side.SELL
            price = (
                center - 2_000 - churn_index if side == Side.BUY else center + 2_000 + churn_index
            )
            client = 206 + churn_index
            churn_order_id = next_order_id
            yield NewOrder(
                client,
                churn_order_id,
                1,
                side,
                OrderType.LIMIT,
                TimeInForce.GTC,
                price,
                1,
            )
            yield CancelOrder(client, churn_order_id, 1)
            next_order_id += 1


def _resolved_parameters(
    workload_id: str,
    spec: MultiWorkloadSpec,
    *,
    active_order_target: int,
    instrument_count: int,
    sweep_depth: int,
    preload_commands: int,
    warmup_commands: int,
    measured_commands: int,
    after_preload_active_order_count: int,
    measured_start_active_order_count: int,
    actual_instrument_command_counts: tuple[int, ...],
) -> tuple[tuple[str, str], ...]:
    first = spec.streams[0]
    return tuple(
        sorted(
            {
                "active_order_target": str(active_order_target),
                "aggressive_basis_points": str(first.aggressive_basis_points),
                "boundary_quantity_basis_points": str(first.boundary_quantity_basis_points),
                "client_count": str(first.client_count),
                "composition": "benchmark_specific",
                "hot_level_count": "8" if workload_id == "W02" else "0",
                "instrument_count": str(instrument_count),
                "invalid_basis_points": str(first.invalid_basis_points),
                "latency_first_sample_index": "31",
                "latency_sample_stride": "32",
                "market_basis_points": str(first.market_basis_points),
                "mid_price": str(first.mid_price),
                "operation_weights": (
                    f"{first.operation_weights.new},{first.operation_weights.cancel},"
                    f"{first.operation_weights.replace}"
                ),
                "price_model": first.price_model.value,
                "price_span_ticks": str(first.price_span_ticks),
                "profile": first.profile.value,
                "preload_commands": str(preload_commands),
                "after_preload_active_order_count": str(after_preload_active_order_count),
                "measured_start_active_order_count": str(measured_start_active_order_count),
                "stream_command_budgets": ",".join(
                    str(stream.command_count) for stream in spec.streams
                ),
                "actual_instrument_command_counts": ",".join(
                    str(count) for count in actual_instrument_command_counts
                ),
                "sweep_depth": str(sweep_depth) if workload_id == "W05" else "0",
                "cycle_commands": (
                    str(W12_CYCLE_COMMANDS)
                    if workload_id == "W12"
                    else (
                        str(sweep_depth + 1)
                        if workload_id == "W05"
                        else (
                            "20"
                            if workload_id == "W04"
                            else (
                                "4"
                                if workload_id == "W10"
                                else ("1" if workload_id == "W07" else "2")
                            )
                        )
                    )
                ),
                "underlying_multi_generator_version": str(MULTI_GENERATOR_VERSION),
                "warmup_commands": str(warmup_commands),
                "measured_commands": str(measured_commands),
                "measured_sweep_count": (
                    str(measured_commands // (sweep_depth + 1)) if workload_id == "W05" else "0"
                ),
                "w09_primary_activity_basis_points": ("7000" if workload_id == "W09" else "0"),
                "w09_active_order_counts": (
                    ",".join(str(stream.active_order_target) for stream in spec.streams)
                    if workload_id == "W09"
                    else "0"
                ),
            }.items()
        )
    )


def _parameter_uint(parameters: dict[str, str], name: str) -> int:
    try:
        text = parameters[name]
    except KeyError as exc:
        raise ValueError(f"workload parameters omit {name}") from exc
    if not text.isdecimal() or (len(text) > 1 and text.startswith("0")):
        raise ValueError(f"workload parameter {name} is not canonical decimal")
    return int(text)


def _operation_category(workload_id: str, command: Command) -> str:
    if workload_id == "W04":
        if isinstance(command, CancelOrder):
            return "cancel"
        if isinstance(command, ReplaceOrder):
            return "replace"
        if command.order_type == OrderType.MARKET:
            return "marketable_flow"
        return "nonmarketable_add"
    if workload_id == "W05" and isinstance(command, NewOrder):
        return "crossing_sweep" if command.order_type == OrderType.MARKET else "sweep_level_insert"
    if isinstance(command, NewOrder):
        return "new"
    if isinstance(command, CancelOrder):
        return "cancel"
    return "replace"


def _verify_stream_line(
    source: BinaryIO,
    hasher: _ByteHasher,
    expected: str,
) -> None:
    raw = source.readline(MAX_BENCHMARK_STREAM_LINE_BYTES + 1)
    if not raw:
        raise ValueError("workload stream ended before its declared record count")
    if len(raw) > MAX_BENCHMARK_STREAM_LINE_BYTES:
        raise ValueError("workload stream record exceeds the 1024-byte limit")
    hasher.update(raw)
    if not raw.endswith(b"\n") or b"\r" in raw:
        raise ValueError("workload stream records must be LF terminated")
    try:
        actual = raw[:-1].decode("ascii", errors="strict")
    except UnicodeDecodeError as exc:
        raise ValueError("workload stream is not canonical ASCII") from exc
    if actual != expected:
        raise ValueError("workload stream record differs from its resolved composition")


def _write_line(output: BinaryIO, line: str) -> None:
    encoded = (line + "\n").encode("ascii")
    if len(encoded) > MAX_BENCHMARK_STREAM_LINE_BYTES:
        raise ValueError("workload stream record exceeds the 1024-byte limit")
    output.write(encoded)


def _new_temporary_path(directory: Path, final_name: str) -> Path:
    for _ in range(100):
        candidate = directory / f".{final_name}.{secrets.token_hex(8)}.tmp"
        if not candidate.exists():
            return candidate
    raise OSError("could not select a unique same-directory temporary path")


def _write_staged_bytes(destination: Path, payload: bytes) -> None:
    with destination.open("xb") as output:
        output.write(payload)
        output.flush()
        os.fsync(output.fileno())


def _publish_staged_group(publications: list[tuple[Path, Path]]) -> None:
    """Publish a fully staged artifact group without replacing existing files."""

    destinations = tuple(destination for _, destination in publications)
    if len(destinations) != len(set(destinations)):
        raise ValueError("staged artifact group contains duplicate destinations")
    existing = tuple(path.name for path in destinations if path.exists())
    if existing:
        raise FileExistsError("workload materialization never overwrites: " + ", ".join(existing))

    published: list[Path] = []
    try:
        for temporary, destination in publications:
            os.link(temporary, destination)
            published.append(destination)
    except Exception as exc:
        try:
            _remove_paths(published)
        except RuntimeError as cleanup_error:
            raise RuntimeError(
                "staged publication failed and rollback was incomplete"
            ) from cleanup_error
        raise exc
    _discard_paths([temporary for temporary, _ in publications])


def _materialize_replay_log(
    executable: Path,
    *,
    workload: Path,
    workload_sha256: str,
    output: Path,
    expected_records: int,
    expected_events: int,
    expected_committed: int,
    expected_rejected: int,
    expected_event_digest: str,
    expected_final_digest: str,
) -> str:
    _require_log_materializer(executable)
    if output.exists():
        raise FileExistsError(f"replay log already exists: {output.name}")
    try:
        completed = subprocess.run(
            (
                str(executable),
                "--workload",
                str(workload),
                "--workload-sha256",
                workload_sha256,
                "--output",
                str(output),
            ),
            check=False,
            capture_output=True,
        )
    except OSError as exc:
        raise RuntimeError("native log materializer could not be started") from exc
    if completed.returncode != 0:
        detail = completed.stderr[:256].decode("ascii", errors="replace").strip()
        suffix = f": {detail}" if detail else ""
        raise RuntimeError(
            f"native log materializer failed with exit code {completed.returncode}{suffix}"
        )
    if completed.stderr:
        raise ValueError("native log materializer emitted unexpected stderr")
    receipt = _parse_log_materialization_receipt(completed.stdout)
    _require_log_magic(output)
    log_sha256 = file_sha256(output)
    expected = {
        "committed": str(expected_committed),
        "event_digest": expected_event_digest,
        "events": str(expected_events),
        "final_digest": expected_final_digest,
        "log_id": workload_sha256[:32],
        "log_sha256": log_sha256,
        "records": str(expected_records),
        "rejected": str(expected_rejected),
        "schema": LOG_MATERIALIZATION_SCHEMA,
        "workload_sha256": workload_sha256,
    }
    if receipt != expected:
        raise ValueError("native log materializer receipt differs from reference evidence")
    return log_sha256


def _verify_timed_input(directory: Path, manifest: WorkloadManifest) -> None:
    if manifest.workload_id != "W10":
        return
    if manifest.timed_input_file is None or manifest.timed_input_sha256 is None:
        raise ValueError("W10 replay input metadata is incomplete")
    timed_input = directory / manifest.timed_input_file
    _require_log_magic(timed_input)
    if file_sha256(timed_input) != manifest.timed_input_sha256:
        raise ValueError("W10 replay input digest differs from its manifest")


def _require_log_magic(path: Path) -> None:
    try:
        with path.open("rb") as source:
            magic = source.read(8)
    except OSError as exc:
        raise ValueError("W10 replay input is not readable") from exc
    if magic != b"ATLSLG01":
        raise ValueError("W10 replay input is not an ATLSLG01 command log")


def _parse_log_materialization_receipt(payload: bytes) -> dict[str, str]:
    if len(payload) > 4_096:
        raise ValueError("native log materializer receipt exceeds its input limit")
    try:
        text = payload.decode("ascii", errors="strict")
    except UnicodeDecodeError as exc:
        raise ValueError("native log materializer receipt must be ASCII") from exc
    if not text.endswith("\n") or "\r" in text or text.count("\n") != 1:
        raise ValueError("native log materializer must emit one LF-terminated receipt")
    try:
        raw = json.loads(
            text,
            object_pairs_hook=_unique_json_object,
            parse_int=_reject_json_number,
            parse_float=_reject_json_number,
            parse_constant=_reject_json_number,
        )
    except json.JSONDecodeError as exc:
        raise ValueError("native log materializer receipt is not valid JSON") from exc
    mapping = _plan_mapping(raw, "native log materializer receipt")
    _require_plan_keys(mapping, _LOG_RECEIPT_KEYS, "native log materializer receipt")
    if any(not isinstance(value, str) for value in mapping.values()):
        raise ValueError("native log materializer receipt values must be strings")
    receipt = {key: str(value) for key, value in mapping.items()}
    for name in ("committed", "events", "records", "rejected"):
        _plan_uint(receipt[name], name)
    for name in ("event_digest", "final_digest", "log_sha256", "workload_sha256"):
        value = receipt[name]
        if len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
            raise ValueError(f"native log materializer {name} is not a SHA-256 digest")
    log_id = receipt["log_id"]
    if len(log_id) != 32 or any(character not in "0123456789abcdef" for character in log_id):
        raise ValueError("native log materializer log_id is not canonical")
    if receipt["schema"] != LOG_MATERIALIZATION_SCHEMA:
        raise ValueError("unsupported native log materializer receipt schema")
    if payload != canonical_json_bytes(receipt):
        raise ValueError("native log materializer receipt is not canonical")
    return receipt


def _require_log_materializer(executable: Path) -> None:
    if not executable.is_file():
        raise ValueError("native log materializer must name an existing file")


def _artifact_stem(
    *,
    workload_id: str,
    seed: int,
    preload_commands: int,
    warmup_commands: int,
    measured_commands: int,
    instrument_count: int,
    active_order_target: int,
    sweep_depth: int,
) -> str:
    return (
        f"{workload_id.lower()}-g{BENCHMARK_GENERATOR_VERSION}-s{seed}"
        f"-p{preload_commands}-w{warmup_commands}-m{measured_commands}"
        f"-i{instrument_count}-a{active_order_target}-d{sweep_depth}"
    )


def _final_artifact_paths(
    output_directory: Path,
    *,
    workload_id: str,
    seed: int,
    preload_commands: int,
    warmup_commands: int,
    measured_commands: int,
    instrument_count: int,
    active_order_target: int,
    sweep_depth: int,
) -> tuple[Path, ...]:
    stem = _artifact_stem(
        workload_id=workload_id,
        seed=seed,
        preload_commands=preload_commands,
        warmup_commands=warmup_commands,
        measured_commands=measured_commands,
        instrument_count=instrument_count,
        active_order_target=active_order_target,
        sweep_depth=sweep_depth,
    )
    paths = (
        output_directory / f"{stem}.atlas",
        output_directory / f"{stem}.json",
    )
    if workload_id == "W10":
        return (*paths, output_directory / f"{stem}.atlslg")
    return paths


def _plan_point_shape_key(
    point: BenchmarkPlanPoint,
) -> tuple[str, int, int, int, int, int, int, int]:
    return (
        point.workload_id,
        point.seed,
        point.preload_commands,
        point.warmup_commands,
        point.measured_commands,
        point.instrument_count,
        point.active_order_target,
        point.sweep_depth,
    )


def _published_manifest_paths(
    manifest_path: Path,
    manifest: WorkloadManifest,
) -> tuple[Path, ...]:
    paths = [manifest_path, manifest_path.parent / manifest.stream_file]
    if manifest.timed_input_file is not None:
        paths.append(manifest_path.parent / manifest.timed_input_file)
    return tuple(paths)


def _remove_paths(paths: list[Path]) -> None:
    failures: list[str] = []
    for path in reversed(paths):
        try:
            path.unlink()
        except FileNotFoundError:
            continue
        except OSError:
            failures.append(path.name)
    if failures:
        raise RuntimeError("could not roll back materialized artifacts: " + ", ".join(failures))


def _discard_paths(paths: list[Path]) -> None:
    for path in reversed(paths):
        try:
            path.unlink()
        except OSError:
            pass


def _unique_json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate benchmark plan key: {key}")
        result[key] = value
    return result


def _reject_json_number(value: str) -> object:
    raise ValueError(f"benchmark plan numbers must be canonical decimal strings: {value}")


def _plan_mapping(value: object, name: str) -> dict[str, object]:
    if not isinstance(value, dict) or any(not isinstance(key, str) for key in value):
        raise ValueError(f"{name} must be an object")
    return value


def _plan_array(value: object, name: str) -> list[object]:
    if not isinstance(value, list):
        raise ValueError(f"{name} must be an array")
    return value


def _plan_string(value: object, name: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{name} must be a string")
    return value


def _plan_uint(value: object, name: str) -> int:
    if (
        not isinstance(value, str)
        or not value.isdecimal()
        or (len(value) > 1 and value.startswith("0"))
    ):
        raise ValueError(f"{name} must be a canonical decimal string")
    parsed = int(value)
    if parsed > U64_MAX:
        raise ValueError(f"{name} exceeds u64")
    return parsed


def _require_plan_keys(
    value: dict[str, object],
    expected: set[str],
    name: str,
) -> None:
    if set(value) != expected:
        raise ValueError(f"{name} has unknown or missing keys")


def _require_plan_identifier(name: str, value: str) -> None:
    if (
        not isinstance(value, str)
        or not value
        or len(value) > 128
        or not value.isascii()
        or not value[0].isalnum()
        or any(not (character.isalnum() or character in "_.-") for character in value)
    ):
        raise ValueError(f"{name} is not a canonical plan identifier")


def _instrument_config(entry: CatalogEntry) -> InstrumentConfig:
    return InstrumentConfig(
        entry.instrument_id,
        MatchingConfig(
            entry.max_order_quantity,
            entry.tick_increment,
            entry.max_active_orders,
        ),
    )
