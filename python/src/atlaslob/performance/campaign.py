"""Resumable execution of frozen Phase 5 benchmark plans."""

from __future__ import annotations

import shutil
import tempfile
import time
from collections import defaultdict
from collections.abc import Iterator, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Final, Literal

from atlaslob.performance.analysis import (
    analyze_observations,
    render_report_markdown,
    render_report_svg,
)
from atlaslob.performance.bundle import (
    BundleSummary,
    verify_bundle_with_verified_workloads,
    write_inventory,
)
from atlaslob.performance.schemas import (
    EnvironmentManifest,
    Observation,
    WorkloadManifest,
    environment_from_dict,
    file_sha256,
    observation_from_dict,
    read_canonical_document,
    validate_observation_against_workload,
    write_canonical_document,
)
from atlaslob.performance.suite import (
    Runner,
    RunnerMode,
    _prepare_runner,
    _run_label,
    run_verified_suite,
)
from atlaslob.performance.workloads import (
    BenchmarkPlan,
    BenchmarkPlanPoint,
    VerifiedWorkload,
    load_benchmark_plan,
    revalidate_verified_workload,
    verify_campaign_workload,
)

_MODE_BY_BOUNDARY: Final[dict[str, RunnerMode]] = {
    "core_throughput": "throughput",
    "core_latency": "latency",
    "core_allocation": "allocation",
    "core_construction": "construction",
    "core_preload": "preload",
    "core_setup_allocation": "setup-allocation",
    "replay_fast": "replay-fast",
    "replay_verify": "replay-verify",
    "python_objects": "python-objects",
    "python_columns": "python-columns",
    "python_summary": "python-summary",
}
_ALLOCATION_BOUNDARIES: Final = frozenset({"core_allocation", "core_setup_allocation"})
_PYTHON_BATCH_SIZES: Final = (1, 64, 1_024, 65_536)
_ATTEMPT_SLOTS_PER_BATCH: Final = 10_000
_ATTEMPT_PREFIX: Final = "attempt-"
_OBSERVATION_NAME: Final = "observation-00001.json"
OFFICIAL_TIER_ORDER: Final = ("study", "memory", "replay", "python", "headline")
_PHYSICAL_HOST_FIELDS: Final = (
    "os",
    "kernel",
    "host_class",
    "cpu_model",
    "architecture",
    "physical_cores",
    "logical_cpus",
    "microcode",
    "memory_bytes",
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
)


@dataclass(frozen=True, slots=True)
class CampaignShape:
    point_id: str
    tier: str
    workload_id: str
    boundary: str
    mode: RunnerMode
    batch_size: int | None

    @property
    def directory_name(self) -> str:
        suffix = "" if self.batch_size is None else f"-batch-{self.batch_size:05d}"
        return f"{self.boundary.replace('_', '-')}{suffix}"

    @property
    def role(self) -> Literal["core", "allocation", "python"]:
        if self.boundary.startswith("python_"):
            return "python"
        if self.boundary in _ALLOCATION_BOUNDARIES:
            return "allocation"
        return "core"


@dataclass(frozen=True, slots=True)
class CampaignRunners:
    core: Runner
    allocation: Runner | None = None
    python: Runner | None = None


@dataclass(frozen=True, slots=True)
class CampaignSummary:
    shapes: int
    attempts: int
    valid: int
    invalid: int


@dataclass(frozen=True, slots=True)
class OrderedTierSummary:
    tier: str
    campaign: CampaignSummary
    elapsed_seconds: float
    checkpoint_path: Path
    checkpoint_sha256: str


@dataclass(frozen=True, slots=True)
class OrderedCampaignSummary:
    tiers: tuple[OrderedTierSummary, ...]

    @property
    def campaign(self) -> CampaignSummary:
        return CampaignSummary(
            shapes=sum(item.campaign.shapes for item in self.tiers),
            attempts=sum(item.campaign.attempts for item in self.tiers),
            valid=sum(item.campaign.valid for item in self.tiers),
            invalid=sum(item.campaign.invalid for item in self.tiers),
        )


ShapeKey = tuple[object, ...]


@dataclass(frozen=True, slots=True)
class VerifiedWorkloadCatalog(Mapping[ShapeKey, VerifiedWorkload]):
    """Immutable exact-plan mapping of shapes to verified workload bytes."""

    plan_path: Path
    plan_sha256: str
    plan: BenchmarkPlan
    workload_directory: Path
    entries: tuple[tuple[ShapeKey, VerifiedWorkload], ...]

    def __getitem__(self, key: ShapeKey) -> VerifiedWorkload:
        for candidate, workload in self.entries:
            if candidate == key:
                return workload
        raise KeyError(key)

    def __iter__(self) -> Iterator[ShapeKey]:
        return (key for key, _ in self.entries)

    def __len__(self) -> int:
        return len(self.entries)


