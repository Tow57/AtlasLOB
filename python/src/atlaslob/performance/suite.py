"""One-process-per-observation benchmark orchestration."""

from __future__ import annotations

import subprocess
import threading
import time
from dataclasses import dataclass, replace
from pathlib import Path
from typing import BinaryIO, Literal

from atlaslob.performance.environment import (
    verify_python_runtime_compatibility,
    verify_runtime_compatibility,
)
from atlaslob.performance.schemas import (
    OBSERVATION_DOCUMENT_MAX_BYTES,
    EnvironmentManifest,
    Observation,
    WorkloadManifest,
    environment_from_dict,
    file_sha256,
    measurement_parameters_for_boundary,
    observation_from_dict,
    parse_canonical_document,
    read_canonical_document,
    validate_observation_against_workload,
    workload_measurement_parameters,
    write_canonical_document,
)
from atlaslob.performance.workloads import verify_workload_manifest

RunnerMode = Literal[
    "throughput",
    "latency",
    "allocation",
    "construction",
    "preload",
    "setup-allocation",
    "replay-fast",
    "replay-verify",
    "python-objects",
    "python-columns",
    "python-summary",
]
RunnerVariant = Literal["standalone", "baseline", "candidate"]
_BOUNDARY_BY_MODE = {
    "throughput": "core_throughput",
    "latency": "core_latency",
    "allocation": "core_allocation",
    "construction": "core_construction",
    "preload": "core_preload",
    "setup-allocation": "core_setup_allocation",
    "replay-fast": "replay_fast",
    "replay-verify": "replay_verify",
    "python-objects": "python_objects",
    "python-columns": "python_columns",
    "python-summary": "python_summary",
}


@dataclass(frozen=True, slots=True)
class Runner:
    executable: Path
    environment_path: Path
    variant: RunnerVariant
    wheel: Path | None = None
    worker: Path | None = None
    command_prefix: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if any(not isinstance(item, str) or not item for item in self.command_prefix):
            raise ValueError("runner command prefix must contain nonempty strings")


@dataclass(frozen=True, slots=True)
class _ProcessCapture:
    returncode: int
    stdout: bytes
    stderr: bytes


