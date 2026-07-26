"""Deterministic statistics and human-readable benchmark reports."""

from __future__ import annotations

import hashlib
import json
from collections import defaultdict
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from decimal import Decimal, localcontext
from fractions import Fraction
from html import escape
from pathlib import Path

from atlaslob.performance.schemas import (
    TARGET_METRIC_BY_BOUNDARY,
    BenchmarkReport,
    Comparison,
    EnvironmentManifest,
    Experiment,
    GroupStatistics,
    Observation,
    ScalarStatistics,
    WorkloadManifest,
    canonical_json_bytes,
    environment_from_dict,
    file_sha256,
    observation_from_dict,
    observation_to_dict,
    read_canonical_document,
    validate_observation_against_workload,
)
from atlaslob.performance.workloads import verify_workload_manifest

_QUANTILES = (
    ("p50", 500, 1_000),
    ("p90", 900, 1_000),
    ("p95", 950, 1_000),
    ("p99", 990, 1_000),
    ("p99.9", 999, 1_000),
)
EXPERIMENT_PLAN_SCHEMA = "ATLAS_BENCH_EXPERIMENT_PLAN_V1"
_EXPERIMENT_PLAN_MAX_BYTES = 1024 * 1024


@dataclass(frozen=True, slots=True)
class ExperimentPlan:
    experiment_id: str
    policy: str
    target_comparison_id: str | None
    control_comparison_ids: tuple[str, ...]
    correctness_gate: str
    complexity_gate: str
    note_path: str
    note_sha256: str
    rationale: str