def expand_campaign(
    plan: BenchmarkPlan,
    *,
    point_ids: Sequence[str] = (),
    tiers: Sequence[str] = (),
) -> tuple[CampaignShape, ...]:
    """Expand one plan into deterministic point/boundary/batch shapes."""

    if point_ids and tiers:
        raise ValueError("campaign point and tier filters are mutually exclusive")
    selected_points = set(point_ids)
    selected_tiers = set(tiers)
    known_points = {point.point_id for point in plan.points}
    known_tiers = {point.tier for point in plan.points}
    if selected_points - known_points:
        raise ValueError("campaign point filter names an unknown plan point")
    if selected_tiers - known_tiers:
        raise ValueError("campaign tier filter names an unknown plan tier")

    shapes: list[CampaignShape] = []
    for point in plan.points:
        if selected_points and point.point_id not in selected_points:
            continue
        if selected_tiers and point.tier not in selected_tiers:
            continue
        for boundary in point.boundaries:
            mode = _MODE_BY_BOUNDARY[boundary]
            batch_sizes: tuple[int | None, ...] = (
                tuple(point.python_batch_sizes) if boundary.startswith("python_") else (None,)
            )
            shapes.extend(
                CampaignShape(
                    point.point_id,
                    point.tier,
                    point.workload_id,
                    boundary,
                    mode,
                    batch_size,
                )
                for batch_size in batch_sizes
            )
    if not shapes:
        raise ValueError("campaign selection contains no measurement shapes")
    return tuple(shapes)


def run_campaign(
    plan_path: Path,
    bundle_directory: Path,
    *,
    runners: CampaignRunners,
    suite_label: str,
    valid_observations: int = 10,
    max_attempts: int = 20,
    timeout_seconds: int = 900,
    point_ids: Sequence[str] = (),
    tiers: Sequence[str] = (),
    resume: bool = False,
    _catalog: VerifiedWorkloadCatalog | None = None,
) -> CampaignSummary:
    """Run a standalone campaign round-robin, retaining every attempt."""

    if isinstance(valid_observations, bool) or not 1 <= valid_observations <= 10_000:
        raise ValueError("valid_observations must be in [1, 10000]")
    if isinstance(max_attempts, bool) or not valid_observations <= max_attempts <= 10_000:
        raise ValueError("max_attempts must be between the valid target and 10000")

    catalog = (
        build_verified_workload_catalog(plan_path, bundle_directory / "workloads")
        if _catalog is None
        else _require_current_catalog(_catalog, plan_path, bundle_directory / "workloads")
    )
    plan = catalog.plan
    all_shapes = expand_campaign(plan)
    selected_shapes = expand_campaign(plan, point_ids=point_ids, tiers=tiers)
    existing_shapes = _existing_campaign_shapes(bundle_directory, all_shapes)
    relevant_shapes = tuple(dict.fromkeys((*selected_shapes, *existing_shapes)))
    prepared = _preflight_runners(relevant_shapes, runners)
    _require_one_physical_host(
        tuple(value[1] for value in prepared.values()),
    )
    seen_document_digests: set[str] = set()
    seen_run_labels: set[str] = set()
    loaded_states: dict[CampaignShape, list[Observation]] = {}
    for shape in relevant_shapes:
        loaded_states[shape] = _load_shape_attempts(
            shape,
            bundle_directory,
            plan,
            catalog,
            prepared,
            suite_label,
            seen_document_digests,
            seen_run_labels,
        )
    states = {shape: loaded_states[shape] for shape in selected_shapes}
    if any(len(attempts) > max_attempts for attempts in states.values()):
        raise ValueError("campaign output already exceeds the attempt limit")
    if not resume and any(states[shape] for shape in selected_shapes):
        raise ValueError("campaign output already contains attempts; use --resume")
    if (bundle_directory / "inventory.json").exists():
        raise ValueError("an inventoried campaign is immutable")

    while any(
        sum(observation.valid for observation in states[shape]) < valid_observations
        for shape in selected_shapes
    ):
        for shape in selected_shapes:
            existing = states[shape]
            if sum(observation.valid for observation in existing) >= valid_observations:
                continue
            if len(existing) >= max_attempts:
                raise ValueError(
                    f"campaign shape {shape.point_id}/{shape.directory_name} "
                    "reached its attempt limit"
                )
            attempt = len(existing) + 1
            point = _point_by_id(plan, shape.point_id)
            workload = catalog[_point_shape_key(point)]
            manifest_path = workload.manifest_path
            runner = _runner_for(shape, runners)
            output = _shape_directory(bundle_directory, shape) / (f"{_ATTEMPT_PREFIX}{attempt:05d}")
            paths = run_verified_suite(
                workload,
                output,
                baseline=runner,
                suite_label=suite_label,
                mode=shape.mode,
                observations=1,
                block_start=_campaign_attempt_counter(shape, attempt),
                batch_size=shape.batch_size,
                timeout_seconds=timeout_seconds,
            )
            if paths != (output / _OBSERVATION_NAME,):
                raise ValueError("campaign runner produced an unexpected observation path")
            observation = _validate_attempt(
                paths[0],
                shape,
                manifest_path,
                workload.manifest,
                prepared[shape.role],
                suite_label,
                attempt,
            )
            _record_unique_attempt(
                paths[0],
                observation,
                seen_document_digests,
                seen_run_labels,
            )
            existing.append(observation)
            if not observation.valid:
                raise ValueError(
                    f"campaign retained invalid attempt for "
                    f"{shape.point_id}/{shape.directory_name}: "
                    f"{observation.failure_reason}"
                )

    return _summarize(states, selected_shapes)