def run_suite(
    manifest_path: Path,
    output_directory: Path,
    *,
    baseline: Runner,
    candidate: Runner | None = None,
    suite_label: str,
    mode: RunnerMode = "throughput",
    observations: int | None = None,
    block_start: int = 1,
    batch_size: int | None = None,
    timeout_seconds: int = 900,
) -> tuple[Path, ...]:
    """Launch one runner process per retained observation.

    A single runner produces standalone observations.  Two runners produce
    ``A-B-B-A`` blocks, where ``observations`` is the number of blocks.
    """

    if mode not in _BOUNDARY_BY_MODE:
        raise ValueError("unsupported runner mode")
    python_mode = mode.startswith("python-")
    if python_mode:
        if batch_size not in {1, 64, 1024, 65_536}:
            raise ValueError("Python suites require a frozen batch size")
    elif batch_size is not None:
        raise ValueError("batch_size is only valid for Python suites")
    if (
        not suite_label
        or len(suite_label) > 32
        or not suite_label[0].isalnum()
        or any(not (character.isalnum() or character in "_.-") for character in suite_label)
    ):
        raise ValueError("suite_label must be a safe identifier of at most 32 characters")
    resolved_observations = (
        (5 if candidate is not None else 10) if observations is None else observations
    )
    if isinstance(resolved_observations, bool) or not 1 <= resolved_observations <= 10_000:
        raise ValueError("observations must be in [1, 10000]")
    if isinstance(timeout_seconds, bool) or not 1 <= timeout_seconds <= 900:
        raise ValueError("timeout_seconds must be in [1, 900]")
    if isinstance(block_start, bool) or not 1 <= block_start <= 10_000_000:
        raise ValueError("block_start must be in [1, 10000000]")
    if baseline.variant != ("baseline" if candidate is not None else "standalone"):
        raise ValueError("baseline runner has the wrong variant")
    if candidate is not None and candidate.variant != "candidate":
        raise ValueError("candidate runner has the wrong variant")

    manifest = verify_workload_manifest(manifest_path)
    boundary = _BOUNDARY_BY_MODE[mode]
    measurement_parameters_for_boundary(
        manifest,
        boundary,
        batch_size=batch_size,
    )
    manifest_document_digest = file_sha256(manifest_path)
    prepared: dict[RunnerVariant, tuple[str, EnvironmentManifest, str]] = {
        baseline.variant: _prepare_runner(baseline, mode),
    }
    if candidate is not None:
        prepared[candidate.variant] = _prepare_runner(candidate, mode)
        contexts = {item[1].host_context_sha256 for item in prepared.values()}
        if len(contexts) != 1:
            raise ValueError("baseline and candidate host/build contexts differ")
        if prepared["baseline"][0] == prepared["candidate"][0]:
            raise ValueError("baseline and candidate binaries must differ")

    schedule: list[tuple[int, int, Runner]] = []
    if candidate is None:
        schedule.extend((0, 0, baseline) for _ in range(resolved_observations))
    else:
        for block in range(block_start, block_start + resolved_observations):
            schedule.extend(
                (
                    (block, 1, baseline),
                    (block, 2, candidate),
                    (block, 3, candidate),
                    (block, 4, baseline),
                )
            )

    if output_directory.exists() and any(output_directory.iterdir()):
        raise ValueError("observation output directory must be empty")
    output_directory.mkdir(parents=True, exist_ok=True)
    paths: list[Path] = []
    counters: dict[str, int] = {"standalone": block_start - 1} if candidate is None else {}
    for sequence, (block, position, runner) in enumerate(schedule, start=1):
        executable_digest, environment, environment_digest = prepared[runner.variant]
        counters[runner.variant] = counters.get(runner.variant, 0) + 1
        run_label = _run_label(
            suite_label,
            manifest.workload_id,
            manifest.stream_sha256,
            boundary,
            runner.variant,
            block,
            position,
            counters[runner.variant],
        )
        try:
            observation = _run_once(
                runner,
                environment,
                environment_digest,
                executable_digest,
                manifest,
                manifest_document_digest,
                manifest_path.parent / manifest.stream_file,
                suite_label,
                mode,
                block,
                position,
                run_label,
                batch_size,
                timeout_seconds,
            )
        except KeyboardInterrupt:
            observation = _failed_observation(
                boundary,
                manifest,
                manifest_document_digest,
                suite_label,
                environment,
                environment_digest,
                executable_digest,
                runner.variant,
                block,
                position,
                run_label,
                batch_size,
                "runner was interrupted by the orchestrator",
            )
            _validate_observation(
                observation,
                manifest,
                manifest_document_digest,
                suite_label,
                environment,
                environment_digest,
                executable_digest,
                run_label,
                runner.variant,
                block,
                position,
                boundary,
            )
            path = output_directory / f"observation-{sequence:05d}.json"
            if path.exists():
                raise ValueError("observation output path already exists") from None
            write_canonical_document(path, observation)
            raise
        try:
            _validate_observation(
                observation,
                manifest,
                manifest_document_digest,
                suite_label,
                environment,
                environment_digest,
                executable_digest,
                run_label,
                runner.variant,
                block,
                position,
                boundary,
            )
        except ValueError:
            observation = _failed_observation(
                boundary,
                manifest,
                manifest_document_digest,
                suite_label,
                environment,
                environment_digest,
                executable_digest,
                runner.variant,
                block,
                position,
                run_label,
                batch_size,
                "runner evidence differed from its invocation",
            )
            _validate_observation(
                observation,
                manifest,
                manifest_document_digest,
                suite_label,
                environment,
                environment_digest,
                executable_digest,
                run_label,
                runner.variant,
                block,
                position,
                boundary,
            )
        path = output_directory / f"observation-{sequence:05d}.json"
        if path.exists():
            raise ValueError("observation output path already exists")
        write_canonical_document(path, observation)
        paths.append(path)
    return tuple(paths)


