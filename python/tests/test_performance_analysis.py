from __future__ import annotations

from dataclasses import replace
from pathlib import Path

import pytest
from atlaslob.performance.analysis import (
    EXPERIMENT_PLAN_SCHEMA,
    ExperimentPlan,
    analyze_observations,
    interquartile_range,
    load_experiment_plans,
    median,
    median_absolute_deviation,
    nearest_rank,
    render_report_markdown,
    render_report_svg,
)
from atlaslob.performance.schemas import (
    AllocationMetrics,
    EnvironmentManifest,
    Observation,
    WorkloadManifest,
    canonical_json_bytes,
    document_sha256,
    environment_to_dict,
    parse_canonical_document,
    report_from_dict,
    report_to_dict,
    workload_measurement_parameters,
    workload_to_dict,
)
from atlaslob.performance.workloads import materialize_workload


def _environment(binary: str) -> EnvironmentManifest:
    return EnvironmentManifest(
        commit="1" * 40,
        tag=None,
        dirty=False,
        binary_sha256=binary,
        os="Ubuntu 24.04.2 LTS",
        kernel="6.8.0",
        host_class="analysis-host",
        cpu_model="Example CPU",
        architecture="x86_64",
        physical_cores=8,
        logical_cpus=16,
        microcode="0x1",
        memory_bytes=64 * 1024**3,
        compiler="clang 18.1.8",
        compiler_flags=("-O3", "-DNDEBUG", "-fno-omit-frame-pointer"),
        build_receipt_sha256="f" * 64,
        build_target_profiles=(("atlas_bench_runner.0000", "-O3 -DNDEBUG"),),
        build_type="Release",
        optimization="O3",
        ndebug=True,
        frame_pointers=True,
        invariants=False,
        sanitizers=False,
        lto=False,
        benchmark_build=True,
        warnings_as_errors=True,
        debug_symbols=True,
        cxx20=True,
        release_flags_locked=True,
        affinity=(2,),
        pinned_cpu=2,
        smt_sibling_idle=True,
        numa_nodes=1,
        numa_cpu_policy="2",
        numa_memory_policy="0",
        filesystem="ext4",
        storage_class="local-nvme",
        governor="performance",
        turbo="enabled",
        smt="enabled",
        virtualization="none",
        perf_version="perf version 6.8",
        runtime_kind="native",
        python_implementation=None,
        python_version=None,
        python_cache_tag=None,
        atlaslob_version=None,
        interpreter_sha256=None,
        wheel_sha256=None,
        package_sha256=None,
        wrapper_sha256=None,
        harness_sha256=None,
        classification="exploratory",
        host_context_sha256="",
        limitations=("synthetic test environment",),
    )


def _observation(
    manifest: WorkloadManifest,
    environment: EnvironmentManifest,
    environment_digest: str,
    *,
    elapsed_ns: int,
    run_label: str,
    variant: str = "standalone",
    block_index: int = 0,
    block_position: int = 0,
    boundary: str = "core_throughput",
    latency: tuple[int, ...] | None = None,
    allocations: AllocationMetrics | None = None,
    valid: bool = True,
) -> Observation:
    return Observation(
        boundary=boundary,
        timed_input_kind="none",
        timed_input_sha256=None,
        measurement_parameters=workload_measurement_parameters(manifest),
        workload_id=manifest.workload_id,
        workload_sha256=manifest.stream_sha256,
        workload_manifest_sha256=document_sha256(workload_to_dict(manifest)),
        binary_sha256=environment.binary_sha256,
        environment_sha256=environment_digest,
        host_context_sha256=environment.host_context_sha256,
        suite_label="analysis01",
        run_label=run_label,
        variant=variant,
        block_index=block_index,
        block_position=block_position,
        preload_commands=manifest.preload_commands,
        warmup_commands=manifest.warmup_commands,
        commands=manifest.measured_commands if valid else 0,
        events=manifest.expected_events if valid else 0,
        committed=manifest.expected_committed if valid else 0,
        rejected=manifest.expected_rejected if valid else 0,
        engine_errors=manifest.expected_engine_errors if valid else 0,
        elapsed_ns=elapsed_ns,
        rss_before_bytes=1_000,
        rss_after_bytes=2_000,
        peak_rss_bytes=2_500,
        latency_ns=latency,
        allocations=allocations,
        event_digest=manifest.expected_event_digest if valid else "0" * 64,
        final_digest=manifest.expected_final_digest if valid else "0" * 64,
        valid=valid,
        failure_reason=None if valid else "retained runner failure",
    )