def run_ordered_campaign(
    plan_path: Path,
    bundle_directory: Path,
    checkpoint_directory: Path,
    *,
    runners: CampaignRunners,
    suite_label: str,
    valid_observations: int = 10,
    max_attempts: int = 20,
    timeout_seconds: int = 900,
    resume: bool = False,
) -> OrderedCampaignSummary:
    """Run the five official tiers in frozen order with one verified catalog."""

    catalog = build_verified_workload_catalog(plan_path, bundle_directory / "workloads")
    results: list[OrderedTierSummary] = []
    for tier in OFFICIAL_TIER_ORDER:
        if results:
            verify_campaign_checkpoint(bundle_directory, results[-1].checkpoint_path)
        started = time.monotonic()
        summary = run_campaign(
            plan_path,
            bundle_directory,
            runners=runners,
            suite_label=suite_label,
            valid_observations=valid_observations,
            max_attempts=max_attempts,
            timeout_seconds=timeout_seconds,
            tiers=(tier,),
            resume=resume,
            _catalog=catalog,
        )
        checkpoint_path = checkpoint_directory / f"after-{tier}.sha256"
        create_campaign_checkpoint(bundle_directory, checkpoint_path)
        results.append(
            OrderedTierSummary(
                tier=tier,
                campaign=summary,
                elapsed_seconds=time.monotonic() - started,
                checkpoint_path=checkpoint_path.resolve(),
                checkpoint_sha256=file_sha256(checkpoint_path),
            )
        )
    for result in results:
        verify_campaign_checkpoint(bundle_directory, result.checkpoint_path)
    return OrderedCampaignSummary(tuple(results))


def create_campaign_checkpoint(bundle_directory: Path, checkpoint_path: Path) -> None:
    """Write and immediately verify a deterministic SHA-256 bundle checkpoint."""

    bundle = bundle_directory.resolve(strict=True)
    files: list[Path] = []
    for path in bundle.rglob("*"):
        if path.is_symlink():
            raise ValueError("campaign bundle checkpoint does not permit symlinks")
        if path.is_file():
            files.append(path.resolve(strict=True))
    files.sort(key=lambda path: path.as_posix())
    checkpoint_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="ascii",
            newline="\n",
            prefix=f".{checkpoint_path.name}.",
            dir=checkpoint_path.parent,
            delete=False,
        ) as output:
            temporary_name = output.name
            for path in files:
                output.write(f"{file_sha256(path)}  {path}\n")
        Path(temporary_name).replace(checkpoint_path)
        temporary_name = None
        verify_campaign_checkpoint(bundle, checkpoint_path)
    finally:
        if temporary_name is not None:
            Path(temporary_name).unlink(missing_ok=True)


