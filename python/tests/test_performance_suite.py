from __future__ import annotations

import shutil
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
from atlaslob.performance.workloads import (
    VerifiedWorkload,
    materialize_workload,
    verify_campaign_workload,
)


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


def _run_once_with_capture(
    monkeypatch: pytest.MonkeyPatch,
    manifest_path: Path,
    environment: EnvironmentManifest,
    environment_digest: str,
    capture: suite._ProcessCapture,
    *,
    process_failure: str | None = None,
    diagnostic_phases: bool = False,
) -> Observation:
    from atlaslob.performance.workloads import verify_workload_manifest

    manifest = verify_workload_manifest(manifest_path)
    monkeypatch.setattr(
        suite,
        "_run_bounded",
        lambda *_: (capture, process_failure),
    )
    return suite._run_once(
        Runner(
            manifest_path.parent / "runner", manifest_path.parent / "environment.json", "standalone"
        ),
        environment,
        environment_digest,
        environment.binary_sha256,
        manifest,
        file_sha256(manifest_path),
        manifest_path.parent / manifest.stream_file,
        "diagnostic01",
        "throughput",
        0,
        0,
        "diagnostic01-run",
        None,
        10,
        diagnostic_phases,
    )


def test_bounded_process_capture_retains_timeout_and_output_bombs() -> None:
    completed, failure = suite._run_bounded(
        [
            sys.executable,
            "-c",
            "import sys,time; print(123, flush=True); "
            "print(456, file=sys.stderr, flush=True); time.sleep(5)",
        ],
        1,
    )
    assert completed is not None
    assert failure == "runner timed out"
    assert completed.returncode < 0
    assert completed.stdout == b"123\n"
    assert completed.stderr == b"456\n"

    completed, failure = suite._run_bounded(
        [sys.executable, "-c", 'import sys; sys.stderr.buffer.write(b"x" * 70000)'],
        10,
    )
    assert completed is not None
    assert failure == "runner output exceeded evidence bounds"
    assert completed.returncode <= 0
    assert len(completed.stderr) <= 64 * 1024


def test_bounded_process_capture_retains_signal_status() -> None:
    completed, failure = suite._run_bounded(
        [
            sys.executable,
            "-c",
            "import os,signal; os.kill(os.getpid(), signal.SIGTERM)",
        ],
        10,
    )
    assert completed is not None
    assert failure is None
    assert completed.returncode == -15
    reason = suite._process_failure_reason("runner terminated by signal", completed, None)
    assert "status=signal:SIGTERM" in reason
    assert "stdout_bytes=0" in reason
    assert "stderr_bytes=0" in reason


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
    assert result.failure_reason is not None
    assert result.failure_reason.startswith("runner process status contradicts its evidence; ")
    assert "status=exit:1" in result.failure_reason


