from __future__ import annotations

import shutil
from dataclasses import replace
from pathlib import Path

import pytest
from atlaslob.performance import campaign
from atlaslob.performance.bundle import BundleSummary, verify_bundle
from atlaslob.performance.schemas import (
    EnvironmentManifest,
    Observation,
    canonical_json_bytes,
    file_sha256,
    measurement_parameters_for_boundary,
    write_canonical_document,
)
from atlaslob.performance.suite import Runner, _run_label
from atlaslob.performance.workloads import (
    BenchmarkPlan,
    BenchmarkPlanPoint,
    benchmark_plan_to_dict,
    load_benchmark_plan,
    materialize_benchmark_plan,
    verify_workload_manifest,
)

REPOSITORY = Path(__file__).resolve().parents[2]


def _environment() -> EnvironmentManifest:
    return EnvironmentManifest(
        commit="1" * 40,
        tag=None,
        dirty=False,
        binary_sha256="a" * 64,
        os="Ubuntu 24.04.2 LTS",
        kernel="6.8.0",
        host_class="campaign-host",
        cpu_model="Example CPU",
        architecture="x86_64",
        physical_cores=8,
        logical_cpus=16,
        microcode="0x1",
        memory_bytes=64 * 1024**3,
        compiler="clang 18.1.8",
        compiler_flags=("-O3", "-DNDEBUG", "-g", "-fno-omit-frame-pointer"),
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
        numa_cpu_policy="allowed-2",
        numa_memory_policy="allowed-0",
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
        limitations=("synthetic campaign environment",),
    )


def _plan() -> BenchmarkPlan:
    return BenchmarkPlan(
        "campaign-test",
        (
            BenchmarkPlanPoint(
                "a-w04",
                "smoke",
                "W04",
                4,
                20,
                20,
                40,
                20,
                1,
                16,
                ("core_throughput",),
            ),
            BenchmarkPlanPoint(
                "b-w04",
                "smoke",
                "W04",
                5,
                20,
                20,
                40,
                20,
                1,
                16,
                ("core_throughput",),
            ),
        ),
    )


def _prepare_bundle(tmp_path: Path) -> tuple[Path, Path, EnvironmentManifest, str]:
    plan = _plan()
    tmp_path.mkdir(parents=True, exist_ok=True)
    plan_path = tmp_path / "plan.json"
    plan_path.write_bytes(canonical_json_bytes(benchmark_plan_to_dict(plan)))
    bundle = tmp_path / "bundle"
    materialize_benchmark_plan(plan, bundle / "workloads")
    environment = _environment()
    environment_path = bundle / "environments" / "core.json"
    environment_digest = write_canonical_document(environment_path, environment)
    return plan_path, bundle, environment, environment_digest


def _observation(
    manifest_path: Path,
    environment: EnvironmentManifest,
    environment_digest: str,
    *,
    run_label: str,
    valid: bool = True,
) -> Observation:
    manifest = verify_workload_manifest(manifest_path)
    return Observation(
        boundary="core_throughput",
        timed_input_kind="none",
        timed_input_sha256=None,
        measurement_parameters=measurement_parameters_for_boundary(manifest, "core_throughput"),
        workload_id=manifest.workload_id,
        workload_sha256=manifest.stream_sha256,
        workload_manifest_sha256=file_sha256(manifest_path),
        binary_sha256=environment.binary_sha256,
        environment_sha256=environment_digest,
        host_context_sha256=environment.host_context_sha256,
        suite_label="campaign01",
        run_label=run_label,
        variant="standalone",
        block_index=0,
        block_position=0,
        preload_commands=manifest.preload_commands,
        warmup_commands=manifest.warmup_commands,
        commands=manifest.measured_commands if valid else 0,
        events=manifest.expected_events if valid else 0,
        committed=manifest.expected_committed if valid else 0,
        rejected=manifest.expected_rejected if valid else 0,
        engine_errors=manifest.expected_engine_errors if valid else 0,
        elapsed_ns=1_000 if valid else 0,
        rss_before_bytes=1_000 if valid else 0,
        rss_after_bytes=1_000 if valid else 0,
        peak_rss_bytes=1_000 if valid else 0,
        latency_ns=None,
        allocations=None,
        event_digest=manifest.expected_event_digest if valid else "0" * 64,
        final_digest=manifest.expected_final_digest if valid else "0" * 64,
        valid=valid,
        failure_reason=None if valid else "injected campaign failure",
    )