def verify_campaign_checkpoint(bundle_directory: Path, checkpoint_path: Path) -> None:
    """Recheck every exact file named by a deterministic campaign checkpoint."""

    bundle = bundle_directory.resolve(strict=True)
    lines = checkpoint_path.read_text(encoding="ascii").splitlines()
    paths: list[Path] = []
    for line in lines:
        try:
            expected, raw_path = line.split("  ", 1)
        except ValueError as exc:
            raise ValueError("campaign checkpoint has invalid sha256sum syntax") from exc
        path = Path(raw_path)
        try:
            path.relative_to(bundle)
        except ValueError as exc:
            raise ValueError("campaign checkpoint path escapes its bundle") from exc
        if len(expected) != 64 or any(char not in "0123456789abcdef" for char in expected):
            raise ValueError("campaign checkpoint contains an invalid SHA-256")
        if file_sha256(path) != expected:
            raise ValueError(f"campaign checkpoint digest mismatch: {path}")
        paths.append(path)
    if paths != sorted(set(paths), key=lambda path: path.as_posix()):
        raise ValueError("campaign checkpoint paths are not unique and sorted")


def finalize_campaign(
    plan_path: Path,
    bundle_directory: Path,
    *,
    valid_observations: int = 10,
    allow_exploratory: bool = False,
) -> BundleSummary:
    """Validate complete coverage, render reports, and inventory the bundle."""

    if isinstance(valid_observations, bool) or not 1 <= valid_observations <= 10_000:
        raise ValueError("valid_observations must be in [1, 10000]")
    if valid_observations < 10 and not allow_exploratory:
        raise ValueError("official campaign finalization requires at least ten valid observations")
    if (bundle_directory / "inventory.json").exists():
        raise ValueError("an inventoried campaign is immutable")
    reports_directory = bundle_directory / "reports"
    if reports_directory.exists() and any(reports_directory.iterdir()):
        raise ValueError("campaign report directory must be empty")

    catalog = build_verified_workload_catalog(plan_path, bundle_directory / "workloads")
    plan = catalog.plan
    shapes = expand_campaign(plan)
    _require_exact_shape_directories(bundle_directory, shapes)
    environments = _load_environments(bundle_directory / "environments")
    environment_by_digest = {
        file_sha256(path): (path, environment) for path, environment in environments
    }
    observations_by_context: dict[str, list[Path]] = defaultdict(list)
    roles_by_context: dict[str, set[str]] = defaultdict(set)
    referenced_environments: set[str] = set()
    seen_document_digests: set[str] = set()
    seen_run_labels: set[str] = set()
    suite_labels: set[str] = set()

    for shape in shapes:
        point = _point_by_id(plan, shape.point_id)
        workload = catalog[_point_shape_key(point)]
        manifest_path = workload.manifest_path
        manifest = workload.manifest
        attempts = _load_shape_attempts_without_runner(
            shape,
            bundle_directory,
            manifest_path,
            manifest,
            seen_document_digests,
            seen_run_labels,
            suite_labels,
        )
        if sum(observation.valid for _, observation in attempts) < valid_observations:
            raise ValueError(
                f"campaign shape {shape.point_id}/{shape.directory_name} "
                "has too few valid observations"
            )
        for path, observation in attempts:
            environment_entry = environment_by_digest.get(observation.environment_sha256)
            if environment_entry is None:
                raise ValueError("campaign observation references a missing environment")
            environment = environment_entry[1]
            _validate_environment_role(environment, shape.role)
            if not allow_exploratory and environment.classification != "official":
                raise ValueError("official campaign finalization found exploratory evidence")
            observations_by_context[observation.host_context_sha256].append(path)
            roles_by_context[observation.host_context_sha256].add(shape.role)
            referenced_environments.add(observation.environment_sha256)

    if referenced_environments != set(environment_by_digest):
        raise ValueError("campaign contains an unreferenced environment manifest")
    if len(suite_labels) != 1:
        raise ValueError("campaign observations must use one suite label")
    _require_python_harnesses(bundle_directory, tuple(environment_by_digest.values()))
    _require_one_physical_host(tuple(environment for _, environment in environments))
    reports_preexisted = reports_directory.exists()
    staging_directory = Path(tempfile.mkdtemp(prefix=".reports-", dir=bundle_directory))
    reports_published = False
    try:
        _render_campaign_reports(
            bundle_directory,
            staging_directory,
            observations_by_context,
            roles_by_context,
            catalog,
            environment_by_digest,
        )
        if reports_preexisted:
            reports_directory.rmdir()
        staging_directory.replace(reports_directory)
        reports_published = True
        write_inventory(bundle_directory)
        return verify_bundle_with_verified_workloads(bundle_directory, tuple(catalog.values()))
    except BaseException:
        (bundle_directory / "inventory.json").unlink(missing_ok=True)
        if reports_published and reports_directory.exists():
            shutil.rmtree(reports_directory)
        if reports_preexisted and not reports_directory.exists():
            reports_directory.mkdir()
        raise
    finally:
        if staging_directory.exists():
            shutil.rmtree(staging_directory)