def _run_label(
    suite_label: str,
    workload_id: str,
    stream_sha256: str,
    boundary: str,
    variant: RunnerVariant,
    block: int,
    position: int,
    counter: int,
) -> str:
    return (
        f"{suite_label}-{workload_id.lower()}-{stream_sha256[:12]}-{boundary}-"
        f"{variant}-b{block:05d}-p{position}-r{counter:05d}"
    )


def _prepare_runner(
    runner: Runner,
    mode: RunnerMode,
) -> tuple[str, EnvironmentManifest, str]:
    if not runner.executable.is_file():
        raise ValueError("runner executable does not exist")
    python_mode = mode.startswith("python-")
    environment = read_canonical_document(runner.environment_path, environment_from_dict)
    if python_mode:
        if runner.wheel is None or runner.worker is None:
            raise ValueError("Python runner requires a wheel and standalone worker")
        if environment.runtime_kind != "cpython":
            raise ValueError("Python benchmark runner requires a CPython environment manifest")
        verify_python_runtime_compatibility(
            environment,
            runner.executable,
            runner.wheel,
            runner.worker,
        )
        return environment.binary_sha256, environment, file_sha256(runner.environment_path)
    if runner.wheel is not None or runner.worker is not None:
        raise ValueError("native benchmark runners cannot carry Python wheel inputs")
    expected_stem = (
        "atlas_bench_alloc_runner"
        if mode in {"allocation", "setup-allocation"}
        else "atlas_bench_runner"
    )
    if runner.executable.stem != expected_stem:
        raise ValueError(f"{mode} requires {expected_stem}")
    if environment.runtime_kind != "native":
        raise ValueError("native benchmark runner requires a native environment manifest")
    executable_digest = file_sha256(runner.executable)
    if executable_digest != environment.binary_sha256:
        raise ValueError("runner binary differs from its environment manifest")
    verify_runtime_compatibility(environment, runner.executable)
    return executable_digest, environment, file_sha256(runner.environment_path)