def _w04(tmp_path: Path) -> WorkloadManifest:
    return materialize_workload(
        "W04",
        tmp_path,
        seed=9,
        preload_commands=20,
        warmup_commands=20,
        measured_commands=40,
        active_order_target=20,
    )[1]


def _manifest_entry(manifest: WorkloadManifest) -> tuple[str, WorkloadManifest]:
    return document_sha256(workload_to_dict(manifest)), manifest


def test_integer_statistics_use_deterministic_nearest_rank_rules() -> None:
    values = (1, 2, 3, 4, 100)
    assert median(values) == 3
    assert median_absolute_deviation(values) == 1
    assert nearest_rank(values, 999, 1_000) == 100
    assert interquartile_range(values) == 2


def test_abba_comparison_uses_explicit_roles_not_lexicographic_hashes(
    tmp_path: Path,
) -> None:
    manifest = _w04(tmp_path)
    baseline = _environment("f" * 64)
    candidate = _environment("0" * 64)
    baseline_digest = document_sha256(environment_to_dict(baseline))
    candidate_digest = document_sha256(environment_to_dict(candidate))
    observations = (
        _observation(
            manifest,
            baseline,
            baseline_digest,
            elapsed_ns=200,
            run_label="baseline-1",
            variant="baseline",
            block_index=1,
            block_position=1,
        ),
        _observation(
            manifest,
            candidate,
            candidate_digest,
            elapsed_ns=100,
            run_label="candidate-1",
            variant="candidate",
            block_index=1,
            block_position=2,
        ),
        _observation(
            manifest,
            candidate,
            candidate_digest,
            elapsed_ns=100,
            run_label="candidate-2",
            variant="candidate",
            block_index=1,
            block_position=3,
        ),
        _observation(
            manifest,
            baseline,
            baseline_digest,
            elapsed_ns=200,
            run_label="baseline-2",
            variant="baseline",
            block_index=1,
            block_position=4,
        ),
    )

    report = analyze_observations(
        observations,
        workloads=(_manifest_entry(manifest),),
        environments=((baseline_digest, baseline), (candidate_digest, candidate)),
    )

    comparison = report.comparisons[0]
    assert comparison.baseline_binary_sha256 == "f" * 64
    assert comparison.candidate_binary_sha256 == "0" * 64
    assert comparison.target_median_change_percent == "100"
    assert comparison.abba_blocks == 1
    assert comparison.workload_sha256 == manifest.stream_sha256
    assert comparison.classification == "exploratory"
    encoded = canonical_json_bytes(report_to_dict(report))
    assert parse_canonical_document(encoded, report_from_dict) == report


def test_invalid_only_group_is_retained_in_report(tmp_path: Path) -> None:
    manifest = _w04(tmp_path)
    environment = _environment("a" * 64)
    environment_digest = document_sha256(environment_to_dict(environment))
    invalid = _observation(
        manifest,
        environment,
        environment_digest,
        elapsed_ns=0,
        run_label="standalone-invalid",
        valid=False,
    )

    report = analyze_observations(
        (invalid,),
        workloads=(_manifest_entry(manifest),),
        environments=((environment_digest, environment),),
    )

    assert report.groups[0].valid_observations == 0
    assert report.groups[0].invalid_observations == 1
    assert report.groups[0].median_commands_per_second is None
    assert "retained invalid observations" in " ".join(report.limitations)