def _render_campaign_reports(
    bundle_directory: Path,
    reports_directory: Path,
    observations_by_context: dict[str, list[Path]],
    roles_by_context: dict[str, set[str]],
    catalog: VerifiedWorkloadCatalog,
    environment_by_digest: dict[str, tuple[Path, EnvironmentManifest]],
) -> None:
    used_report_names: set[str] = set()
    for context in sorted(observations_by_context):
        observation_paths = tuple(
            sorted(
                observations_by_context[context],
                key=lambda path: path.relative_to(bundle_directory).as_posix(),
            )
        )
        observation_values = tuple(
            read_canonical_document(path, observation_from_dict) for path in observation_paths
        )
        workload_digests = {item.workload_manifest_sha256 for item in observation_values}
        environment_digests = {item.environment_sha256 for item in observation_values}
        workloads = tuple(
            (workload.manifest_sha256, workload.manifest)
            for workload in sorted(catalog.values(), key=lambda item: item.manifest_path.name)
            if workload.manifest_sha256 in workload_digests
        )
        environments = tuple(
            (digest, environment)
            for digest, (_path, environment) in sorted(
                environment_by_digest.items(), key=lambda item: item[1][0].name
            )
            if digest in environment_digests
        )
        report = analyze_observations(
            observation_values,
            workloads=workloads,
            environments=environments,
            source_digests=tuple(file_sha256(path) for path in observation_paths),
        )
        role_name = "-".join(sorted(roles_by_context[context]))
        report_name = role_name
        if report_name in used_report_names:
            report_name = f"{role_name}-{context[:12]}"
        used_report_names.add(report_name)
        report_path = reports_directory / f"report-{report_name}.json"
        write_canonical_document(report_path, report)
        report_path.with_suffix(".md").write_text(
            render_report_markdown(report),
            encoding="ascii",
            newline="\n",
        )
        report_path.with_suffix(".svg").write_text(
            render_report_svg(report),
            encoding="ascii",
            newline="\n",
        )


def build_verified_workload_catalog(
    plan_path: Path,
    directory: Path,
) -> VerifiedWorkloadCatalog:
    """Publish an immutable catalog only after every exact plan input verifies."""

    resolved_plan_path = plan_path.resolve(strict=True)
    plan_sha256 = file_sha256(resolved_plan_path)
    plan = load_benchmark_plan(resolved_plan_path)
    if not directory.is_dir():
        raise ValueError("campaign workload directory does not exist")
    resolved_directory = directory.resolve(strict=True)
    expected = {_point_shape_key(point) for point in plan.points}
    entries: list[tuple[ShapeKey, VerifiedWorkload]] = []
    seen: set[ShapeKey] = set()
    for path in sorted(resolved_directory.glob("*.json")):
        workload = verify_campaign_workload(path)
        key = _manifest_shape_key(workload.manifest)
        if key in seen:
            raise ValueError("campaign contains duplicate workload manifests")
        seen.add(key)
        entries.append((key, workload))
    if seen != expected:
        raise ValueError("campaign workload manifests do not exactly cover its plan")
    if (
        file_sha256(resolved_plan_path) != plan_sha256
        or load_benchmark_plan(resolved_plan_path) != plan
    ):
        raise ValueError("campaign plan changed during workload verification")
    return VerifiedWorkloadCatalog(
        plan_path=resolved_plan_path,
        plan_sha256=plan_sha256,
        plan=plan,
        workload_directory=resolved_directory,
        entries=tuple(entries),
    )