def _run_once(
    runner: Runner,
    environment: EnvironmentManifest,
    environment_digest: str,
    executable_digest: str,
    manifest: WorkloadManifest,
    manifest_document_digest: str,
    workload_path: Path,
    suite_label: str,
    mode: RunnerMode,
    block: int,
    position: int,
    run_label: str,
    batch_size: int | None,
    timeout_seconds: int,
) -> Observation:
    common_arguments = [
        "--workload",
        str(workload_path),
        "--workload-id",
        manifest.workload_id,
        "--workload-sha256",
        manifest.stream_sha256,
        "--workload-manifest-sha256",
        manifest_document_digest,
        "--binary-sha256",
        executable_digest,
        "--environment-sha256",
        environment_digest,
        "--host-context-sha256",
        environment.host_context_sha256,
        "--run-label",
        run_label,
        "--suite-label",
        suite_label,
        "--variant",
        runner.variant,
        "--block-index",
        str(block),
        "--block-position",
        str(position),
        "--preload-count",
        str(manifest.preload_commands),
        "--warmup-count",
        str(manifest.warmup_commands),
        "--measured-count",
        str(manifest.measured_commands),
        "--expected-events",
        str(manifest.expected_events),
        "--expected-committed",
        str(manifest.expected_committed),
        "--expected-rejected",
        str(manifest.expected_rejected),
        "--expected-engine-errors",
        str(manifest.expected_engine_errors),
        "--expected-event-digest",
        manifest.expected_event_digest,
        "--expected-final-digest",
        manifest.expected_final_digest,
    ]
    boundary = _BOUNDARY_BY_MODE[mode]
    if mode.startswith("python-"):
        assert runner.worker is not None
        assert batch_size is not None
        common_parameters = dict(workload_measurement_parameters(manifest))
        arguments = [
            *runner.command_prefix,
            str(runner.executable),
            str(runner.worker),
            "run",
            *common_arguments,
            "--instrument-count",
            common_parameters["instrument_count"],
            "--measured-start-active-order-count",
            str(manifest.measured_start_active_order_count),
            "--sweep-depth",
            common_parameters["sweep_depth"],
            "--batch-size",
            str(batch_size),
            "--output-mode",
            mode.removeprefix("python-"),
        ]
    else:
        arguments = [*runner.command_prefix, str(runner.executable), *common_arguments]
        for name, value in measurement_parameters_for_boundary(manifest, boundary):
            arguments.extend(("--measurement-parameter", f"{name}={value}"))
        if mode in {"replay-fast", "replay-verify"}:
            assert manifest.timed_input_file is not None
            assert manifest.timed_input_sha256 is not None
            arguments.extend(
                (
                    "--replay-log",
                    str(workload_path.parent / manifest.timed_input_file),
                    "--replay-log-sha256",
                    manifest.timed_input_sha256,
                )
            )
        if mode in {"construction", "preload", "setup-allocation"}:
            arguments.extend(
                (
                    "--expected-empty-state-digest",
                    manifest.expected_empty_state_digest,
                    "--expected-preload-events",
                    str(manifest.expected_preload_events),
                    "--expected-preload-committed",
                    str(manifest.expected_preload_committed),
                    "--expected-preload-rejected",
                    str(manifest.expected_preload_rejected),
                    "--expected-preload-engine-errors",
                    str(manifest.expected_preload_engine_errors),
                    "--expected-preload-event-digest",
                    manifest.expected_preload_event_digest,
                    "--expected-preload-state-digest",
                    manifest.expected_preload_state_digest,
                    "--expected-preload-active-orders",
                    str(manifest.after_preload_active_order_count),
                )
            )
        if mode != "allocation":
            arguments.extend(("--mode", mode))
    completed, process_failure = _run_bounded(arguments, timeout_seconds)
    if completed is None:
        return _failed_observation(
            boundary,
            manifest,
            manifest_document_digest,
            suite_label,
            environment,
            environment_digest,
            executable_digest,
            runner.variant,
            block,
            position,
            run_label,
            batch_size,
            process_failure or "runner could not be launched",
        )
    try:
        observation = parse_canonical_document(completed.stdout, observation_from_dict)
    except ValueError:
        return _failed_observation(
            boundary,
            manifest,
            manifest_document_digest,
            suite_label,
            environment,
            environment_digest,
            executable_digest,
            runner.variant,
            block,
            position,
            run_label,
            batch_size,
            "runner produced invalid evidence",
        )
    contradiction = (
        bool(completed.stderr)
        or (observation.valid and completed.returncode != 0)
        or (not observation.valid and completed.returncode == 0)
    )
    if contradiction:
        observation = replace(
            observation,
            valid=False,
            failure_reason="runner process status contradicts its evidence",
        )
    return observation