def test_latency_is_service_time_only_and_throughput_has_both_rates(
    tmp_path: Path,
) -> None:
    manifest = _w04(tmp_path)
    environment = _environment("a" * 64)
    environment_digest = document_sha256(environment_to_dict(environment))
    latency = _observation(
        manifest,
        environment,
        environment_digest,
        elapsed_ns=1_000,
        run_label="standalone-latency",
        boundary="core_latency",
        latency=(10,),
    )
    throughput = _observation(
        manifest,
        environment,
        environment_digest,
        elapsed_ns=1_000,
        run_label="standalone-throughput",
    )

    report = analyze_observations(
        (latency, throughput),
        workloads=(_manifest_entry(manifest),),
        environments=((environment_digest, environment),),
    )
    groups = {group.boundary: group for group in report.groups}
    assert groups["core_latency"].median_commands_per_second is None
    assert groups["core_latency"].latency_sample_count == 1
    assert groups["core_latency"].latency_quantiles_ns[-1] == ("p99.9", 10)
    assert groups["core_throughput"].median_commands_per_second is not None
    assert groups["core_throughput"].median_events_per_second is not None
    assert groups["core_throughput"].process_rss_delta_bytes is not None
    assert groups["core_throughput"].process_rss_delta_bytes.median == "1000"
    assert groups["core_throughput"].peak_rss_bytes is not None
    assert groups["core_throughput"].peak_rss_bytes.median == "2500"
    assert groups["core_throughput"].resting_order_denominator == 20
    assert groups["core_throughput"].process_rss_delta_bytes_per_resting_order is not None
    assert groups["core_throughput"].process_rss_delta_bytes_per_resting_order.median == "50"


def test_analysis_rejects_unbound_or_mismatched_valid_evidence(tmp_path: Path) -> None:
    manifest = _w04(tmp_path)
    environment = _environment("a" * 64)
    environment_digest = document_sha256(environment_to_dict(environment))
    observation = _observation(
        manifest,
        environment,
        environment_digest,
        elapsed_ns=1_000,
        run_label="standalone",
    )

    with pytest.raises(ValueError, match="missing workload or environment"):
        analyze_observations(
            (observation,),
            workloads=(),
            environments=((environment_digest, environment),),
        )
    with pytest.raises(ValueError, match="expected workload"):
        analyze_observations(
            (replace(observation, final_digest="9" * 64),),
            workloads=(_manifest_entry(manifest),),
            environments=((environment_digest, environment),),
        )


def test_report_and_svg_regenerate_byte_for_byte(tmp_path: Path) -> None:
    manifest = _w04(tmp_path)
    environment = _environment("a" * 64)
    environment_digest = document_sha256(environment_to_dict(environment))
    observation = _observation(
        manifest,
        environment,
        environment_digest,
        elapsed_ns=1_000,
        run_label="standalone",
    )
    report = analyze_observations(
        (observation,),
        workloads=(_manifest_entry(manifest),),
        environments=((environment_digest, environment),),
    )
    encoded = canonical_json_bytes(report_to_dict(report))

    assert parse_canonical_document(encoded, report_from_dict) == report
    svg = render_report_svg(report)
    markdown = render_report_markdown(report)
    assert svg == render_report_svg(report)
    assert "instrument_count=1" in svg
    assert "instrument_count=1" in markdown
    assert markdown.endswith("\n")

    with pytest.raises(ValueError, match="groups exceed"):
        replace(report, groups=(report.groups[0],) * 65_537)


def test_svg_keeps_measurement_boundaries_in_separate_panels(tmp_path: Path) -> None:
    manifest = _w04(tmp_path)
    environment = _environment("a" * 64)
    environment_digest = document_sha256(environment_to_dict(environment))
    report = analyze_observations(
        (
            _observation(
                manifest,
                environment,
                environment_digest,
                elapsed_ns=1_000,
                run_label="throughput",
            ),
            _observation(
                manifest,
                environment,
                environment_digest,
                elapsed_ns=1_000,
                run_label="latency",
                boundary="core_latency",
                latency=(10,),
            ),
        ),
        workloads=(_manifest_entry(manifest),),
        environments=((environment_digest, environment),),
    )
    groups = {group.boundary: group for group in report.groups}
    rendered_report = replace(
        report,
        groups=tuple(
            sorted(
                (
                    groups["core_throughput"],
                    replace(
                        groups["core_throughput"],
                        boundary="replay_fast",
                        timed_input_kind="atlslg01",
                        timed_input_sha256="e" * 64,
                    ),
                    replace(groups["core_throughput"], boundary="python_summary"),
                    replace(groups["core_throughput"], boundary="core_preload"),
                    groups["core_latency"],
                ),
                key=lambda group: group.boundary,
            )
        ),
    )

    svg = render_report_svg(rendered_report)
    core_title = svg.index("Core execution median commands per second")
    replay_title = svg.index("Replay median commands per second")
    python_title = svg.index("Python batch median commands per second")
    setup_title = svg.index("Preload median commands per second")
    latency_title = svg.index("Median p50 service time in nanoseconds")
    assert core_title < svg.index("core_throughput/W04") < replay_title
    assert replay_title < svg.index("replay_fast/W04") < python_title
    assert python_title < svg.index("python_summary/W04") < setup_title
    assert setup_title < svg.index("core_preload/W04") < latency_title
    assert "core_latency/W04" not in svg[:latency_title]