def test_partial_stdout_is_retained_but_never_accepted(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    manifest_path = _manifest(tmp_path)
    environment = _environment()
    environment_digest = document_sha256(environment_to_dict(environment))
    partial = b'{"schema":"ATLAS_BENCH_OBSERVATION_V1"'
    result = _run_once_with_capture(
        monkeypatch,
        manifest_path,
        environment,
        environment_digest,
        suite._ProcessCapture(returncode=0, stdout=partial, stderr=b""),
    )

    assert not result.valid
    assert result.failure_reason is not None
    assert result.failure_reason.startswith("runner produced invalid evidence; ")
    assert f"stdout_bytes={len(partial)}" in result.failure_reason
    assert "stdout_prefix_b64=" in result.failure_reason


def test_partial_stderr_invalidates_valid_stdout_and_is_retained(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    manifest_path = _manifest(tmp_path)
    environment = _environment()
    environment_digest = document_sha256(environment_to_dict(environment))
    observation = _valid_observation(
        manifest_path,
        environment,
        environment_digest,
        suite_label="diagnostic01",
        run_label="diagnostic01-run",
        variant="standalone",
        block=0,
        position=0,
    )
    stderr = b"bounded diagnostic detail"
    result = _run_once_with_capture(
        monkeypatch,
        manifest_path,
        environment,
        environment_digest,
        suite._ProcessCapture(
            returncode=0,
            stdout=canonical_json_bytes(observation_to_dict(observation)),
            stderr=stderr,
        ),
    )

    assert not result.valid
    assert result.failure_reason is not None
    assert result.failure_reason.startswith("runner process status contradicts its evidence; ")
    assert f"stderr_bytes={len(stderr)}" in result.failure_reason
    assert "stderr_prefix_b64=" in result.failure_reason


def test_timeout_retains_last_diagnostic_phase_without_accepting_partial_output(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    manifest_path = _manifest(tmp_path)
    environment = _environment()
    environment_digest = document_sha256(environment_to_dict(environment))
    result = _run_once_with_capture(
        monkeypatch,
        manifest_path,
        environment,
        environment_digest,
        suite._ProcessCapture(
            returncode=-9,
            stdout=b"partial",
            stderr=(
                b"ATLAS_DIAGNOSTIC_PHASE workload-parsed\n"
                b"ATLAS_DIAGNOSTIC_PHASE measured-region-enter 123456\n"
                b"bounded native detail"
            ),
        ),
        process_failure="runner timed out",
        diagnostic_phases=True,
    )

    assert not result.valid
    assert result.failure_reason is not None
    assert result.failure_reason.startswith("runner timed out; phase=measured-region-enter; ")
    assert "status=signal:SIGKILL" in result.failure_reason
    assert "ATLAS_DIAGNOSTIC_PHASE" not in result.failure_reason
    assert "stderr_bytes=21" in result.failure_reason


def test_diagnostic_markers_do_not_change_successful_observation_bytes(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    manifest_path = _manifest(tmp_path)
    environment = _environment()
    environment_digest = document_sha256(environment_to_dict(environment))
    expected = _valid_observation(
        manifest_path,
        environment,
        environment_digest,
        suite_label="diagnostic01",
        run_label="diagnostic01-run",
        variant="standalone",
        block=0,
        position=0,
    )
    canonical = canonical_json_bytes(observation_to_dict(expected))
    result = _run_once_with_capture(
        monkeypatch,
        manifest_path,
        environment,
        environment_digest,
        suite._ProcessCapture(
            returncode=0,
            stdout=canonical,
            stderr=(
                b"ATLAS_DIAGNOSTIC_PHASE workload-parsed\n"
                b"ATLAS_DIAGNOSTIC_PHASE observation-ready\n"
            ),
        ),
        diagnostic_phases=True,
    )

    assert result == expected
    assert canonical_json_bytes(observation_to_dict(result)) == canonical


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


def test_direct_suite_still_invokes_eager_semantic_verification(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    manifest_path = _manifest(tmp_path)

    def eager_called(_path: Path) -> None:
        raise ValueError("eager verifier called")

    monkeypatch.setattr(suite, "verify_workload_manifest", eager_called)
    output = tmp_path / "direct"
    with pytest.raises(ValueError, match="eager verifier called"):
        suite.run_suite(
            manifest_path,
            output,
            baseline=Runner(Path("runner"), Path("environment"), "standalone"),
            suite_label="directverify01",
            observations=1,
        )
    assert not output.exists()


def _run_verified_without_process(
    monkeypatch: pytest.MonkeyPatch,
    workload: VerifiedWorkload,
    output: Path,
) -> tuple[Path, ...]:
    environment = _environment()
    environment_digest = document_sha256(environment_to_dict(environment))
    monkeypatch.setattr(
        suite,
        "_prepare_runner",
        lambda *_: (environment.binary_sha256, environment, environment_digest),
    )

    def run_once(*arguments: object) -> Observation:
        return _valid_observation(
            workload.manifest_path,
            environment,
            environment_digest,
            suite_label="verified01",
            run_label=cast(str, arguments[11]),
            variant="standalone",
            block=0,
            position=0,
        )

    monkeypatch.setattr(suite, "_run_once", run_once)
    return suite.run_verified_suite(
        workload,
        output,
        baseline=Runner(Path("runner"), Path("environment"), "standalone"),
        suite_label="verified01",
        observations=1,
    )


def test_verified_suite_does_not_repeat_semantic_replay(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    workload = verify_campaign_workload(_manifest(tmp_path))

    def forbidden(_path: Path) -> None:
        raise AssertionError("semantic verifier must not run for a capability")

    monkeypatch.setattr(suite, "verify_workload_manifest", forbidden)
    paths = _run_verified_without_process(monkeypatch, workload, tmp_path / "verified-output")

    assert len(paths) == 1


@pytest.mark.parametrize("target", ("manifest", "stream"))
def test_verified_suite_rejects_same_size_byte_mutation_before_output_or_runner(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, target: str
) -> None:
    workload = verify_campaign_workload(_manifest(tmp_path))
    path = workload.manifest_path if target == "manifest" else workload.stream_path
    payload = bytearray(path.read_bytes())
    payload[-2] ^= 1
    path.write_bytes(payload)
    called = False

    def prepare(*_arguments: object) -> tuple[str, EnvironmentManifest, str]:
        nonlocal called
        called = True
        raise AssertionError("runner preparation must not start")

    monkeypatch.setattr(suite, "_prepare_runner", prepare)
    output = tmp_path / f"mutated-{target}"
    with pytest.raises(ValueError, match=f"{target} bytes changed"):
        suite.run_verified_suite(
            workload,
            output,
            baseline=Runner(Path("runner"), Path("environment"), "standalone"),
            suite_label="mutation01",
            observations=1,
        )

    assert path.stat().st_size == len(payload)
    assert not called
    assert not output.exists()


def test_verified_suite_rejects_w10_timed_input_mutation_before_output_or_runner(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    source = Path(__file__).resolve().parents[2] / "benchmarks" / "fixtures" / "v1"
    directory = tmp_path / "w10"
    directory.mkdir()
    for path in source.glob("w10-*"):
        shutil.copy2(path, directory / path.name)
    workload = verify_campaign_workload(next(directory.glob("w10-*.json")))
    assert workload.timed_input_path is not None
    payload = bytearray(workload.timed_input_path.read_bytes())
    payload[-1] ^= 1
    workload.timed_input_path.write_bytes(payload)
    called = False

    def prepare(*_arguments: object) -> tuple[str, EnvironmentManifest, str]:
        nonlocal called
        called = True
        raise AssertionError("runner preparation must not start")

    monkeypatch.setattr(suite, "_prepare_runner", prepare)
    output = tmp_path / "mutated-timed-input"
    with pytest.raises(ValueError, match="timed-input bytes changed"):
        suite.run_verified_suite(
            workload,
            output,
            baseline=Runner(Path("runner"), Path("environment"), "standalone"),
            suite_label="mutation02",
            observations=1,
        )

    assert not called
    assert not output.exists()


def test_fixture_manifest_digest_matches_canonical_document(tmp_path: Path) -> None:
    manifest_path = _manifest(tmp_path)
    from atlaslob.performance.workloads import verify_workload_manifest

    manifest = verify_workload_manifest(manifest_path)
    assert file_sha256(manifest_path) == document_sha256(workload_to_dict(manifest))


def test_python_timeout_diagnostics_are_opt_in_and_passed_only_to_worker(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    manifest_path = _manifest(tmp_path)
    from atlaslob.performance.workloads import verify_workload_manifest

    manifest = verify_workload_manifest(manifest_path)
    environment = replace(
        _environment(),
        runtime_kind="cpython",
        benchmark_build=False,
        python_implementation="CPython",
        python_version="3.12.3",
        python_cache_tag="cpython-312",
        atlaslob_version="0.2.0",
        interpreter_sha256="1" * 64,
        wheel_sha256="2" * 64,
        package_sha256="3" * 64,
        wrapper_sha256="4" * 64,
        harness_sha256="5" * 64,
        host_context_sha256="",
    )
    environment_digest = document_sha256(environment_to_dict(environment))
    invocations: list[list[str]] = []

    def bounded(arguments: list[str], _timeout: int) -> tuple[suite._ProcessCapture, str]:
        invocations.append(arguments)
        return (
            suite._ProcessCapture(
                returncode=-9,
                stdout=b"",
                stderr=b"ATLAS_DIAGNOSTIC_PHASE measured-enter 123456\n",
            ),
            "runner timed out",
        )

    monkeypatch.setattr(suite, "_run_bounded", bounded)
    runner = Runner(
        tmp_path / "python",
        tmp_path / "environment.json",
        "standalone",
        wheel=tmp_path / "atlaslob.whl",
        worker=tmp_path / "worker.py",
    )

    result = suite._run_once(
        runner,
        environment,
        environment_digest,
        environment.binary_sha256,
        manifest,
        file_sha256(manifest_path),
        manifest_path.parent / manifest.stream_file,
        "diagnostic01",
        "python-columns",
        0,
        0,
        "diagnostic01-python-run",
        1,
        10,
        True,
    )

    assert invocations and invocations[0][-1] == "--diagnostic-phases"
    assert not result.valid
    assert result.failure_reason is not None
    assert result.failure_reason.startswith("runner timed out; phase=measured-enter; ")


def test_diagnostic_phases_reject_official_environments_before_runner_launch(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    manifest_path = _manifest(tmp_path)
    official = replace(
        _environment(),
        classification="official",
        limitations=(),
        host_context_sha256="",
    )
    launches = 0

    monkeypatch.setattr(
        suite,
        "_prepare_runner",
        lambda *_: (official.binary_sha256, official, "f" * 64),
    )

    def bounded(*_: object) -> tuple[suite._ProcessCapture | None, str | None]:
        nonlocal launches
        launches += 1
        return None, "unexpected launch"

    monkeypatch.setattr(suite, "_run_bounded", bounded)
    with pytest.raises(ValueError, match="exploratory environments"):
        suite.run_suite(
            manifest_path,
            tmp_path / "observations",
            baseline=Runner(
                tmp_path / "python",
                tmp_path / "environment.json",
                "standalone",
                wheel=tmp_path / "atlaslob.whl",
                worker=tmp_path / "worker.py",
            ),
            suite_label="diagnostic01",
            mode="python-columns",
            observations=1,
            batch_size=1,
            diagnostic_phases=True,
        )

    assert launches == 0
    assert not (tmp_path / "observations").exists()