def _require_current_catalog(
    catalog: VerifiedWorkloadCatalog,
    plan_path: Path,
    directory: Path,
) -> VerifiedWorkloadCatalog:
    resolved_plan_path = plan_path.resolve(strict=True)
    resolved_directory = directory.resolve(strict=True)
    if (
        catalog.plan_path != resolved_plan_path
        or catalog.workload_directory != resolved_directory
        or file_sha256(resolved_plan_path) != catalog.plan_sha256
        or load_benchmark_plan(resolved_plan_path) != catalog.plan
    ):
        raise ValueError("verified workload catalog does not match the campaign plan")
    expected_paths = {workload.manifest_path for _, workload in catalog.entries}
    current_paths = tuple(sorted(resolved_directory.glob("*.json")))
    if (
        any(path.is_symlink() for path in current_paths)
        or {path.resolve(strict=True) for path in current_paths} != expected_paths
    ):
        raise ValueError("verified workload catalog paths no longer exactly cover the directory")
    expected = {_point_shape_key(point) for point in catalog.plan.points}
    actual = {key for key, _ in catalog.entries}
    if len(actual) != len(catalog.entries) or actual != expected:
        raise ValueError("verified workload catalog does not exactly cover the campaign plan")
    for key, workload in catalog.entries:
        if (
            key != _manifest_shape_key(workload.manifest)
            or workload.manifest_path.parent != resolved_directory
        ):
            raise ValueError("verified workload catalog contains a shape or path mismatch")
        revalidate_verified_workload(workload)
    return catalog


def _point_shape_key(point: BenchmarkPlanPoint) -> tuple[object, ...]:
    return (
        point.workload_id,
        point.seed,
        point.preload_commands,
        point.warmup_commands,
        point.measured_commands,
        point.instrument_count,
        point.active_order_target,
        point.sweep_depth if point.workload_id == "W05" else 0,
    )


def _manifest_shape_key(manifest: WorkloadManifest) -> tuple[object, ...]:
    parameters = dict(manifest.parameters)
    return (
        manifest.workload_id,
        manifest.seed,
        manifest.preload_commands,
        manifest.warmup_commands,
        manifest.measured_commands,
        int(parameters["instrument_count"]),
        int(parameters["active_order_target"]),
        int(parameters["sweep_depth"]),
    )


def _point_by_id(plan: BenchmarkPlan, point_id: str) -> BenchmarkPlanPoint:
    return next(point for point in plan.points if point.point_id == point_id)


def _preflight_runners(
    shapes: Sequence[CampaignShape],
    runners: CampaignRunners,
) -> dict[str, tuple[str, EnvironmentManifest, str]]:
    roles = {shape.role for shape in shapes}
    prepared: dict[str, tuple[str, EnvironmentManifest, str]] = {}
    for role in sorted(roles):
        runner = _runner_for_role(role, runners)
        modes = sorted({shape.mode for shape in shapes if shape.role == role})
        first: tuple[str, EnvironmentManifest, str] | None = None
        for mode in modes:
            current = _prepare_runner(runner, mode)
            if first is None:
                first = current
            elif current != first:
                raise ValueError("campaign runner identity changed during preflight")
        assert first is not None
        prepared[role] = first
    return prepared


def _runner_for(shape: CampaignShape, runners: CampaignRunners) -> Runner:
    return _runner_for_role(shape.role, runners)


def _runner_for_role(role: str, runners: CampaignRunners) -> Runner:
    runner = {
        "core": runners.core,
        "allocation": runners.allocation,
        "python": runners.python,
    }[role]
    if runner is None:
        raise ValueError(f"campaign requires a {role} runner")
    if runner.variant != "standalone":
        raise ValueError("campaign runners must use the standalone variant")
    return runner


def _shape_directory(bundle_directory: Path, shape: CampaignShape) -> Path:
    return bundle_directory / "observations" / shape.point_id / shape.directory_name


def _campaign_attempt_counter(shape: CampaignShape, attempt: int) -> int:
    if shape.batch_size is None:
        return attempt
    batch_index = _PYTHON_BATCH_SIZES.index(shape.batch_size)
    return batch_index * _ATTEMPT_SLOTS_PER_BATCH + attempt


def _existing_campaign_shapes(
    bundle_directory: Path,
    shapes: Sequence[CampaignShape],
) -> tuple[CampaignShape, ...]:
    root = bundle_directory / "observations"
    if not root.exists():
        return ()
    if not root.is_dir():
        raise ValueError("campaign observation path is not a directory")
    shape_by_path = {(shape.point_id, shape.directory_name): shape for shape in shapes}
    existing: set[CampaignShape] = set()
    for point_entry in root.iterdir():
        if not point_entry.is_dir():
            raise ValueError("campaign observation root contains an unexpected file")
        entries = tuple(point_entry.iterdir())
        if not entries:
            raise ValueError("campaign observation point directory is empty")
        for shape_entry in entries:
            shape = shape_by_path.get((point_entry.name, shape_entry.name))
            if shape is None or not shape_entry.is_dir():
                raise ValueError("campaign contains an unknown observation shape")
            _strict_attempt_paths(shape_entry)
            existing.add(shape)
    return tuple(shape for shape in shapes if shape in existing)