def test_official_report_retains_invalid_observations_as_limitations(
    tmp_path: Path,
) -> None:
    manifest = _w04(tmp_path)
    environment = replace(_environment("a" * 64), classification="official", limitations=())
    environment_digest = document_sha256(environment_to_dict(environment))
    valid = tuple(
        _observation(
            manifest,
            environment,
            environment_digest,
            elapsed_ns=1_000 + index,
            run_label=f"official-valid-{index:02d}",
        )
        for index in range(10)
    )
    invalid = _observation(
        manifest,
        environment,
        environment_digest,
        elapsed_ns=0,
        run_label="official-invalid",
        valid=False,
    )

    report = analyze_observations(
        (*valid, invalid),
        workloads=(_manifest_entry(manifest),),
        environments=((environment_digest, environment),),
    )

    assert report.classification == "official"
    assert report.groups[0].valid_observations == 10
    assert report.groups[0].invalid_observations == 1
    assert any("retained invalid observations" in item for item in report.limitations)


def test_experiment_control_must_be_core_throughput(tmp_path: Path) -> None:
    manifest = _w04(tmp_path)
    baseline = _environment("a" * 64)
    candidate = _environment("b" * 64)
    baseline_digest = document_sha256(environment_to_dict(baseline))
    candidate_digest = document_sha256(environment_to_dict(candidate))
    observations: list[Observation] = []
    for boundary in ("core_throughput", "core_latency"):
        for block in range(1, 6):
            for position, environment, environment_digest, variant in (
                (1, baseline, baseline_digest, "baseline"),
                (2, candidate, candidate_digest, "candidate"),
                (3, candidate, candidate_digest, "candidate"),
                (4, baseline, baseline_digest, "baseline"),
            ):
                observations.append(
                    _observation(
                        manifest,
                        environment,
                        environment_digest,
                        elapsed_ns=1_000,
                        run_label=f"{boundary}-{block}-{position}",
                        variant=variant,
                        block_index=block,
                        block_position=position,
                        boundary=boundary,
                        latency=(10,) if boundary == "core_latency" else None,
                    )
                )
    base_report = analyze_observations(
        observations,
        workloads=(_manifest_entry(manifest),),
        environments=((baseline_digest, baseline), (candidate_digest, candidate)),
    )
    comparisons = {item.boundary: item for item in base_report.comparisons}
    plan = ExperimentPlan(
        experiment_id="EXP-001",
        policy="general",
        target_comparison_id=comparisons["core_throughput"].comparison_id,
        control_comparison_ids=(comparisons["core_latency"].comparison_id,),
        correctness_gate="passed",
        complexity_gate="passed",
        note_path="experiments/EXP-001.md",
        note_sha256="c" * 64,
        rationale="synthetic experiment plan",
    )

    with pytest.raises(ValueError, match="controls must be core-throughput"):
        analyze_observations(
            observations,
            workloads=(_manifest_entry(manifest),),
            environments=((baseline_digest, baseline), (candidate_digest, candidate)),
            experiment_plans=(plan,),
        )


def test_experiment_plan_loader_rejects_numbers_duplicates_and_noncanonical_order(
    tmp_path: Path,
) -> None:
    path = tmp_path / "experiments.json"
    value = {
        "schema": EXPERIMENT_PLAN_SCHEMA,
        "experiments": [
            {
                "experiment_id": "EXP-001",
                "policy": "general",
                "target_comparison_id": None,
                "control_comparison_ids": [],
                "correctness_gate": "not_run",
                "complexity_gate": "not_run",
                "note_path": "experiments/EXP-001.md",
                "note_sha256": "d" * 64,
                "rationale": "baseline did not support this hypothesis",
            }
        ],
    }
    path.write_bytes(canonical_json_bytes(value))
    plans = load_experiment_plans(path)
    assert plans[0].experiment_id == "EXP-001"

    path.write_text(
        '{"experiments":[],"schema":"ATLAS_BENCH_EXPERIMENT_PLAN_V1","extra":1}\n',
        encoding="ascii",
        newline="\n",
    )
    with pytest.raises(ValueError, match="numbers|unexpected"):
        load_experiment_plans(path)