def _run_bounded(
    arguments: list[str],
    timeout_seconds: int,
) -> tuple[_ProcessCapture | None, str | None]:
    try:
        process = subprocess.Popen(
            arguments,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError:
        return None, "runner could not be launched"
    assert process.stdout is not None
    assert process.stderr is not None
    exceeded = threading.Event()
    outputs = (bytearray(), bytearray())

    def read_stream(
        stream: BinaryIO,
        output: bytearray,
        maximum: int,
    ) -> None:
        try:
            while chunk := stream.read(64 * 1024):
                if len(output) + len(chunk) > maximum:
                    exceeded.set()
                    return
                output.extend(chunk)
        except OSError:
            exceeded.set()

    threads = (
        threading.Thread(
            target=read_stream,
            args=(process.stdout, outputs[0], OBSERVATION_DOCUMENT_MAX_BYTES),
            daemon=True,
        ),
        threading.Thread(
            target=read_stream,
            args=(process.stderr, outputs[1], 64 * 1024),
            daemon=True,
        ),
    )
    for thread in threads:
        thread.start()
    deadline = time.monotonic() + timeout_seconds
    failure: str | None = None
    try:
        while process.poll() is None:
            if exceeded.wait(timeout=0.05):
                failure = "runner output exceeded evidence bounds"
                process.kill()
                break
            if time.monotonic() >= deadline:
                failure = "runner timed out"
                process.kill()
                break
        process.wait()
    except BaseException:
        if process.poll() is None:
            process.kill()
        process.wait()
        for thread in threads:
            thread.join(timeout=1)
        process.stdout.close()
        process.stderr.close()
        raise
    for thread in threads:
        thread.join(timeout=1)
    process.stdout.close()
    process.stderr.close()
    if any(thread.is_alive() for thread in threads):
        return None, "runner output could not be captured"
    if failure is not None or exceeded.is_set():
        return None, failure or "runner output could not be captured"
    return (
        _ProcessCapture(
            returncode=process.returncode,
            stdout=bytes(outputs[0]),
            stderr=bytes(outputs[1]),
        ),
        None,
    )


def _failed_observation(
    boundary: str,
    manifest: WorkloadManifest,
    manifest_document_digest: str,
    suite_label: str,
    environment: EnvironmentManifest,
    environment_digest: str,
    executable_digest: str,
    variant: RunnerVariant,
    block: int,
    position: int,
    run_label: str,
    batch_size: int | None,
    reason: str,
) -> Observation:
    from atlaslob.performance.schemas import AllocationMetrics

    return Observation(
        boundary=boundary,
        timed_input_kind=("atlslg01" if boundary.startswith("replay_") else "none"),
        timed_input_sha256=(
            manifest.timed_input_sha256 if boundary.startswith("replay_") else None
        ),
        measurement_parameters=measurement_parameters_for_boundary(
            manifest, boundary, batch_size=batch_size
        ),
        workload_id=manifest.workload_id,
        workload_sha256=manifest.stream_sha256,
        workload_manifest_sha256=manifest_document_digest,
        binary_sha256=executable_digest,
        environment_sha256=environment_digest,
        host_context_sha256=environment.host_context_sha256,
        suite_label=suite_label,
        run_label=run_label,
        variant=variant,
        block_index=block,
        block_position=position,
        preload_commands=manifest.preload_commands,
        warmup_commands=manifest.warmup_commands,
        commands=0,
        events=0,
        committed=0,
        rejected=0,
        engine_errors=0,
        elapsed_ns=0,
        rss_before_bytes=0,
        rss_after_bytes=0,
        peak_rss_bytes=0,
        latency_ns=() if boundary == "core_latency" else None,
        allocations=(
            AllocationMetrics(0, 0, 0, 0, 0)
            if boundary in {"core_allocation", "core_setup_allocation"}
            else None
        ),
        event_digest="0" * 64,
        final_digest="0" * 64,
        valid=False,
        failure_reason=reason,
    )


def _validate_observation(
    value: Observation,
    manifest: WorkloadManifest,
    manifest_document_digest: str,
    suite_label: str,
    environment: EnvironmentManifest,
    environment_digest: str,
    executable_digest: str,
    run_label: str,
    variant: RunnerVariant,
    block: int,
    position: int,
    boundary: str,
) -> None:
    if (
        value.boundary != boundary
        or value.workload_id != manifest.workload_id
        or value.workload_sha256 != manifest.stream_sha256
        or value.binary_sha256 != executable_digest
        or value.environment_sha256 != environment_digest
        or value.host_context_sha256 != environment.host_context_sha256
        or value.suite_label != suite_label
        or value.run_label != run_label
        or value.variant != variant
        or value.block_index != block
        or value.block_position != position
        or value.preload_commands != manifest.preload_commands
        or value.warmup_commands != manifest.warmup_commands
    ):
        raise ValueError("observation identity or region metadata differs from its invocation")
    validate_observation_against_workload(
        value,
        manifest,
        manifest_sha256=manifest_document_digest,
    )