def _load_shape_attempts(
    shape: CampaignShape,
    bundle_directory: Path,
    plan: BenchmarkPlan,
    catalog: VerifiedWorkloadCatalog,
    prepared: dict[str, tuple[str, EnvironmentManifest, str]],
    suite_label: str,
    seen_document_digests: set[str],
    seen_run_labels: set[str],
) -> list[Observation]:
    point = _point_by_id(plan, shape.point_id)
    workload = catalog[_point_shape_key(point)]
    manifest_path = workload.manifest_path
    manifest = workload.manifest
    directory = _shape_directory(bundle_directory, shape)
    if not directory.exists():
        return []
    attempts = _strict_attempt_paths(directory)
    result: list[Observation] = []
    for attempt, path in enumerate(attempts, start=1):
        observation = _validate_attempt(
            path,
            shape,
            manifest_path,
            manifest,
            prepared[shape.role],
            suite_label,
            attempt,
        )
        _record_unique_attempt(
            path,
            observation,
            seen_document_digests,
            seen_run_labels,
        )
        result.append(observation)
    return result


def _load_shape_attempts_without_runner(
    shape: CampaignShape,
    bundle_directory: Path,
    manifest_path: Path,
    manifest: WorkloadManifest,
    seen_document_digests: set[str],
    seen_run_labels: set[str],
    suite_labels: set[str],
) -> tuple[tuple[Path, Observation], ...]:
    directory = _shape_directory(bundle_directory, shape)
    attempts = _strict_attempt_paths(directory)
    result: list[tuple[Path, Observation]] = []
    for attempt, path in enumerate(attempts, start=1):
        observation = read_canonical_document(path, observation_from_dict)
        _validate_shape_observation(observation, shape, manifest_path, manifest)
        _validate_attempt_run_label(observation, shape, manifest, attempt)
        _record_unique_attempt(
            path,
            observation,
            seen_document_digests,
            seen_run_labels,
        )
        suite_labels.add(observation.suite_label)
        result.append((path, observation))
    return tuple(result)


def _strict_attempt_paths(directory: Path) -> tuple[Path, ...]:
    if not directory.is_dir():
        raise ValueError("campaign shape directory is missing")
    paths: list[Path] = []
    for expected_index, entry in enumerate(
        sorted(directory.iterdir(), key=lambda path: path.name),
        start=1,
    ):
        expected_name = f"{_ATTEMPT_PREFIX}{expected_index:05d}"
        if not entry.is_dir() or entry.name != expected_name:
            raise ValueError("campaign attempts must be contiguous canonical directories")
        contents = tuple(entry.iterdir())
        if len(contents) != 1 or contents[0].name != _OBSERVATION_NAME:
            raise ValueError("campaign attempt directory has unexpected contents")
        paths.append(contents[0])
    return tuple(paths)


def _validate_attempt(
    path: Path,
    shape: CampaignShape,
    manifest_path: Path,
    manifest: WorkloadManifest,
    prepared: tuple[str, EnvironmentManifest, str],
    suite_label: str,
    attempt: int,
) -> Observation:
    observation = read_canonical_document(path, observation_from_dict)
    _validate_shape_observation(observation, shape, manifest_path, manifest)
    binary_digest, environment, environment_digest = prepared
    if (
        observation.binary_sha256 != binary_digest
        or observation.environment_sha256 != environment_digest
        or observation.host_context_sha256 != environment.host_context_sha256
        or observation.suite_label != suite_label
        or observation.variant != "standalone"
    ):
        raise ValueError("campaign attempt differs from its runner or suite identity")
    _validate_attempt_run_label(observation, shape, manifest, attempt)
    return observation


def _validate_attempt_run_label(
    observation: Observation,
    shape: CampaignShape,
    manifest: WorkloadManifest,
    attempt: int,
) -> None:
    expected = _run_label(
        observation.suite_label,
        manifest.workload_id,
        manifest.stream_sha256,
        shape.boundary,
        "standalone",
        0,
        0,
        _campaign_attempt_counter(shape, attempt),
    )
    if observation.run_label != expected:
        raise ValueError("campaign attempt has a stale or noncanonical run label")


