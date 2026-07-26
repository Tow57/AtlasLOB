from __future__ import annotations

import os
from dataclasses import replace
from pathlib import Path

import pytest
from atlaslob.performance import bundle
from atlaslob.performance.analysis import (
    analyze_observations,
    render_report_markdown,
    render_report_svg,
)
from atlaslob.performance.schemas import (
    EnvironmentManifest,
    Observation,
    document_sha256,
    environment_to_dict,
    file_sha256,
    measurement_parameters_for_boundary,
    workload_to_dict,
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
        host_class="bundle-host",
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
        limitations=("synthetic bundle environment",),
    )


def _create_bundle(directory: Path) -> None:
    manifest_path, manifest = materialize_workload(
        "W04",
        directory,
        seed=5,
        preload_commands=20,
        warmup_commands=20,
        measured_commands=40,
        active_order_target=20,
    )
    environment = _environment()
    environment_path = directory / "environment.json"
    environment_digest = write_canonical_document(environment_path, environment)
    observation = Observation(
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
        suite_label="bundle01",
        run_label="bundle01-standalone-00001",
        variant="standalone",
        block_index=0,
        block_position=0,
        preload_commands=manifest.preload_commands,
        warmup_commands=manifest.warmup_commands,
        commands=manifest.measured_commands,
        events=manifest.expected_events,
        committed=manifest.expected_committed,
        rejected=manifest.expected_rejected,
        engine_errors=manifest.expected_engine_errors,
        elapsed_ns=1_000,
        rss_before_bytes=1_000,
        rss_after_bytes=2_000,
        peak_rss_bytes=2_500,
        latency_ns=None,
        allocations=None,
        event_digest=manifest.expected_event_digest,
        final_digest=manifest.expected_final_digest,
        valid=True,
        failure_reason=None,
    )
    observation_path = directory / "observation-00001.json"
    observation_digest = write_canonical_document(observation_path, observation)
    report = analyze_observations(
        (observation,),
        workloads=((document_sha256(workload_to_dict(manifest)), manifest),),
        environments=((document_sha256(environment_to_dict(environment)), environment),),
        source_digests=(observation_digest,),
    )
    report_path = directory / "report.json"
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
    bundle.write_inventory(directory)


def test_bundle_inventory_and_exact_source_closure(tmp_path: Path) -> None:
    _create_bundle(tmp_path)

    summary = bundle.verify_bundle(tmp_path)
    assert summary.workloads == 1
    assert summary.environments == 1
    assert summary.observations == 1
    assert summary.reports == 1

    extra = replace(
        _environment("b" * 64),
        limitations=("unreferenced synthetic environment",),
        host_context_sha256="",
    )
    write_canonical_document(tmp_path / "extra-environment.json", extra)
    bundle.write_inventory(tmp_path)
    with pytest.raises(ValueError, match="every environment"):
        bundle.verify_bundle(tmp_path)


def test_bundle_rejects_orphan_and_nonregenerating_renderings(tmp_path: Path) -> None:
    _create_bundle(tmp_path)
    (tmp_path / "orphan.md").write_text("orphan\n", encoding="ascii", newline="\n")
    bundle.write_inventory(tmp_path)
    with pytest.raises(ValueError, match="orphan Markdown"):
        bundle.verify_bundle(tmp_path)

    (tmp_path / "orphan.md").unlink()
    (tmp_path / "report.svg").write_text(
        "<svg></svg>\n",
        encoding="ascii",
        newline="\n",
    )
    bundle.write_inventory(tmp_path)
    with pytest.raises(ValueError, match="SVG does not regenerate"):
        bundle.verify_bundle(tmp_path)


def test_bundle_rejects_symlinks_and_reparse_points(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    target = tmp_path / "target"
    target.write_text("target\n", encoding="ascii", newline="\n")
    link = tmp_path / "link"
    try:
        os.symlink(target, link)
    except OSError:
        pass
    else:
        with pytest.raises(ValueError, match="symbolic links or junctions"):
            bundle.write_inventory(tmp_path)
        link.unlink()

    reparse = tmp_path / "simulated-junction"
    reparse.mkdir()
    original = bundle._is_link_or_reparse
    monkeypatch.setattr(
        bundle,
        "_is_link_or_reparse",
        lambda path: path == reparse or original(path),
    )
    with pytest.raises(ValueError, match="symbolic links or junctions"):
        bundle.write_inventory(tmp_path)


@pytest.mark.parametrize(
    ("command_type", "payload", "expected"),
    (
        (
            1,
            (
                (7).to_bytes(4, "big")
                + (11).to_bytes(8, "big")
                + (13).to_bytes(4, "big")
                + bytes((1, 1, 1, 1))
                + (-5).to_bytes(8, "big", signed=True)
                + (17).to_bytes(8, "big")
            ),
            ("N", 7, 11, 13, 1, 1, 1, 1, -5, 17),
        ),
        (
            2,
            (7).to_bytes(4, "big") + (11).to_bytes(8, "big") + (13).to_bytes(4, "big"),
            ("C", 7, 11, 13),
        ),
        (
            3,
            (7).to_bytes(4, "big")
            + (11).to_bytes(8, "big")
            + (12).to_bytes(8, "big")
            + (13).to_bytes(4, "big")
            + (-5).to_bytes(8, "big", signed=True)
            + (17).to_bytes(8, "big"),
            ("R", 7, 11, 12, 13, -5, 17),
        ),
    ),
)
def test_w10_log_payload_decoder_binds_every_command_field(
    command_type: int,
    payload: bytes,
    expected: tuple[int | str, ...],
) -> None:
    assert bundle._decode_log_command(command_type, payload) == expected
