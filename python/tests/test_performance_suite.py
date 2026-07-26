from __future__ import annotations

import sys
from dataclasses import replace
from pathlib import Path
from typing import cast

import pytest
from atlaslob.performance import suite
from atlaslob.performance.schemas import (
    EnvironmentManifest,
    Observation,
    canonical_json_bytes,
    document_sha256,
    environment_to_dict,
    file_sha256,
    measurement_parameters_for_boundary,
    observation_from_dict,
    observation_to_dict,
    read_canonical_document,
    workload_to_dict,
)
from atlaslob.performance.suite import Runner
from atlaslob.performance.workloads import materialize_workload


def _environment(binary: str = "a" * 64) -> EnvironmentManifest:
    return EnvironmentManifest(
        commit="1" * 40,
        tag=None,
        dirty=False,
        binary_sha256=binary,
        os="Ubuntu 24.04.2 LTS",
        kernel="6.8.0",
        host_class="suite-host",
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
        limitations=("synthetic suite environment",),
    )


def _valid_observation(
    manifest_path: Path,
    environment: EnvironmentManifest,
    environment_digest: str,
    *,
    suite_label: str,
    run_label: str,
    variant: str,
    block: int,
    position: int,
) -> Observation:
    from atlaslob.performance.workloads import verify_workload_manifest

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
        suite_label=suite_label,
        run_label=run_label,
        variant=variant,
        block_index=block,
        block_position=position,
        preload_commands=manifest.preload_commands,
        warmup_commands=manifest.warmup_commands,
        commands=manifest.measured_commands,
        events=manifest.expected_events,
        committed=manifest.expected_committed,
        rejected=manifest.expected_rejected,
        engine_errors=manifest.expected_engine_errors,
        elapsed_ns=1_000,
        rss_before_bytes=1_000,
        rss_after_bytes=1_000,
        peak_rss_bytes=1_000,
        latency_ns=None,
        allocations=None,
        event_digest=manifest.expected_event_digest,
        final_digest=manifest.expected_final_digest,
        valid=True,
        failure_reason=None,
    )


def _manifest(tmp_path: Path) -> Path:
    return materialize_workload(
        "W04",
        tmp_path / "workload",
        seed=4,
        preload_commands=20,
        warmup_commands=20,
        measured_commands=40,
        active_order_target=20,
    )[0]


def test_bounded_process_capture_retains_timeout_and_output_bombs() -> None:
    completed, failure = suite._run_bounded(
        [sys.executable, "-c", "import time; time.sleep(5)"],
        1,
    )
    assert completed is None
    assert failure == "runner timed out"

    completed, failure = suite._run_bounded(
        [sys.executable, "-c", "import sys; sys.stderr.buffer.write(b'x' * 70000)"],
        10,
    )
    assert completed is None
    assert failure == "runner output exceeded evidence bounds"