def _record_unique_attempt(
    path: Path,
    observation: Observation,
    seen_document_digests: set[str],
    seen_run_labels: set[str],
) -> None:
    document_digest = file_sha256(path)
    if document_digest in seen_document_digests:
        raise ValueError("campaign contains a duplicate observation document")
    if observation.run_label in seen_run_labels:
        raise ValueError("campaign contains a duplicate observation run label")
    seen_document_digests.add(document_digest)
    seen_run_labels.add(observation.run_label)


def _validate_shape_observation(
    observation: Observation,
    shape: CampaignShape,
    manifest_path: Path,
    manifest: WorkloadManifest,
) -> None:
    if (
        observation.boundary != shape.boundary
        or observation.workload_id != shape.workload_id
        or dict(observation.measurement_parameters).get("batch_size")
        != (None if shape.batch_size is None else str(shape.batch_size))
    ):
        raise ValueError("campaign attempt differs from its planned shape")
    validate_observation_against_workload(
        observation,
        manifest,
        manifest_sha256=file_sha256(manifest_path),
    )


def _load_environments(
    directory: Path,
) -> tuple[tuple[Path, EnvironmentManifest], ...]:
    if not directory.is_dir():
        raise ValueError("campaign environment directory does not exist")
    entries = tuple(directory.iterdir())
    if any(not path.is_file() or path.suffix != ".json" for path in entries):
        raise ValueError("campaign environment directory contains an unexpected entry")
    values = tuple(
        (
            path,
            read_canonical_document(path, environment_from_dict),
        )
        for path in sorted(entries)
    )
    if not values:
        raise ValueError("campaign contains no environment manifests")
    digests = tuple(file_sha256(path) for path, _ in values)
    if len(set(digests)) != len(digests):
        raise ValueError("campaign contains duplicate environment documents")
    return values


def _validate_environment_role(environment: EnvironmentManifest, role: str) -> None:
    if role == "python":
        if environment.runtime_kind != "cpython":
            raise ValueError("Python campaign evidence requires a CPython environment")
    elif environment.runtime_kind != "native":
        raise ValueError("native campaign evidence requires a native environment")
    else:
        expected_target = (
            "atlas_bench_alloc_runner" if role == "allocation" else "atlas_bench_runner"
        )
        if not any(
            name == expected_target or name.startswith(f"{expected_target}.")
            for name, _ in environment.build_target_profiles
        ):
            raise ValueError("native campaign environment has the wrong runner target")


def _require_one_physical_host(environments: Sequence[EnvironmentManifest]) -> None:
    identities = {
        tuple(getattr(environment, field) for field in _PHYSICAL_HOST_FIELDS)
        for environment in environments
    }
    if len(identities) != 1:
        raise ValueError("campaign environments do not describe one physical host")


def _require_exact_shape_directories(
    bundle_directory: Path,
    shapes: Sequence[CampaignShape],
) -> None:
    root = bundle_directory / "observations"
    if not root.is_dir():
        raise ValueError("campaign observation directory does not exist")
    expected: dict[str, set[str]] = defaultdict(set)
    for shape in shapes:
        expected[shape.point_id].add(shape.directory_name)
    actual_points = {entry.name for entry in root.iterdir() if entry.is_dir()}
    if actual_points != set(expected) or any(not entry.is_dir() for entry in root.iterdir()):
        raise ValueError("campaign observation points do not exactly cover its plan")
    for point_id, directory_names in expected.items():
        point_directory = root / point_id
        entries = tuple(point_directory.iterdir())
        if {entry.name for entry in entries} != directory_names or any(
            not entry.is_dir() for entry in entries
        ):
            raise ValueError("campaign observation shapes do not exactly cover its plan")


def _require_python_harnesses(
    bundle_directory: Path,
    environments: Sequence[tuple[Path, EnvironmentManifest]],
) -> None:
    expected = {
        environment.harness_sha256
        for _, environment in environments
        if environment.runtime_kind == "cpython"
    }
    if not expected:
        return
    actual = {
        file_sha256(path)
        for path in bundle_directory.rglob("*")
        if path.is_file() and path.name != "inventory.json"
    }
    if not expected.issubset(actual):
        raise ValueError("campaign is missing its CPython worker harness")


def _summarize(
    states: dict[CampaignShape, list[Observation]],
    shapes: Sequence[CampaignShape],
) -> CampaignSummary:
    observations = tuple(observation for shape in shapes for observation in states[shape])
    return CampaignSummary(
        shapes=len(shapes),
        attempts=len(observations),
        valid=sum(observation.valid for observation in observations),
        invalid=sum(not observation.valid for observation in observations),
    )
