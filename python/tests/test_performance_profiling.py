from __future__ import annotations

from pathlib import Path
from typing import cast

import pytest
from atlaslob.performance import profiling
from atlaslob.performance.schemas import (
    Observation,
    file_sha256,
    measurement_parameters_for_boundary,
    write_canonical_document,
)
from atlaslob.performance.suite import Runner, _ProcessCapture
from atlaslob.performance.workloads import materialize_workload, verify_workload_manifest


def _observation(manifest_path: Path, *, valid: bool = True) -> Observation:
    manifest = verify_workload_manifest(manifest_path)
    return Observation(
        boundary="core_throughput",
        timed_input_kind="none",
        timed_input_sha256=None,
        measurement_parameters=measurement_parameters_for_boundary(manifest, "core_throughput"),
        workload_id=manifest.workload_id,
        workload_sha256=manifest.stream_sha256,
        workload_manifest_sha256=file_sha256(manifest_path),
        binary_sha256="a" * 64,
        environment_sha256="b" * 64,
        host_context_sha256="c" * 64,
        suite_label="profile01",
        run_label="profile01-run",
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
        failure_reason=None if valid else "perf permission denied",
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


def test_perf_prefix_freezes_counters_and_dwarf_call_graph(tmp_path: Path) -> None:
    perf = tmp_path / "perf"
    output = tmp_path / "result"

    stat = profiling.perf_prefix(perf, kind="stat", output=output)
    record = profiling.perf_prefix(perf, kind="record", output=output)

    assert stat[-1] == record[-1] == "--"
    assert ",".join(profiling.PERF_COUNTERS) in stat
    assert ("-g", "--call-graph", "dwarf") == record[3:6]
    with pytest.raises(ValueError, match="stat or record"):
        profiling.perf_prefix(perf, kind=cast(profiling.ProfileKind, "unknown"), output=output)


def test_capture_profile_retains_stat_and_sanitized_record_outputs(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    manifest_path = _manifest(tmp_path)
    perf = tmp_path / "perf"
    perf.write_bytes(b"placeholder")
    runner = Runner(Path("atlas_bench_runner"), Path("environment.json"), "standalone")
    monkeypatch.setattr(profiling, "_prepare_runner", lambda *_: None)

    def fake_run_suite(
        _manifest_path: Path,
        output: Path,
        *,
        baseline: Runner,
        **_kwargs: object,
    ) -> tuple[Path, ...]:
        output.mkdir(parents=True)
        prefix = baseline.command_prefix
        artifact = Path(prefix[prefix.index("-o") + 1])
        if "stat" in prefix:
            unavailable_counter = "cache-misses" if output.name == "attempt-00002" else None
            artifact.write_text(
                "# /home/private/AtlasLOB/build/runner\n"
                + "".join(
                    f"{'<not supported>' if counter == unavailable_counter else '1'}"
                    f";;{counter};1;100.00\n"
                    for counter in profiling.PERF_COUNTERS
                ),
                encoding="utf-8",
                newline="\n",
            )
        else:
            artifact.write_bytes(b"perf-data")
        observation_path = output / "observation-00001.json"
        write_canonical_document(observation_path, _observation(manifest_path))
        return (observation_path,)

    monkeypatch.setattr(profiling, "run_suite", fake_run_suite)
    stat = profiling.capture_profile(
        manifest_path,
        tmp_path / "stat",
        runner=runner,
        perf_executable=perf,
        suite_label="profile01",
        kind="stat",
        observations=2,
    )
    assert stat.captures == 2
    assert all(path.is_file() for path in stat.observations)
    assert "/home/private" not in (stat.observations[0].parent / "perf-stat.txt").read_text(
        encoding="utf-8"
    )
    summary = (tmp_path / "stat" / "perf-counter-summary.txt").read_text(encoding="ascii")
    assert "cycles;2;0;1;1;1" in summary
    assert "cache-misses;1;1;1;1;1" in summary
    assert "limitation=cache-misses unavailable in 1 of 2 captures" in summary

    monkeypatch.setattr(
        profiling,
        "_run_bounded",
        lambda *_: (
            _ProcessCapture(
                0,
                b"symbol /home/private/AtlasLOB/build/runner\n",
                b"",
            ),
            None,
        ),
    )
    record = profiling.capture_profile(
        manifest_path,
        tmp_path / "record",
        runner=runner,
        perf_executable=perf,
        suite_label="profile01",
        kind="record",
        observations=1,
    )
    report = (record.observations[0].parent / "perf-report.txt").read_text(encoding="utf-8")
    assert "/home/private" not in report
    assert "<private-path>" in report


def test_capture_profile_stops_after_retaining_invalid_observation(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    manifest_path = _manifest(tmp_path)
    perf = tmp_path / "perf"
    perf.write_bytes(b"placeholder")
    monkeypatch.setattr(profiling, "_prepare_runner", lambda *_: None)

    def fail_run(
        _manifest_path: Path,
        output: Path,
        **_kwargs: object,
    ) -> tuple[Path, ...]:
        output.mkdir(parents=True)
        path = output / "observation-00001.json"
        write_canonical_document(path, _observation(manifest_path, valid=False))
        return (path,)

    monkeypatch.setattr(profiling, "run_suite", fail_run)
    output = tmp_path / "failed"
    with pytest.raises(ValueError, match="invalid runner observation"):
        profiling.capture_profile(
            manifest_path,
            output,
            runner=Runner(
                Path("atlas_bench_runner"),
                Path("environment.json"),
                "standalone",
            ),
            perf_executable=perf,
            suite_label="profile01",
            kind="stat",
            observations=1,
        )
    assert (output / "attempt-00001" / "observation-00001.json").is_file()