def _attempt_run_label(manifest_path: Path, arguments: dict[str, object]) -> str:
    manifest = verify_workload_manifest(manifest_path)
    assert arguments["mode"] == "throughput"
    suite_label = arguments["suite_label"]
    counter = arguments["block_start"]
    assert isinstance(suite_label, str)
    assert isinstance(counter, int) and not isinstance(counter, bool)
    return _run_label(
        suite_label,
        manifest.workload_id,
        manifest.stream_sha256,
        "core_throughput",
        "standalone",
        0,
        0,
        counter,
    )


def test_study_plan_expands_to_frozen_51_shapes() -> None:
    plan = load_benchmark_plan(REPOSITORY / "benchmarks" / "plans" / "phase5-study-v1.json")
    shapes = campaign.expand_campaign(plan)

    assert len(shapes) == 51
    assert sum(shape.role == "python" for shape in shapes) == 12
    assert tuple(shape.point_id for shape in shapes) == tuple(
        sorted(shape.point_id for shape in shapes)
    )
    assert {shape.batch_size for shape in shapes if shape.role == "python"} == {
        1,
        64,
        1_024,
        65_536,
    }
    python_shapes = tuple(shape for shape in shapes if shape.role == "python")
    assert len(
        {
            (
                shape.boundary,
                campaign._campaign_attempt_counter(shape, 1),
            )
            for shape in python_shapes
        }
    ) == len(python_shapes)


