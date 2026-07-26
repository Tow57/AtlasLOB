from __future__ import annotations

import os
import subprocess
import sys
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace

import pytest
from atlaslob.performance import environment as environment_module
from atlaslob.performance.schemas import (
    CatalogEntry,
    EnvironmentManifest,
    Observation,
    environment_from_dict,
    observation_from_dict,
    observation_to_dict,
    parse_canonical_document,
    read_canonical_document,
    write_canonical_document,
)
from atlaslob.performance.workloads import materialize_workload


def _environment(binary: str = "a" * 64) -> EnvironmentManifest:
    return EnvironmentManifest(
        commit="1" * 40,
        tag=None,
        dirty=False,
        binary_sha256=binary,
        os="Ubuntu 24.04.2 LTS",
        kernel="6.8.0",
        host_class="test-host-class",
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


def _observation(environment: EnvironmentManifest) -> Observation:
    return Observation(
        boundary="core_throughput",
        timed_input_kind="none",
        timed_input_sha256=None,
        measurement_parameters=(
            ("instrument_count", "1"),
            ("measured_start_active_order_count", "20"),
            ("sweep_depth", "0"),
        ),
        workload_id="W04",
        workload_sha256="b" * 64,
        workload_manifest_sha256="9" * 64,
        binary_sha256=environment.binary_sha256,
        environment_sha256="c" * 64,
        host_context_sha256=environment.host_context_sha256,
        suite_label="schema01",
        run_label="standalone-b00000-p0-r00001",
        variant="standalone",
        block_index=0,
        block_position=0,
        preload_commands=20,
        warmup_commands=20,
        commands=40,
        events=100,
        committed=40,
        rejected=0,
        engine_errors=0,
        elapsed_ns=10_000,
        rss_before_bytes=1_000,
        rss_after_bytes=2_000,
        peak_rss_bytes=2_500,
        latency_ns=None,
        allocations=None,
        event_digest="d" * 64,
        final_digest="e" * 64,
        valid=True,
        failure_reason=None,
    )


def test_environment_and_observation_round_trip_as_canonical_ascii(tmp_path: Path) -> None:
    environment = _environment()
    observation = _observation(environment)
    environment_path = tmp_path / "environment.json"
    observation_path = tmp_path / "observation.json"

    write_canonical_document(environment_path, environment)
    write_canonical_document(observation_path, observation)

    assert read_canonical_document(environment_path, environment_from_dict) == environment
    assert read_canonical_document(observation_path, observation_from_dict) == observation
    assert environment_path.read_bytes().endswith(b"\n")
    assert b": " not in environment_path.read_bytes()
    assert b", " not in environment_path.read_bytes()


def test_observation_rejects_suite_labels_longer_than_thirty_two_characters() -> None:
    with pytest.raises(ValueError, match="at most 32"):
        replace(_observation(_environment()), suite_label="s" * 33)


def test_canonical_decoder_rejects_duplicate_whitespace_and_overflow() -> None:
    observation = _observation(_environment())
    encoded = (
        __import__("json").dumps(
            observation_to_dict(observation),
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode("ascii")

    duplicate = encoded.replace(b"{", b'{"schema":"ATLAS_BENCH_OBSERVATION_V1",', 1)
    with pytest.raises(ValueError, match="duplicate"):
        parse_canonical_document(duplicate, observation_from_dict)
    with pytest.raises(ValueError, match="canonical"):
        parse_canonical_document(encoded.replace(b":", b": ", 1), observation_from_dict)
    overflow = encoded.replace(b'"commands":"40"', f'"commands":"{1 << 64}"'.encode())
    with pytest.raises(ValueError, match="representation"):
        parse_canonical_document(overflow, observation_from_dict)


def test_official_environment_qualification_is_fail_closed_and_private_text_rejected() -> None:
    official = replace(_environment(), classification="official", limitations=())
    assert official.classification == "official"

    with pytest.raises(ValueError, match="idle SMT sibling"):
        replace(
            official,
            classification="official",
            smt_sibling_idle=None,
            host_context_sha256="",
        )
    with pytest.raises(ValueError, match="private"):
        replace(
            _environment(),
            cpu_model="2001:db8::1",
            host_context_sha256="",
        )
    with pytest.raises(ValueError, match="private"):
        replace(
            _environment(),
            compiler=r"C:\Users\example-user\clang.exe",
            host_context_sha256="",
        )


@pytest.mark.parametrize(
    ("stdout", "expected"),
    (
        ("", False),
        (" M tracked.cpp\n?? new.cpp\n", True),
    ),
)
def test_git_dirty_accepts_clean_and_multiline_porcelain(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    stdout: str,
    expected: bool,
) -> None:
    def fake_run(*args: object, **kwargs: object) -> SimpleNamespace:
        return SimpleNamespace(returncode=0, stdout=stdout, stderr="")

    monkeypatch.setattr(subprocess, "run", fake_run)
    assert environment_module._git_dirty(tmp_path) is expected


def test_git_dirty_fails_closed_on_command_error(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    def fake_run(*args: object, **kwargs: object) -> SimpleNamespace:
        return SimpleNamespace(returncode=1, stdout="", stderr="failure")

    monkeypatch.setattr(subprocess, "run", fake_run)
    assert environment_module._git_dirty(tmp_path) is None


@pytest.mark.parametrize(
    ("returncode", "stdout", "expected"),
    ((0, "kvm\n", "detected"), (1, "none\n", "none"), (2, "", "unknown")),
)
def test_virtualization_detection_distinguishes_bare_metal_from_failure(
    monkeypatch: pytest.MonkeyPatch,
    returncode: int,
    stdout: str,
    expected: str,
) -> None:
    def fake_run(*args: object, **kwargs: object) -> SimpleNamespace:
        return SimpleNamespace(returncode=returncode, stdout=stdout, stderr="")

    monkeypatch.setattr(subprocess, "run", fake_run)
    assert environment_module._virtualization("Linux", "6.8", frozenset()) == expected


def test_schema_analysis_and_materialization_do_not_import_native_extension() -> None:
    code = """
import builtins
import sys
real_import = builtins.__import__
def guarded(name, *args, **kwargs):
    if name == "atlaslob._native_engine":
        raise AssertionError("native extension import attempted")
    return real_import(name, *args, **kwargs)
builtins.__import__ = guarded
import atlaslob.performance.analysis
import atlaslob.performance.schemas
import atlaslob.performance.workloads
assert "atlaslob._native_engine" not in sys.modules
"""
    completed = subprocess.run(
        [sys.executable, "-c", code],
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr


def test_schema_structural_bounds_fail_before_large_evidence_is_accepted(
    tmp_path: Path,
) -> None:
    _, manifest = materialize_workload(
        "W04",
        tmp_path,
        seed=7,
        preload_commands=20,
        warmup_commands=20,
        measured_commands=40,
        active_order_target=20,
    )
    with pytest.raises(ValueError, match="4096"):
        replace(
            manifest,
            catalog=tuple(CatalogEntry(index + 1, 100, 1, 100) for index in range(4_097)),
        )
    with pytest.raises(ValueError, match="study bound"):
        replace(manifest, measured_commands=100_000_000)

    environment = _environment()
    with pytest.raises(ValueError, match="below logical_cpus"):
        replace(environment, affinity=(16,), pinned_cpu=None, host_context_sha256="")
    with pytest.raises(ValueError, match="compiler_flags"):
        replace(
            environment,
            compiler_flags=("-O3",) * 16_385,
            host_context_sha256="",
        )
    with pytest.raises(ValueError, match="single-line ASCII"):
        replace(environment, cpu_model="x" * 4_097, host_context_sha256="")

    observation = _observation(environment)
    with pytest.raises(ValueError, match="200000"):
        replace(
            observation,
            boundary="core_latency",
            latency_ns=(1,) * 200_001,
        )
    with pytest.raises(ValueError, match="entry limit"):
        replace(
            observation,
            measurement_parameters=tuple((f"dimension-{index:03d}", "1") for index in range(65)),
        )


def test_python_identity_worker_output_is_bounded(tmp_path: Path) -> None:
    returncode, stdout, stderr = environment_module._bounded_identity_process(
        (sys.executable, "-c", "print('identity')"),
        cwd=tmp_path,
        environment=dict(os.environ),
    )
    assert returncode == 0
    assert stdout.splitlines() == [b"identity"]
    assert stderr == b""

    with pytest.raises(ValueError, match="exceeds"):
        environment_module._bounded_identity_process(
            (
                sys.executable,
                "-c",
                "import sys; sys.stdout.buffer.write(b'x' * 70000)",
            ),
            cwd=tmp_path,
            environment=dict(os.environ),
        )