def load_experiment_plans(path: Path) -> tuple[ExperimentPlan, ...]:
    """Load a bounded canonical experiment decision plan."""

    try:
        if path.stat().st_size > _EXPERIMENT_PLAN_MAX_BYTES:
            raise ValueError("experiment plan exceeds its size bound")
        data = path.read_bytes()
    except OSError as exc:
        raise ValueError("experiment plan is not readable") from exc
    if len(data) > _EXPERIMENT_PLAN_MAX_BYTES:
        raise ValueError("experiment plan exceeds its size bound")
    try:
        text = data.decode("ascii")
        if not text.endswith("\n") or "\r" in text or text.count("\n") != 1:
            raise ValueError("experiment plan must be one LF-terminated JSON record")
        raw = json.loads(
            text,
            object_pairs_hook=_unique_json_object,
            parse_int=_reject_json_number,
            parse_float=_reject_json_number,
            parse_constant=_reject_json_number,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("experiment plan is not canonical ASCII JSON") from exc
    if not isinstance(raw, dict) or set(raw) != {"schema", "experiments"}:
        raise ValueError("experiment plan has unexpected fields")
    if raw["schema"] != EXPERIMENT_PLAN_SCHEMA or not isinstance(raw["experiments"], list):
        raise ValueError("experiment plan schema or experiments are invalid")
    if len(raw["experiments"]) > 999:
        raise ValueError("experiment plan exceeds its entry limit")
    plans: list[ExperimentPlan] = []
    encoded: list[dict[str, object]] = []
    keys = {
        "experiment_id",
        "policy",
        "target_comparison_id",
        "control_comparison_ids",
        "correctness_gate",
        "complexity_gate",
        "note_path",
        "note_sha256",
        "rationale",
    }
    for value in raw["experiments"]:
        if not isinstance(value, dict) or set(value) != keys:
            raise ValueError("experiment plan entry has unexpected fields")
        controls = value["control_comparison_ids"]
        if not isinstance(controls, list) or any(not isinstance(item, str) for item in controls):
            raise ValueError("experiment control comparison IDs must be strings")
        if controls != sorted(set(controls)):
            raise ValueError("experiment control comparison IDs must be unique and sorted")
        target = value["target_comparison_id"]
        if target is not None and not isinstance(target, str):
            raise ValueError("experiment target comparison ID must be a string or null")

        plan = ExperimentPlan(
            experiment_id=_required_string(value, "experiment_id"),
            policy=_required_string(value, "policy"),
            target_comparison_id=target,
            control_comparison_ids=tuple(controls),
            correctness_gate=_required_string(value, "correctness_gate"),
            complexity_gate=_required_string(value, "complexity_gate"),
            note_path=_required_string(value, "note_path"),
            note_sha256=_required_string(value, "note_sha256"),
            rationale=_required_string(value, "rationale"),
        )
        plans.append(plan)
        encoded.append(
            {
                "experiment_id": plan.experiment_id,
                "policy": plan.policy,
                "target_comparison_id": plan.target_comparison_id,
                "control_comparison_ids": list(plan.control_comparison_ids),
                "correctness_gate": plan.correctness_gate,
                "complexity_gate": plan.complexity_gate,
                "note_path": plan.note_path,
                "note_sha256": plan.note_sha256,
                "rationale": plan.rationale,
            }
        )
    if tuple(plan.experiment_id for plan in plans) != tuple(
        sorted({plan.experiment_id for plan in plans})
    ):
        raise ValueError("experiment plan IDs must be unique and sorted")
    canonical = canonical_json_bytes({"schema": EXPERIMENT_PLAN_SCHEMA, "experiments": encoded})
    if data != canonical:
        raise ValueError("experiment plan is not canonical")
    return tuple(plans)


def median(values: Sequence[int]) -> Fraction:
    if not values:
        raise ValueError("median requires at least one value")
    return _fraction_median(tuple(Fraction(value) for value in values))


def median_absolute_deviation(values: Sequence[int]) -> Fraction:
    center = median(values)
    return _fraction_median(tuple(abs(Fraction(value) - center) for value in values))


def nearest_rank(values: Sequence[int], numerator: int, denominator: int) -> int:
    if not values:
        raise ValueError("nearest-rank quantile requires at least one value")
    if not 0 < numerator <= denominator:
        raise ValueError("quantile must be in (0, 1]")
    ordered = sorted(values)
    rank = (numerator * len(ordered) + denominator - 1) // denominator
    return ordered[rank - 1]


def interquartile_range(values: Sequence[int]) -> int:
    return nearest_rank(values, 3, 4) - nearest_rank(values, 1, 4)


def analyze_observations(
    observations: Sequence[Observation],
    *,
    workloads: Sequence[tuple[str, WorkloadManifest]],
    environments: Sequence[tuple[str, EnvironmentManifest]],
    source_digests: Sequence[str] | None = None,
    limitations: Iterable[str] = (),
    experiment_plans: Sequence[ExperimentPlan] = (),
) -> BenchmarkReport:
    """Validate evidence bindings before computing any aggregate."""

    if not observations:
        raise ValueError("analysis requires at least one observation")
    workload_by_digest = {item.stream_sha256: (digest, item) for digest, item in workloads}
    if len(workload_by_digest) != len(workloads):
        raise ValueError("duplicate workload stream evidence")
    if len({digest for digest, _ in workloads}) != len(workloads):
        raise ValueError("duplicate workload manifest evidence")
    environment_by_digest = dict(environments)
    if len(environment_by_digest) != len(environments):
        raise ValueError("duplicate environment document evidence")

    for item in observations:
        workload_entry = workload_by_digest.get(item.workload_sha256)
        environment = environment_by_digest.get(item.environment_sha256)
        if workload_entry is None or environment is None:
            raise ValueError("observation references missing workload or environment evidence")
        workload_digest, workload = workload_entry
        if (
            item.workload_id != workload.workload_id
            or item.binary_sha256 != environment.binary_sha256
            or item.host_context_sha256 != environment.host_context_sha256
        ):
            raise ValueError("observation identity differs from its workload/environment")
        expected_runtime = "cpython" if item.boundary.startswith("python_") else "native"
        if environment.runtime_kind != expected_runtime:
            raise ValueError("observation boundary is incompatible with its runtime environment")
        validate_observation_against_workload(
            item,
            workload,
            manifest_sha256=workload_digest,
        )

    host_contexts = {item.host_context_sha256 for item in observations}
    if len(host_contexts) != 1:
        raise ValueError("mixed host/build contexts cannot share a report")
    suite_labels = {item.suite_label for item in observations}
    if len(suite_labels) != 1:
        raise ValueError("one report cannot combine multiple suite labels")
    run_labels = tuple(item.run_label for item in observations)
    if len(run_labels) != len(set(run_labels)):
        raise ValueError("observation run labels must be unique within a report")
    grouped: dict[
        tuple[
            str,
            str,
            str | None,
            tuple[tuple[str, str], ...],
            str,
            str,
            str,
            str,
        ],
        list[Observation],
    ] = defaultdict(list)
    for item in observations:
        grouped[
            (
                item.boundary,
                item.timed_input_kind,
                item.timed_input_sha256,
                item.measurement_parameters,
                item.workload_id,
                item.workload_sha256,
                item.workload_manifest_sha256,
                item.binary_sha256,
            )
        ].append(item)
    groups = tuple(
        _group_statistics(key, tuple(values), workload_by_digest[key[5]][1])
        for key, values in sorted(grouped.items())
    )
    comparisons = _comparisons(groups, observations, environment_by_digest)
    experiments = _experiments(comparisons, experiment_plans)

    if source_digests is None:
        source_digests = tuple(
            hashlib.sha256(canonical_json_bytes(observation_to_dict(item))).hexdigest()
            for item in observations
        )
    canonical_sources = tuple(sorted(source_digests))
    if len(canonical_sources) != len(observations) or len(set(canonical_sources)) != len(
        observations
    ):
        raise ValueError("every observation must have one unique source digest")

    recorded_limitations = set(limitations)
    referenced_environments = {
        item.environment_sha256: environment_by_digest[item.environment_sha256]
        for item in observations
    }
    classification = (
        "official"
        if all(item.classification == "official" for item in referenced_environments.values())
        and all(group.valid_observations >= 10 for group in groups)
        else "exploratory"
    )
    for environment in referenced_environments.values():
        recorded_limitations.update(environment.limitations)
    if classification == "exploratory":
        recorded_limitations.add(
            "report is exploratory and must not be used for authoritative performance claims"
        )
    if any(item.classification == "exploratory" for item in comparisons):
        recorded_limitations.add(
            "at least one A/B comparison uses exploratory environment qualification"
        )
    if _has_excluded_abba_blocks(observations):
        recorded_limitations.add(
            "A/B comparisons exclude incomplete blocks and blocks containing invalid observations"
        )
    for group in groups:
        if group.valid_observations < 10:
            recorded_limitations.add(
                f"{group.boundary}/{group.workload_id} has fewer than ten valid observations"
            )
        if group.invalid_observations:
            recorded_limitations.add(
                f"{group.boundary}/{group.workload_id} includes retained invalid observations"
            )
    return BenchmarkReport(
        classification=classification,
        suite_label=next(iter(suite_labels)),
        host_context_sha256=next(iter(host_contexts)),
        workload_manifest_sha256s=tuple(
            sorted({item.workload_manifest_sha256 for item in observations})
        ),
        environment_sha256s=tuple(sorted({item.environment_sha256 for item in observations})),
        source_observations=canonical_sources,
        groups=groups,
        comparisons=comparisons,
        experiments=experiments,
        limitations=tuple(sorted(recorded_limitations)),
    )


def analyze_paths(
    paths: Sequence[Path],
    *,
    workload_paths: Sequence[Path],
    environment_paths: Sequence[Path],
    experiment_plans: Sequence[ExperimentPlan] = (),
) -> BenchmarkReport:
    if not paths:
        raise ValueError("no observation paths were supplied")
    observations = tuple(read_canonical_document(path, observation_from_dict) for path in paths)
    workloads = tuple(
        (file_sha256(path), verify_workload_manifest(path)) for path in workload_paths
    )
    environments = tuple(
        (file_sha256(path), read_canonical_document(path, environment_from_dict))
        for path in environment_paths
    )
    return analyze_observations(
        observations,
        workloads=workloads,
        environments=environments,
        source_digests=tuple(file_sha256(path) for path in paths),
        experiment_plans=experiment_plans,
    )


def _parameter_text(parameters: tuple[tuple[str, str], ...]) -> str:
    return ", ".join(f"{key}={value}" for key, value in parameters)


def render_report_markdown(report: BenchmarkReport) -> str:
    lines = [
        "# AtlasLOB benchmark report",
        "",
        f"Qualification: **{report.classification.upper()}**",
        "",
        f"Suite: `{report.suite_label}`",
        "",
        f"Host/build context: `{report.host_context_sha256}`",
        "",
        "## Observation and timing statistics",
        "",
        (
            "| Boundary | Workload | Dimensions | Workload SHA-256 | Valid | Invalid | Commands | "
            "Resting-order denominator | Elapsed min ns | Elapsed max ns | "
            "Elapsed median ns | Elapsed MAD ns | Elapsed IQR ns |"
        ),
        "|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    lines.extend(
        "| "
        + " | ".join(
            (
                group.boundary,
                group.workload_id,
                _parameter_text(group.measurement_parameters),
                group.workload_sha256,
                str(group.valid_observations),
                str(group.invalid_observations),
                str(group.commands),
                str(group.resting_order_denominator),
                str(group.minimum_elapsed_ns),
                str(group.maximum_elapsed_ns),
                group.median_elapsed_ns,
                group.mad_elapsed_ns,
                str(group.iqr_elapsed_ns),
            )
        )
        + " |"
        for group in report.groups
    )
    lines.extend(
        (
            "",
            "## Throughput statistics",
            "",
            (
                "| Boundary | Workload | Dimensions | Workload SHA-256 | "
                "Commands/s min | Commands/s max | "
                "Commands/s median | Commands/s MAD | Commands/s IQR | Events/s min | "
                "Events/s max | Events/s median | Events/s MAD | Events/s IQR |"
            ),
            "|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        )
    )
    for group in report.groups:
        lines.append(
            "| "
            + " | ".join(
                (
                    group.boundary,
                    group.workload_id,
                    _parameter_text(group.measurement_parameters),
                    group.workload_sha256,
                    group.minimum_commands_per_second or "n/a",
                    group.maximum_commands_per_second or "n/a",
                    group.median_commands_per_second or "n/a",
                    group.mad_commands_per_second or "n/a",
                    group.iqr_commands_per_second or "n/a",
                    group.minimum_events_per_second or "n/a",
                    group.maximum_events_per_second or "n/a",
                    group.median_events_per_second or "n/a",
                    group.mad_events_per_second or "n/a",
                    group.iqr_events_per_second or "n/a",
                )
            )
            + " |"
        )
    lines.extend(
        (
            "",
            "## Closed-loop core service-time latency",
            "",
            (
                "| Boundary | Workload | Dimensions | Workload SHA-256 | "
                "Samples | Min ns | Max ns | "
                "p50 ns | p90 ns | p95 ns | p99 ns | p99.9 ns |"
            ),
            "|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|",
        )
    )
    for group in report.groups:
        quantiles = dict(group.latency_quantiles_ns)
        lines.append(
            "| "
            + " | ".join(
                (
                    group.boundary,
                    group.workload_id,
                    _parameter_text(group.measurement_parameters),
                    group.workload_sha256,
                    str(group.latency_sample_count),
                    "n/a" if group.minimum_latency_ns is None else str(group.minimum_latency_ns),
                    "n/a" if group.maximum_latency_ns is None else str(group.maximum_latency_ns),
                    *(str(quantiles.get(name, "n/a")) for name, _, _ in _QUANTILES),
                )
            )
            + " |"
        )
    lines.extend(
        (
            "",
            "## Process memory and allocation statistics",
            "",
            (
                "| Boundary | Workload | Dimensions | Workload SHA-256 | "
                "Metric | Min | Max | Median | "
                "MAD | IQR |"
            ),
            "|---|---|---|---|---|---:|---:|---:|---:|---:|",
        )
    )
    for group in report.groups:
        metrics = (
            ("peak RSS bytes", group.peak_rss_bytes),
            ("process RSS delta bytes", group.process_rss_delta_bytes),
            (
                "process RSS delta bytes/command",
                group.process_rss_delta_bytes_per_command,
            ),
            (
                "process RSS delta bytes/resting order",
                group.process_rss_delta_bytes_per_resting_order,
            ),
            ("allocation count", group.allocation_count),
            ("deallocation count", group.deallocation_count),
            ("allocated bytes", group.allocated_bytes),
            ("live bytes", group.live_bytes),
            ("peak live bytes", group.peak_live_bytes),
            ("allocations/command", group.allocations_per_command),
        )
        for name, statistics in metrics:
            values = (
                ("n/a",) * 5
                if statistics is None
                else (
                    statistics.minimum,
                    statistics.maximum,
                    statistics.median,
                    statistics.mad,
                    statistics.iqr,
                )
            )
            lines.append(
                "| "
                + " | ".join(
                    (
                        group.boundary,
                        group.workload_id,
                        _parameter_text(group.measurement_parameters),
                        group.workload_sha256,
                        name,
                        *values,
                    )
                )
                + " |"
            )
    if report.comparisons:
        lines.extend(
            (
                "",
                "## A/B comparisons",
                "",
                (
                    "| Comparison ID | Boundary | Workload | Dimensions | "
                    "Target metric | Direction | "
                    "Qualification | Median improvement % | Baseline relative MAD % | "
                    "Candidate relative MAD % | Peak RSS change % | Valid A-B-B-A blocks |"
                ),
                "|---|---|---|---|---|---|---|---:|---:|---:|---:|---:|",
            )
        )
        for comparison in report.comparisons:
            lines.append(
                "| "
                + " | ".join(
                    (
                        comparison.comparison_id,
                        comparison.boundary,
                        comparison.workload_id,
                        _parameter_text(comparison.measurement_parameters),
                        comparison.target_metric,
                        comparison.direction,
                        comparison.classification,
                        comparison.target_median_change_percent,
                        comparison.baseline_relative_mad_percent,
                        comparison.candidate_relative_mad_percent,
                        comparison.peak_rss_change_percent or "n/a",
                        str(comparison.abba_blocks),
                    )
                )
                + " |"
            )
    if report.experiments:
        lines.extend(
            (
                "",
                "## Experiment decisions",
                "",
                (
                    "| Experiment | Policy | Qualification | Threshold | Decision | Target "
                    "improvement % | Noise gate % | Worst control % | Worst peak RSS % | "
                    "Correctness | Complexity | Note |"
                ),
                "|---|---|---|---|---|---:|---:|---:|---:|---|---|---|",
            )
        )
        for experiment in report.experiments:
            lines.append(
                "| "
                + " | ".join(
                    (
                        experiment.experiment_id,
                        experiment.policy,
                        experiment.classification,
                        experiment.threshold_result,
                        experiment.decision,
                        experiment.target_median_change_percent or "n/a",
                        experiment.noise_gate_percent or "n/a",
                        experiment.worst_control_change_percent or "n/a",
                        experiment.worst_peak_rss_change_percent or "n/a",
                        experiment.correctness_gate,
                        experiment.complexity_gate,
                        experiment.note_path,
                    )
                )
                + " |"
            )
    if report.limitations:
        lines.extend(("", "## Limitations", ""))
        lines.extend(f"- {limitation}" for limitation in report.limitations)
    return "\n".join(lines) + "\n"


def render_report_svg(report: BenchmarkReport) -> str:
    """Render deterministic, separately scaled median evidence panels."""

    panels: tuple[tuple[str, tuple[tuple[GroupStatistics, str | int], ...]], ...] = (
        (
            "Median commands per second",
            tuple(
                (group, group.median_commands_per_second or "0")
                for group in report.groups
                if group.median_commands_per_second is not None
            ),
        ),
        (
            "Median p50 service time in nanoseconds",
            tuple(
                (group, dict(group.latency_quantiles_ns)["p50"])
                for group in report.groups
                if group.latency_quantiles_ns
            ),
        ),
        (
            "Median process RSS delta bytes per resting order",
            tuple(
                (group, group.process_rss_delta_bytes_per_resting_order.median)
                for group in report.groups
                if group.process_rss_delta_bytes_per_resting_order is not None
            ),
        ),
        (
            "Median allocations per command",
            tuple(
                (group, group.allocations_per_command.median)
                for group in report.groups
                if group.allocations_per_command is not None
            ),
        ),
    )
    nonempty_panels = tuple(panel for panel in panels if panel[1])
    width = 1_400
    row_height = 34
    panel_header_height = 38
    top = 72
    bottom = 28
    row_count = sum(len(rows) for _, rows in nonempty_panels)
    height = (
        top
        + bottom
        + panel_header_height * max(1, len(nonempty_panels))
        + row_height * max(1, row_count)
    )
    plot_left = 700
    plot_width = 640
    elements = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        (
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" '
            f'height="{height}" viewBox="0 0 {width} {height}">'
        ),
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        '<text x="20" y="30" font-family="sans-serif" font-size="18">'
        "AtlasLOB benchmark scaling evidence</text>",
        f'<text x="20" y="50" font-family="sans-serif" font-size="12">'
        f"Qualification: {escape(report.classification.upper())}</text>",
    ]
    if not nonempty_panels:
        elements.append(
            '<text x="20" y="82" font-family="sans-serif" font-size="12">'
            "No plottable groups are present.</text>"
        )
    y = top
    for title, rows in nonempty_panels:
        elements.append(
            f'<text x="20" y="{y + 22}" font-family="sans-serif" '
            f'font-size="14">{escape(title)}</text>'
        )
        y += panel_header_height
        with localcontext() as context:
            context.prec = 80
            values = tuple(Decimal(str(value)) for _, value in rows)
            maximum = max((abs(value) for value in values), default=Decimal(1))
            if maximum <= 0:
                maximum = Decimal(1)
        for group, value in rows:
            numeric = Decimal(str(value))
            with localcontext() as context:
                context.prec = 80
                bar_width = int((abs(numeric) * plot_width / maximum).to_integral_value())
            label = escape(
                f"{group.boundary}/{group.workload_id} "
                f"[{_parameter_text(group.measurement_parameters)}] "
                f"/{group.workload_sha256[:8]} "
                f"bin={group.binary_sha256[:8]} invalid={group.invalid_observations}"
            )
            fill = "#a33a3a" if numeric < 0 else "#2457c5"
            elements.extend(
                (
                    f'<text x="20" y="{y + 19}" font-family="monospace" '
                    f'font-size="12">{label}</text>',
                    f'<rect x="{plot_left}" y="{y + 4}" width="{bar_width}" '
                    f'height="20" fill="{fill}"/>',
                    f'<text x="{plot_left + bar_width + 6}" y="{y + 19}" '
                    f'font-family="monospace" font-size="12">{escape(str(value))}</text>',
                )
            )
            y += row_height
    elements.append("</svg>")
    return "\n".join(elements) + "\n"


def _group_statistics(
    key: tuple[
        str,
        str,
        str | None,
        tuple[tuple[str, str], ...],
        str,
        str,
        str,
        str,
    ],
    observations: tuple[Observation, ...],
    workload: WorkloadManifest,
) -> GroupStatistics:
    (
        boundary,
        timed_input_kind,
        timed_input_sha256,
        measurement_parameters,
        workload_id,
        workload_sha256,
        workload_manifest_sha256,
        binary_sha256,
    ) = key
    valid = tuple(item for item in observations if item.valid)
    resting_order_denominator = _resting_order_denominator(workload, boundary)
    if not valid:
        return _empty_group(key, len(observations), resting_order_denominator)
    commands = {item.commands for item in valid}
    if len(commands) != 1:
        raise ValueError("group observations have mismatched command counts")
    elapsed = tuple(item.elapsed_ns for item in valid)
    latency_samples = tuple(
        sample for item in valid for sample in (() if item.latency_ns is None else item.latency_ns)
    )
    if boundary in {
        "core_allocation",
        "core_latency",
        "core_construction",
        "core_setup_allocation",
    }:
        command_rates: tuple[Fraction, ...] = ()
        event_rates: tuple[Fraction, ...] = ()
    else:
        if any(value == 0 for value in elapsed):
            raise ValueError("timing observation has zero elapsed_ns")
        command_rates = tuple(
            Fraction(item.commands * 1_000_000_000, item.elapsed_ns) for item in valid
        )
        event_rates = tuple(
            Fraction(item.events * 1_000_000_000, item.elapsed_ns) for item in valid
        )
    command_statistics = _rate_statistics(command_rates)
    event_statistics = _rate_statistics(event_rates)
    quantiles = (
        ()
        if not latency_samples
        else tuple(
            (name, nearest_rank(latency_samples, numerator, denominator))
            for name, numerator, denominator in _QUANTILES
        )
    )
    command_count = next(iter(commands))
    if command_count == 0 and boundary != "core_construction":
        raise ValueError("valid workload-bound observations must process commands")
    peak_rss = _scalar_statistics(tuple(Fraction(item.peak_rss_bytes) for item in valid))
    rss_deltas = tuple(Fraction(item.rss_after_bytes - item.rss_before_bytes) for item in valid)
    rss_delta = _scalar_statistics(rss_deltas)
    rss_delta_per_command = (
        None
        if command_count == 0
        else _scalar_statistics(tuple(value / command_count for value in rss_deltas))
    )
    allocation_payloads = tuple(item.allocations for item in valid if item.allocations is not None)
    allocation_boundary = boundary in {"core_allocation", "core_setup_allocation"}
    if allocation_boundary and len(allocation_payloads) != len(valid):
        raise ValueError("allocation group contains a missing allocation payload")

    def allocation_statistics(values: tuple[int, ...]) -> ScalarStatistics | None:
        if not allocation_boundary:
            return None
        return _scalar_statistics(tuple(Fraction(value) for value in values))

    return GroupStatistics(
        boundary=boundary,
        timed_input_kind=timed_input_kind,
        timed_input_sha256=timed_input_sha256,
        measurement_parameters=measurement_parameters,
        workload_id=workload_id,
        workload_sha256=workload_sha256,
        workload_manifest_sha256=workload_manifest_sha256,
        binary_sha256=binary_sha256,
        valid_observations=len(valid),
        invalid_observations=len(observations) - len(valid),
        commands=command_count,
        resting_order_denominator=resting_order_denominator,
        minimum_elapsed_ns=min(elapsed),
        maximum_elapsed_ns=max(elapsed),
        median_elapsed_ns=_fraction_text(median(elapsed)),
        mad_elapsed_ns=_fraction_text(median_absolute_deviation(elapsed)),
        iqr_elapsed_ns=interquartile_range(elapsed),
        minimum_commands_per_second=command_statistics[0],
        maximum_commands_per_second=command_statistics[1],
        median_commands_per_second=command_statistics[2],
        mad_commands_per_second=command_statistics[3],
        iqr_commands_per_second=command_statistics[4],
        minimum_events_per_second=event_statistics[0],
        maximum_events_per_second=event_statistics[1],
        median_events_per_second=event_statistics[2],
        mad_events_per_second=event_statistics[3],
        iqr_events_per_second=event_statistics[4],
        latency_sample_count=len(latency_samples),
        minimum_latency_ns=None if not latency_samples else min(latency_samples),
        maximum_latency_ns=None if not latency_samples else max(latency_samples),
        latency_quantiles_ns=quantiles,
        peak_rss_bytes=peak_rss,
        process_rss_delta_bytes=rss_delta,
        process_rss_delta_bytes_per_command=rss_delta_per_command,
        process_rss_delta_bytes_per_resting_order=(
            None
            if resting_order_denominator == 0
            else _scalar_statistics(
                tuple(value / resting_order_denominator for value in rss_deltas)
            )
        ),
        allocation_count=allocation_statistics(
            tuple(item.allocation_count for item in allocation_payloads)
        ),
        deallocation_count=allocation_statistics(
            tuple(item.deallocation_count for item in allocation_payloads)
        ),
        allocated_bytes=allocation_statistics(
            tuple(item.allocated_bytes for item in allocation_payloads)
        ),
        live_bytes=allocation_statistics(tuple(item.live_bytes for item in allocation_payloads)),
        peak_live_bytes=allocation_statistics(
            tuple(item.peak_live_bytes for item in allocation_payloads)
        ),
        allocations_per_command=(
            None
            if not allocation_boundary
            else _scalar_statistics(
                tuple(
                    Fraction(item.allocation_count, command_count) for item in allocation_payloads
                )
            )
        ),
    )


def _empty_group(
    key: tuple[
        str,
        str,
        str | None,
        tuple[tuple[str, str], ...],
        str,
        str,
        str,
        str,
    ],
    invalid_count: int,
    resting_order_denominator: int,
) -> GroupStatistics:
    (
        boundary,
        timed_input_kind,
        timed_input_sha256,
        measurement_parameters,
        workload_id,
        workload_sha256,
        workload_manifest_sha256,
        binary_sha256,
    ) = key
    return GroupStatistics(
        boundary=boundary,
        timed_input_kind=timed_input_kind,
        timed_input_sha256=timed_input_sha256,
        measurement_parameters=measurement_parameters,
        workload_id=workload_id,
        workload_sha256=workload_sha256,
        workload_manifest_sha256=workload_manifest_sha256,
        binary_sha256=binary_sha256,
        valid_observations=0,
        invalid_observations=invalid_count,
        commands=0,
        resting_order_denominator=resting_order_denominator,
        minimum_elapsed_ns=0,
        maximum_elapsed_ns=0,
        median_elapsed_ns="0",
        mad_elapsed_ns="0",
        iqr_elapsed_ns=0,
        minimum_commands_per_second=None,
        maximum_commands_per_second=None,
        median_commands_per_second=None,
        mad_commands_per_second=None,
        iqr_commands_per_second=None,
        minimum_events_per_second=None,
        maximum_events_per_second=None,
        median_events_per_second=None,
        mad_events_per_second=None,
        iqr_events_per_second=None,
        latency_sample_count=0,
        minimum_latency_ns=None,
        maximum_latency_ns=None,
        latency_quantiles_ns=(),
        peak_rss_bytes=None,
        process_rss_delta_bytes=None,
        process_rss_delta_bytes_per_command=None,
        process_rss_delta_bytes_per_resting_order=None,
        allocation_count=None,
        deallocation_count=None,
        allocated_bytes=None,
        live_bytes=None,
        peak_live_bytes=None,
        allocations_per_command=None,
    )


def _resting_order_denominator(workload: WorkloadManifest, boundary: str) -> int:
    if boundary == "core_construction":
        return 0
    if boundary in {"core_preload", "core_setup_allocation"}:
        return workload.after_preload_active_order_count
    return workload.measured_start_active_order_count


def _rate_statistics(
    values: Sequence[Fraction],
) -> tuple[str | None, str | None, str | None, str | None, str | None]:
    if not values:
        return (None, None, None, None, None)
    ordered = tuple(sorted(values))
    center = _fraction_median(ordered)
    deviations = tuple(abs(value - center) for value in ordered)
    lower = ordered[(len(ordered) - 1) // 4]
    upper = ordered[(3 * len(ordered) - 1) // 4]
    return (
        _fraction_text(ordered[0]),
        _fraction_text(ordered[-1]),
        _fraction_text(center),
        _fraction_text(_fraction_median(deviations)),
        _fraction_text(upper - lower),
    )


def _scalar_statistics(values: Sequence[Fraction]) -> ScalarStatistics:
    if not values:
        raise ValueError("scalar statistics require at least one value")
    ordered = tuple(sorted(values))
    center = _fraction_median(ordered)
    deviations = tuple(abs(value - center) for value in ordered)
    lower = ordered[(len(ordered) - 1) // 4]
    upper = ordered[(3 * len(ordered) - 1) // 4]
    return ScalarStatistics(
        minimum=_fraction_text(ordered[0]),
        maximum=_fraction_text(ordered[-1]),
        median=_fraction_text(center),
        mad=_fraction_text(_fraction_median(deviations)),
        iqr=_fraction_text(upper - lower),
    )


def _comparisons(
    groups: tuple[GroupStatistics, ...],
    observations: Sequence[Observation],
    environments: dict[str, EnvironmentManifest],
) -> tuple[Comparison, ...]:
    grouped: dict[
        tuple[
            str,
            str,
            str | None,
            tuple[tuple[str, str], ...],
            str,
            str,
            str,
        ],
        list[GroupStatistics],
    ] = defaultdict(list)
    for group in groups:
        grouped[
            (
                group.boundary,
                group.timed_input_kind,
                group.timed_input_sha256,
                group.measurement_parameters,
                group.workload_id,
                group.workload_sha256,
                group.workload_manifest_sha256,
            )
        ].append(group)
    output: list[Comparison] = []
    for (
        boundary,
        timed_input_kind,
        timed_input_sha256,
        measurement_parameters,
        workload_id,
        workload_digest,
        workload_manifest_digest,
    ), candidates in sorted(grouped.items()):
        if len(candidates) != 2:
            continue
        relevant = tuple(
            item
            for item in observations
            if item.boundary == boundary
            and item.timed_input_kind == timed_input_kind
            and item.timed_input_sha256 == timed_input_sha256
            and item.measurement_parameters == measurement_parameters
            and item.workload_id == workload_id
            and item.workload_sha256 == workload_digest
            and item.workload_manifest_sha256 == workload_manifest_digest
        )
        roles: dict[str, str] = {}
        for item in relevant:
            if item.variant != "standalone":
                previous = roles.setdefault(item.binary_sha256, item.variant)
                if previous != item.variant:
                    raise ValueError("one binary appears under multiple A/B roles")
        baseline = next(
            (item for item in candidates if roles.get(item.binary_sha256) == "baseline"),
            None,
        )
        candidate = next(
            (item for item in candidates if roles.get(item.binary_sha256) == "candidate"),
            None,
        )
        if baseline is None or candidate is None:
            continue
        complete, abba_blocks = _complete_abba_observations(
            relevant,
            baseline.binary_sha256,
            candidate.binary_sha256,
        )
        if not complete:
            continue
        baseline_observations = tuple(
            item for item in complete if item.binary_sha256 == baseline.binary_sha256
        )
        candidate_observations = tuple(
            item for item in complete if item.binary_sha256 == candidate.binary_sha256
        )
        baseline_values = tuple(
            _comparison_metric_value(boundary, item) for item in baseline_observations
        )
        candidate_values = tuple(
            _comparison_metric_value(boundary, item) for item in candidate_observations
        )
        baseline_center = _fraction_median(baseline_values)
        candidate_center = _fraction_median(candidate_values)
        target_metric, direction = TARGET_METRIC_BY_BOUNDARY[boundary]
        change = _improvement_percent(baseline_center, candidate_center, direction)
        baseline_rss = _fraction_median(
            tuple(Fraction(item.peak_rss_bytes) for item in baseline_observations)
        )
        candidate_rss = _fraction_median(
            tuple(Fraction(item.peak_rss_bytes) for item in candidate_observations)
        )
        rss_change = _raw_percent_change(baseline_rss, candidate_rss)
        baseline_qualification = {
            environments[item.environment_sha256].classification
            for item in relevant
            if item.binary_sha256 == baseline.binary_sha256
        }
        candidate_qualification = {
            environments[item.environment_sha256].classification
            for item in relevant
            if item.binary_sha256 == candidate.binary_sha256
        }
        if len(baseline_qualification) != 1 or len(candidate_qualification) != 1:
            raise ValueError("one A/B binary uses mixed environment qualifications")
        comparison_classification = (
            "official"
            if baseline_qualification == candidate_qualification == {"official"}
            and abba_blocks >= 5
            else "exploratory"
        )
        output.append(
            Comparison(
                comparison_id="",
                boundary=boundary,
                timed_input_kind=timed_input_kind,
                timed_input_sha256=timed_input_sha256,
                measurement_parameters=measurement_parameters,
                workload_id=workload_id,
                workload_sha256=workload_digest,
                workload_manifest_sha256=workload_manifest_digest,
                host_context_sha256=complete[0].host_context_sha256,
                baseline_binary_sha256=baseline.binary_sha256,
                candidate_binary_sha256=candidate.binary_sha256,
                target_metric=target_metric,
                direction=direction,
                classification=comparison_classification,
                target_median_change_percent=_fraction_text(change, precision=6),
                baseline_relative_mad_percent=_relative_metric_mad(baseline_values),
                candidate_relative_mad_percent=_relative_metric_mad(candidate_values),
                peak_rss_change_percent=(
                    None if rss_change is None else _fraction_text(rss_change, precision=6)
                ),
                abba_blocks=abba_blocks,
            )
        )
    return tuple(sorted(output, key=lambda item: item.comparison_id))


def _comparison_metric_value(boundary: str, value: Observation) -> Fraction:
    metric, _ = TARGET_METRIC_BY_BOUNDARY[boundary]
    if metric == "commands_per_second":
        if value.elapsed_ns == 0:
            raise ValueError("throughput comparison contains zero elapsed time")
        return Fraction(value.commands * 1_000_000_000, value.elapsed_ns)
    if metric == "elapsed_ns":
        return Fraction(value.elapsed_ns)
    if metric == "p99_ns":
        if value.latency_ns is None or not value.latency_ns:
            raise ValueError("latency comparison contains no samples")
        return Fraction(nearest_rank(value.latency_ns, 99, 100))
    if metric == "allocation_count":
        if value.allocations is None:
            raise ValueError("allocation comparison contains no allocation metrics")
        return Fraction(value.allocations.allocation_count)
    raise AssertionError("unhandled comparison metric")


def _relative_metric_mad(values: Sequence[Fraction]) -> str:
    center = _fraction_median(values)
    if center == 0:
        return "0"
    mad = _fraction_median(tuple(abs(value - center) for value in values))
    return _fraction_text(mad * 100 / abs(center), precision=6)


def _improvement_percent(
    baseline: Fraction,
    candidate: Fraction,
    direction: str,
) -> Fraction:
    if baseline == 0:
        return Fraction(0)
    delta = candidate - baseline
    if direction == "lower_is_better":
        delta = -delta
    return delta * 100 / abs(baseline)


def _raw_percent_change(baseline: Fraction, candidate: Fraction) -> Fraction | None:
    if baseline == 0:
        return None
    return (candidate - baseline) * 100 / abs(baseline)


def _complete_abba_observations(
    observations: Sequence[Observation],
    baseline_binary: str,
    candidate_binary: str,
) -> tuple[tuple[Observation, ...], int]:
    blocks: dict[int, dict[int, Observation]] = defaultdict(dict)
    for item in observations:
        if item.variant == "standalone":
            raise ValueError("standalone and A/B observations cannot be mixed")
        if item.block_position in blocks[item.block_index]:
            raise ValueError("A/B block contains a duplicate position")
        blocks[item.block_index][item.block_position] = item
    expected = (baseline_binary, candidate_binary, candidate_binary, baseline_binary)
    included: list[Observation] = []
    valid_blocks = 0
    for index, positions in sorted(blocks.items()):
        if tuple(sorted(positions)) != (1, 2, 3, 4):
            continue
        actual = tuple(positions[position].binary_sha256 for position in range(1, 5))
        if actual != expected:
            raise ValueError(f"A/B block {index} does not follow A-B-B-A")
        if all(item.valid for item in positions.values()):
            valid_blocks += 1
            included.extend(positions[position] for position in range(1, 5))
    return tuple(included), valid_blocks


def _has_excluded_abba_blocks(observations: Sequence[Observation]) -> bool:
    points: dict[tuple[object, ...], dict[int, dict[int, Observation]]] = defaultdict(
        lambda: defaultdict(dict)
    )
    for item in observations:
        if item.variant == "standalone":
            continue
        point = (
            item.boundary,
            item.timed_input_kind,
            item.timed_input_sha256,
            item.measurement_parameters,
            item.workload_id,
            item.workload_sha256,
            item.workload_manifest_sha256,
        )
        positions = points[point][item.block_index]
        if item.block_position in positions:
            raise ValueError("A/B evidence duplicates one block position")
        positions[item.block_position] = item
    return any(
        tuple(sorted(positions)) != (1, 2, 3, 4)
        or not all(item.valid for item in positions.values())
        for blocks in points.values()
        for positions in blocks.values()
    )


def _experiments(
    comparisons: tuple[Comparison, ...],
    plans: Sequence[ExperimentPlan],
) -> tuple[Experiment, ...]:
    by_id = {item.comparison_id: item for item in comparisons}
    output: list[Experiment] = []
    for plan in plans:
        if plan.policy not in {"general", "capacity_reservation"}:
            raise ValueError("experiment policy is invalid")
        if plan.correctness_gate not in {"passed", "failed", "not_run"}:
            raise ValueError("experiment correctness gate is invalid")
        if plan.complexity_gate not in {"passed", "failed", "not_run"}:
            raise ValueError("experiment complexity gate is invalid")
        if plan.target_comparison_id is None:
            if plan.control_comparison_ids:
                raise ValueError("deferred experiment cannot reference control comparisons")
            if plan.correctness_gate != "not_run" or plan.complexity_gate != "not_run":
                raise ValueError("deferred experiment gates must be not_run")
            output.append(
                Experiment(
                    experiment_id=plan.experiment_id,
                    policy=plan.policy,
                    classification="not_run",
                    target_comparison_id=None,
                    control_comparison_ids=(),
                    threshold_result="not_run",
                    decision="deferred",
                    target_median_change_percent=None,
                    noise_gate_percent=None,
                    worst_control_change_percent=None,
                    worst_peak_rss_change_percent=None,
                    correctness_gate=plan.correctness_gate,
                    complexity_gate=plan.complexity_gate,
                    note_path=plan.note_path,
                    note_sha256=plan.note_sha256,
                    rationale=plan.rationale,
                )
            )
            continue
        if not plan.control_comparison_ids:
            raise ValueError("measured experiment requires at least one control comparison")
        if len(plan.control_comparison_ids) != len(set(plan.control_comparison_ids)):
            raise ValueError("experiment control comparison IDs must be unique")
        if plan.target_comparison_id in plan.control_comparison_ids:
            raise ValueError("experiment target cannot also be a control")
        if "not_run" in {plan.correctness_gate, plan.complexity_gate}:
            raise ValueError("measured experiment gates cannot be not_run")
        target = by_id.get(plan.target_comparison_id)
        controls = tuple(by_id.get(item) for item in plan.control_comparison_ids)
        if target is None or any(item is None for item in controls):
            raise ValueError("experiment plan references a missing comparison")
        typed_controls = tuple(item for item in controls if item is not None)
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
            if any(item.peak_rss_change_percent is None for item in (target, *typed_controls)):
                raise ValueError("experiment plan references comparison without RSS evidence")
            worst_rss = max(
                Decimal(item.peak_rss_change_percent or "0") for item in (target, *typed_controls)
            )
            rss_passed = (
                worst_rss <= Decimal(10)
                if plan.policy == "general"
                else worst_rss <= Decimal(10)
                or (worst_rss <= Decimal(20) and target_change >= Decimal(10))
            )
            threshold_passed = (
                target_change >= Decimal(5)
                and target_change > noise
                and worst_control >= Decimal(-5)
                and rss_passed
            )
        classification = (
            "official"
            if all(item.classification == "official" for item in (target, *typed_controls))
            else "exploratory"
        )
        gates_passed = plan.correctness_gate == "passed" and plan.complexity_gate == "passed"
        if threshold_passed and classification == "official" and gates_passed:
            decision = "accepted"
        elif (
            target_change < 0
            or worst_control < Decimal(-5)
            or not rss_passed
            or plan.correctness_gate == "failed"
            or plan.complexity_gate == "failed"
        ):
            decision = "rejected"
        else:
            decision = "neutral"
        output.append(
            Experiment(
                experiment_id=plan.experiment_id,
                policy=plan.policy,
                classification=classification,
                target_comparison_id=target.comparison_id,
                control_comparison_ids=tuple(sorted(plan.control_comparison_ids)),
                threshold_result="passed" if threshold_passed else "failed",
                decision=decision,
                target_median_change_percent=target.target_median_change_percent,
                noise_gate_percent=_decimal_text(noise, precision=6),
                worst_control_change_percent=_decimal_text(worst_control, precision=6),
                worst_peak_rss_change_percent=_decimal_text(worst_rss, precision=6),
                correctness_gate=plan.correctness_gate,
                complexity_gate=plan.complexity_gate,
                note_path=plan.note_path,
                note_sha256=plan.note_sha256,
                rationale=plan.rationale,
            )
        )
    return tuple(sorted(output, key=lambda item: item.experiment_id))


def _fraction_median(values: Sequence[Fraction]) -> Fraction:
    if not values:
        raise ValueError("median requires at least one value")
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2


def _fraction_text(value: Fraction, *, precision: int = 6) -> str:
    if value.denominator == 1:
        return str(value.numerator)
    with localcontext() as context:
        context.prec = 80
        decimal = Decimal(value.numerator) / Decimal(value.denominator)
    return _decimal_text(decimal, precision=precision)


def _decimal_text(value: Decimal, *, precision: int) -> str:
    with localcontext() as context:
        context.prec = 80
        quantum = Decimal(1).scaleb(-precision)
        text = format(value.quantize(quantum), "f").rstrip("0").rstrip(".")
    return "0" if text in {"", "-0"} else text


def _unique_json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate experiment plan key: {key}")
        value[key] = item
    return value


def _required_string(value: dict[str, object], name: str) -> str:
    item = value[name]
    if not isinstance(item, str):
        raise ValueError("experiment plan text fields must be strings")
    return item


def _reject_json_number(value: str) -> object:
    raise ValueError(f"JSON numbers are forbidden in experiment plans: {value}")