def test_campaign_is_round_robin_resumable_and_retains_invalid_attempt(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    plan_path, bundle, environment, environment_digest = _prepare_bundle(tmp_path)
    schedule: list[str] = []
    invalid_first = False

    monkeypatch.setattr(
        campaign,
        "_preflight_runners",
        lambda *_: {"core": (environment.binary_sha256, environment, environment_digest)},
    )

    def fake_run_suite(
        manifest_path: Path,
        output: Path,
        **kwargs: object,
    ) -> tuple[Path, ...]:
        nonlocal invalid_first
        schedule.append(manifest_path.name)
        output.mkdir(parents=True)
        value = _observation(
            manifest_path,
            environment,
            environment_digest,
            run_label=_attempt_run_label(manifest_path, kwargs),
            valid=not invalid_first,
        )
        invalid_first = False
        path = output / "observation-00001.json"
        write_canonical_document(path, value)
        return (path,)

    monkeypatch.setattr(campaign, "run_suite", fake_run_suite)
    runners = campaign.CampaignRunners(
        Runner(Path("atlas_bench_runner"), bundle / "environments" / "core.json", "standalone")
    )
    summary = campaign.run_campaign(
        plan_path,
        bundle,
        runners=runners,
        suite_label="campaign01",
        valid_observations=2,
        max_attempts=3,
    )
    assert summary == campaign.CampaignSummary(2, 4, 4, 0)
    assert schedule[0] != schedule[1]
    assert schedule[0] == schedule[2]
    assert schedule[1] == schedule[3]

    invalid_plan_path, invalid_bundle, invalid_environment, invalid_digest = _prepare_bundle(
        tmp_path / "invalid"
    )
    environment = invalid_environment
    environment_digest = invalid_digest
    invalid_first = True
    with pytest.raises(ValueError, match="retained invalid"):
        campaign.run_campaign(
            invalid_plan_path,
            invalid_bundle,
            runners=campaign.CampaignRunners(
                Runner(
                    Path("atlas_bench_runner"),
                    invalid_bundle / "environments" / "core.json",
                    "standalone",
                )
            ),
            suite_label="campaign01",
            valid_observations=1,
            max_attempts=2,
        )
    resumed = campaign.run_campaign(
        invalid_plan_path,
        invalid_bundle,
        runners=campaign.CampaignRunners(
            Runner(
                Path("atlas_bench_runner"),
                invalid_bundle / "environments" / "core.json",
                "standalone",
            )
        ),
        suite_label="campaign01",
        valid_observations=1,
        max_attempts=2,
        resume=True,
    )
    assert resumed == campaign.CampaignSummary(2, 3, 2, 1)


def test_finalize_campaign_requires_coverage_and_verifies_bundle(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    plan_path, bundle, environment, environment_digest = _prepare_bundle(tmp_path)
    monkeypatch.setattr(
        campaign,
        "_preflight_runners",
        lambda *_: {"core": (environment.binary_sha256, environment, environment_digest)},
    )

    def fake_run_suite(
        manifest_path: Path,
        output: Path,
        **kwargs: object,
    ) -> tuple[Path, ...]:
        output.mkdir(parents=True)
        value = _observation(
            manifest_path,
            environment,
            environment_digest,
            run_label=_attempt_run_label(manifest_path, kwargs),
        )
        path = output / "observation-00001.json"
        write_canonical_document(path, value)
        return (path,)

    monkeypatch.setattr(campaign, "run_suite", fake_run_suite)
    runners = campaign.CampaignRunners(
        Runner(Path("atlas_bench_runner"), bundle / "environments" / "core.json", "standalone")
    )
    campaign.run_campaign(
        plan_path,
        bundle,
        runners=runners,
        suite_label="campaign01",
        valid_observations=1,
        max_attempts=1,
    )
    summary = campaign.finalize_campaign(
        plan_path,
        bundle,
        valid_observations=1,
        allow_exploratory=True,
    )

    assert summary.workloads == 2
    assert summary.environments == 1
    assert summary.observations == 2
    assert summary.reports == 1
    assert (bundle / "inventory.json").is_file()
    with pytest.raises(ValueError, match="immutable"):
        campaign.finalize_campaign(
            plan_path,
            bundle,
            valid_observations=1,
            allow_exploratory=True,
        )


def test_campaign_rejects_copied_attempt_before_resuming(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    plan_path, bundle, environment, environment_digest = _prepare_bundle(tmp_path)
    monkeypatch.setattr(
        campaign,
        "_preflight_runners",
        lambda *_: {"core": (environment.binary_sha256, environment, environment_digest)},
    )

    def fake_run_suite(
        manifest_path: Path,
        output: Path,
        **kwargs: object,
    ) -> tuple[Path, ...]:
        output.mkdir(parents=True)
        path = output / "observation-00001.json"
        write_canonical_document(
            path,
            _observation(
                manifest_path,
                environment,
                environment_digest,
                run_label=_attempt_run_label(manifest_path, kwargs),
            ),
        )
        return (path,)

    monkeypatch.setattr(campaign, "run_suite", fake_run_suite)
    runners = campaign.CampaignRunners(
        Runner(Path("atlas_bench_runner"), bundle / "environments" / "core.json", "standalone")
    )
    campaign.run_campaign(
        plan_path,
        bundle,
        runners=runners,
        suite_label="campaign01",
        valid_observations=1,
        max_attempts=2,
    )
    source = (
        bundle
        / "observations"
        / "a-w04"
        / "core-throughput"
        / "attempt-00001"
        / "observation-00001.json"
    )
    copied = source.parent.parent / "attempt-00002" / source.name
    copied.parent.mkdir()
    shutil.copyfile(source, copied)

    called = False

    def fail_if_called(*_arguments: object, **_keywords: object) -> tuple[Path, ...]:
        nonlocal called
        called = True
        return ()

    monkeypatch.setattr(campaign, "run_suite", fail_if_called)
    with pytest.raises(ValueError, match="stale or noncanonical run label"):
        campaign.run_campaign(
            plan_path,
            bundle,
            runners=runners,
            suite_label="campaign01",
            valid_observations=2,
            max_attempts=2,
            resume=True,
        )
    assert not called


def test_finalize_campaign_is_retryable_after_verification_failure(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    plan_path, bundle, environment, environment_digest = _prepare_bundle(tmp_path)
    monkeypatch.setattr(
        campaign,
        "_preflight_runners",
        lambda *_: {"core": (environment.binary_sha256, environment, environment_digest)},
    )

    def fake_run_suite(
        manifest_path: Path,
        output: Path,
        **kwargs: object,
    ) -> tuple[Path, ...]:
        output.mkdir(parents=True)
        path = output / "observation-00001.json"
        write_canonical_document(
            path,
            _observation(
                manifest_path,
                environment,
                environment_digest,
                run_label=_attempt_run_label(manifest_path, kwargs),
            ),
        )
        return (path,)

    monkeypatch.setattr(campaign, "run_suite", fake_run_suite)
    campaign.run_campaign(
        plan_path,
        bundle,
        runners=campaign.CampaignRunners(
            Runner(Path("atlas_bench_runner"), bundle / "environments" / "core.json", "standalone")
        ),
        suite_label="campaign01",
        valid_observations=1,
        max_attempts=1,
    )

    calls = 0

    def fail_once(directory: Path) -> BundleSummary:
        nonlocal calls
        calls += 1
        if calls == 1:
            raise ValueError("injected post-publication verification failure")
        return verify_bundle(directory)

    monkeypatch.setattr(campaign, "verify_bundle", fail_once)
    with pytest.raises(ValueError, match="injected post-publication"):
        campaign.finalize_campaign(
            plan_path,
            bundle,
            valid_observations=1,
            allow_exploratory=True,
        )
    assert not (bundle / "inventory.json").exists()
    assert not (bundle / "reports").exists()
    assert not tuple(bundle.glob(".reports-*"))

    summary = campaign.finalize_campaign(
        plan_path,
        bundle,
        valid_observations=1,
        allow_exploratory=True,
    )
    assert summary.observations == 2


def test_finalize_campaign_rejects_duplicate_environments_without_derived_outputs(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    plan_path, bundle, environment, environment_digest = _prepare_bundle(tmp_path)
    monkeypatch.setattr(
        campaign,
        "_preflight_runners",
        lambda *_: {"core": (environment.binary_sha256, environment, environment_digest)},
    )

    def fake_run_suite(
        manifest_path: Path,
        output: Path,
        **kwargs: object,
    ) -> tuple[Path, ...]:
        output.mkdir(parents=True)
        path = output / "observation-00001.json"
        write_canonical_document(
            path,
            _observation(
                manifest_path,
                environment,
                environment_digest,
                run_label=_attempt_run_label(manifest_path, kwargs),
            ),
        )
        return (path,)

    monkeypatch.setattr(campaign, "run_suite", fake_run_suite)
    campaign.run_campaign(
        plan_path,
        bundle,
        runners=campaign.CampaignRunners(
            Runner(Path("atlas_bench_runner"), bundle / "environments" / "core.json", "standalone")
        ),
        suite_label="campaign01",
        valid_observations=1,
        max_attempts=1,
    )
    source = bundle / "environments" / "core.json"
    shutil.copyfile(source, source.with_name("duplicate.json"))

    with pytest.raises(ValueError, match="duplicate environment documents"):
        campaign.finalize_campaign(
            plan_path,
            bundle,
            valid_observations=1,
            allow_exploratory=True,
        )
    assert not (bundle / "inventory.json").exists()
    assert not (bundle / "reports").exists()


def test_official_finalization_requires_ten_observations(tmp_path: Path) -> None:
    plan_path, bundle, _environment_value, _environment_digest = _prepare_bundle(tmp_path)

    with pytest.raises(ValueError, match="at least ten valid observations"):
        campaign.finalize_campaign(
            plan_path,
            bundle,
            valid_observations=1,
        )


def test_campaign_rejects_stale_unselected_evidence_before_running(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    plan_path, bundle, environment, environment_digest = _prepare_bundle(tmp_path)
    manifests = sorted((bundle / "workloads").glob("*.json"))
    second_manifest = next(path for path in manifests if verify_workload_manifest(path).seed == 5)
    stale = replace(
        _observation(
            second_manifest,
            environment,
            environment_digest,
            run_label="different-suite",
        ),
        suite_label="different-suite",
    )
    attempt = bundle / "observations" / "b-w04" / "core-throughput" / "attempt-00001"
    write_canonical_document(attempt / "observation-00001.json", stale)
    monkeypatch.setattr(
        campaign,
        "_preflight_runners",
        lambda *_: {"core": (environment.binary_sha256, environment, environment_digest)},
    )

    called = False

    def fail_if_called(*_arguments: object, **_keywords: object) -> tuple[Path, ...]:
        nonlocal called
        called = True
        return ()

    monkeypatch.setattr(campaign, "run_suite", fail_if_called)
    with pytest.raises(ValueError, match="runner or suite identity"):
        campaign.run_campaign(
            plan_path,
            bundle,
            runners=campaign.CampaignRunners(
                Runner(
                    Path("atlas_bench_runner"),
                    bundle / "environments" / "core.json",
                    "standalone",
                )
            ),
            suite_label="campaign01",
            valid_observations=1,
            max_attempts=1,
            point_ids=("a-w04",),
        )
    assert not called


def test_finalize_campaign_rejects_missing_and_exploratory_evidence(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    plan_path, bundle, environment, environment_digest = _prepare_bundle(tmp_path)
    with pytest.raises(ValueError, match="observation directory"):
        campaign.finalize_campaign(
            plan_path,
            bundle,
            valid_observations=1,
            allow_exploratory=True,
        )

    monkeypatch.setattr(
        campaign,
        "_preflight_runners",
        lambda *_: {"core": (environment.binary_sha256, environment, environment_digest)},
    )

    def fake_run_suite(
        manifest_path: Path,
        output: Path,
        **kwargs: object,
    ) -> tuple[Path, ...]:
        output.mkdir(parents=True)
        path = output / "observation-00001.json"
        write_canonical_document(
            path,
            _observation(
                manifest_path,
                environment,
                environment_digest,
                run_label=_attempt_run_label(manifest_path, kwargs),
            ),
        )
        return (path,)

    monkeypatch.setattr(campaign, "run_suite", fake_run_suite)
    campaign.run_campaign(
        plan_path,
        bundle,
        runners=campaign.CampaignRunners(
            Runner(
                Path("atlas_bench_runner"),
                bundle / "environments" / "core.json",
                "standalone",
            )
        ),
        suite_label="campaign01",
        valid_observations=10,
        max_attempts=10,
    )
    with pytest.raises(ValueError, match="exploratory evidence"):
        campaign.finalize_campaign(
            plan_path,
            bundle,
            valid_observations=10,
        )


def test_native_host_runbook_fails_closed_and_archives_checkpoints() -> None:
    runbook = (REPOSITORY / "docs" / "phase5-native-host-runbook.md").read_text(encoding="utf-8")

    assert 'test -f "$1"' in runbook
    assert "(\n  set -euo pipefail\n  mkdir -p out/phase5-baseline/checkpoints" in runbook
    assert "-C out/phase5-baseline bundle profiles checkpoints" in runbook