def test_runner_status_contradiction_is_retained_as_invalid(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    manifest_path = _manifest(tmp_path)
    from atlaslob.performance.workloads import verify_workload_manifest

    manifest = verify_workload_manifest(manifest_path)
    environment = _environment()
    environment_digest = document_sha256(environment_to_dict(environment))
    observation = _valid_observation(
        manifest_path,
        environment,
        environment_digest,
        suite_label="status01",
        run_label="status01-run",
        variant="standalone",
        block=0,
        position=0,
    )
    capture = suite._ProcessCapture(
        returncode=1,
        stdout=canonical_json_bytes(observation_to_dict(observation)),
        stderr=b"",
    )
    monkeypatch.setattr(suite, "_run_bounded", lambda *_: (capture, None))

    result = suite._run_once(
        Runner(tmp_path / "runner", tmp_path / "environment.json", "standalone"),
        environment,
        environment_digest,
        environment.binary_sha256,
        manifest,
        file_sha256(manifest_path),
        manifest_path.parent / manifest.stream_file,
        "status01",
        "throughput",
        0,
        0,
        "status01-run",
        None,
        10,
    )

    assert not result.valid
    assert result.failure_reason == "runner process status contradicts its evidence"


def test_abba_block_start_schedule_and_no_overwrite(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    manifest_path = _manifest(tmp_path)
    baseline_environment = _environment("a" * 64)
    candidate_environment = replace(
        baseline_environment,
        binary_sha256="b" * 64,
        host_context_sha256="",
    )
    environments = {
        "baseline": baseline_environment,
        "candidate": candidate_environment,
        "standalone": baseline_environment,
    }
    environment_digests = {
        role: document_sha256(environment_to_dict(value)) for role, value in environments.items()
    }
    schedule: list[tuple[str, int, int]] = []

    def prepare(runner: Runner, _mode: suite.RunnerMode) -> tuple[str, EnvironmentManifest, str]:
        environment = environments[runner.variant]
        return (
            environment.binary_sha256,
            environment,
            environment_digests[runner.variant],
        )

    def run_once(*arguments: object) -> Observation:
        runner = cast(Runner, arguments[0])
        block = cast(int, arguments[9])
        position = cast(int, arguments[10])
        run_label = cast(str, arguments[11])
        schedule.append((runner.variant, block, position))
        return _valid_observation(
            manifest_path,
            environments[runner.variant],
            environment_digests[runner.variant],
            suite_label="abba01",
            run_label=run_label,
            variant=runner.variant,
            block=block,
            position=position,
        )

    monkeypatch.setattr(suite, "_prepare_runner", prepare)
    monkeypatch.setattr(suite, "_run_once", run_once)
    output = tmp_path / "observations"
    paths = suite.run_suite(
        manifest_path,
        output,
        baseline=Runner(Path("baseline"), Path("baseline-env"), "baseline"),
        candidate=Runner(Path("candidate"), Path("candidate-env"), "candidate"),
        suite_label="abba01",
        observations=2,
        block_start=7,
    )

    assert len(paths) == 8
    assert schedule == [
        ("baseline", 7, 1),
        ("candidate", 7, 2),
        ("candidate", 7, 3),
        ("baseline", 7, 4),
        ("baseline", 8, 1),
        ("candidate", 8, 2),
        ("candidate", 8, 3),
        ("baseline", 8, 4),
    ]
    with pytest.raises(ValueError, match="must be empty"):
        suite.run_suite(
            manifest_path,
            output,
            baseline=Runner(Path("baseline"), Path("baseline-env"), "standalone"),
            suite_label="standalone01",
            observations=1,
        )


def test_interrupted_attempt_is_written_before_keyboard_interrupt_is_reraised(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    manifest_path = _manifest(tmp_path)
    environment = _environment()
    environment_digest = document_sha256(environment_to_dict(environment))

    monkeypatch.setattr(
        suite,
        "_prepare_runner",
        lambda *_: (environment.binary_sha256, environment, environment_digest),
    )

    def interrupt(*_arguments: object) -> Observation:
        raise KeyboardInterrupt

    monkeypatch.setattr(suite, "_run_once", interrupt)
    output = tmp_path / "observations"
    with pytest.raises(KeyboardInterrupt):
        suite.run_suite(
            manifest_path,
            output,
            baseline=Runner(Path("runner"), Path("environment"), "standalone"),
            suite_label="interrupt01",
            observations=1,
        )

    retained = read_canonical_document(
        output / "observation-00001.json",
        observation_from_dict,
    )
    assert not retained.valid
    assert retained.failure_reason == "runner was interrupted by the orchestrator"


def test_mismatched_runner_evidence_is_replaced_by_retained_invalid_attempt(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    manifest_path = _manifest(tmp_path)
    environment = _environment()
    environment_digest = document_sha256(environment_to_dict(environment))
    monkeypatch.setattr(
        suite,
        "_prepare_runner",
        lambda *_: (environment.binary_sha256, environment, environment_digest),
    )

    def mismatch(*arguments: object) -> Observation:
        run_label = cast(str, arguments[11])
        valid = _valid_observation(
            manifest_path,
            environment,
            environment_digest,
            suite_label="mismatch01",
            run_label=run_label,
            variant="standalone",
            block=0,
            position=0,
        )
        return replace(valid, workload_id="W05")

    monkeypatch.setattr(suite, "_run_once", mismatch)
    output = tmp_path / "observations"
    suite.run_suite(
        manifest_path,
        output,
        baseline=Runner(Path("runner"), Path("environment"), "standalone"),
        suite_label="mismatch01",
        observations=1,
    )

    retained = read_canonical_document(
        output / "observation-00001.json",
        observation_from_dict,
    )
    assert not retained.valid
    assert retained.failure_reason == "runner evidence differed from its invocation"


def test_direct_suite_rejects_incompatible_boundary_workload_pair(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    manifest_path = _manifest(tmp_path)
    environment = _environment()
    environment_digest = document_sha256(environment_to_dict(environment))
    monkeypatch.setattr(
        suite,
        "_prepare_runner",
        lambda *_: (environment.binary_sha256, environment, environment_digest),
    )

    with pytest.raises(ValueError, match="W10|replay"):
        suite.run_suite(
            manifest_path,
            tmp_path / "replay",
            baseline=Runner(Path("runner"), Path("environment"), "standalone"),
            suite_label="badreplay01",
            mode="replay-fast",
            observations=1,
        )


def test_fixture_manifest_digest_matches_canonical_document(tmp_path: Path) -> None:
    manifest_path = _manifest(tmp_path)
    from atlaslob.performance.workloads import verify_workload_manifest

    manifest = verify_workload_manifest(manifest_path)
    assert file_sha256(manifest_path) == document_sha256(workload_to_dict(manifest))
