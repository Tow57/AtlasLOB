from __future__ import annotations

import json
import os
from collections.abc import Callable, Mapping
from pathlib import Path

import atlaslob.cli as cli
import pytest
from atlaslob.cli import main
from atlaslob.differential import (
    CampaignResult,
    EvidenceTransformer,
)
from atlaslob.differential import (
    rerun_exact_prefix as differential_rerun_exact_prefix,
)
from atlaslob.differential import (
    run_fixture as differential_run_fixture,
)
from atlaslob.domain import Command
from atlaslob.generation import WorkloadProfile, resolve_workload_spec
from atlaslob.native import OutputMode
from atlaslob.workload import CampaignCase, generate_workload, write_manifest


def _executable() -> Path:
    configured = os.environ.get("ATLAS_DIFF_NATIVE")
    if configured is not None:
        candidate = Path(configured)
        if candidate.is_file():
            return candidate.resolve()
        raise FileNotFoundError(
            f"ATLAS_DIFF_NATIVE does not name a native evidence executable: {configured}"
        )
    for candidate in (
        Path("build/dev-gcc/atlas_diff_native.exe"),
        Path("build/dev-gcc/atlas_diff_native"),
    ):
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError("build atlas_diff_native or set ATLAS_DIFF_NATIVE")


def _corrupt_final_state(
    _command: Command | None,
    record: Mapping[str, object],
) -> Mapping[str, object]:
    if record.get("kind") != "final":
        return record
    evidence = record.get("evidence")
    if not isinstance(evidence, dict):
        return record
    state = evidence.get("state")
    if not isinstance(state, dict):
        return record
    changed_state = dict(state)
    changed_state["active_order_count"] = "999"
    changed_evidence = dict(evidence)
    changed_evidence["state"] = changed_state
    return {**record, "evidence": changed_evidence}


def _faulted_fixture_runner(
    fault_modes: set[OutputMode],
    transformer: EvidenceTransformer = _corrupt_final_state,
) -> Callable[..., CampaignResult]:
    def faulted_run_fixture(
        executable: Path,
        workload_path: Path,
        evidence_path: Path,
        *,
        mode: OutputMode = "exact",
        case_name: str = "fixture",
        timeout: float | None = 60.0,
        evidence_transformer: EvidenceTransformer | None = None,
        native_evidence_path: Path | None = None,
        workload_manifest_path: Path | None = None,
        capture_native_evidence: bool = True,
    ) -> CampaignResult:
        return differential_run_fixture(
            executable,
            workload_path,
            evidence_path,
            mode=mode,
            case_name=case_name,
            timeout=timeout,
            evidence_transformer=(transformer if mode in fault_modes else evidence_transformer),
            native_evidence_path=native_evidence_path,
            workload_manifest_path=workload_manifest_path,
            capture_native_evidence=capture_native_evidence,
        )

    return faulted_run_fixture


def _corrupt_command_state_at(target_index: int) -> EvidenceTransformer:
    current_index = -1

    def transform(
        command: Command | None,
        record: Mapping[str, object],
    ) -> Mapping[str, object]:
        nonlocal current_index
        if command is None or record.get("kind") != "result":
            return record
        current_index += 1
        if current_index != target_index:
            return record
        evidence = record.get("evidence")
        if not isinstance(evidence, dict):
            return record
        state = evidence.get("state")
        if not isinstance(state, dict):
            return record
        changed_state = dict(state)
        changed_state["active_order_count"] = "999"
        return {**record, "evidence": {**evidence, "state": changed_state}}

    return transform


@pytest.mark.parametrize("mode", ["exact", "compact"])
def test_fixture_cli_writes_portable_passing_summary(
    mode: str,
    tmp_path: Path,
) -> None:
    workload = tmp_path / "workload.atlas"
    generate_workload(
        resolve_workload_spec(
            WorkloadProfile.INVALID_MIX,
            command_count=40,
            snapshot_interval=5,
        ),
        99,
        workload,
    )
    output = tmp_path / "output"

    status = main(
        [
            "fixture",
            "--native",
            str(_executable()),
            "--workload",
            str(workload),
            "--output",
            str(output),
            "--mode",
            mode,
            "--no-shrink",
        ]
    )

    assert status == 0
    report_text = (output / "result.json").read_text(encoding="ascii")
    report = json.loads(report_text)
    assert report["status"] == "passed"
    assert report["mode"] == mode
    assert report["workload_command_count"] == "40"
    assert report["commands_compared"] == "40"
    assert report["failure"] is None
    assert len(report["digests"]["command_records"]) == 64
    assert str(tmp_path) not in report_text
    assert (output / "reference.jsonl").is_file()
    assert not (output / "native.jsonl").exists()


def test_fixture_cli_reuse_removes_only_prior_runner_owned_failure_state(
    tmp_path: Path,
) -> None:
    workload = tmp_path / "workload.atlas"
    generate_workload(
        resolve_workload_spec(
            WorkloadProfile.INVALID_MIX,
            command_count=10,
            snapshot_interval=5,
        ),
        100,
        workload,
    )
    output = tmp_path / "output"
    source = output / "failure" / "original"
    exact_prefix = output / "failure" / "exact-prefix"
    source.mkdir(parents=True)
    exact_prefix.mkdir()
    (output / "result.json").write_text("stale result", encoding="ascii")
    (output / "failure" / "exact-prefix-result.json").write_text(
        "stale exact result",
        encoding="ascii",
    )
    for name in ("original.atlas", "report.json", "minimized.atlas"):
        (source / name).write_text("stale bundle", encoding="ascii")
        (source / f".{name}.tmp").write_text("stale temporary", encoding="ascii")
    for name in (
        "exact-prefix.atlas",
        "exact-prefix-reference.jsonl",
        "exact-prefix-reference-native.jsonl",
    ):
        (exact_prefix / name).write_text("stale exact replay", encoding="ascii")
    manual = source / "reproduction" / "notes.txt"
    manual.parent.mkdir()
    manual.write_text("keep me", encoding="ascii")

    status = main(
        [
            "fixture",
            "--native",
            str(_executable()),
            "--workload",
            str(workload),
            "--output",
            str(output),
            "--mode",
            "exact",
            "--no-shrink",
        ]
    )

    assert status == 0
    result = json.loads((output / "result.json").read_text(encoding="ascii"))
    assert result["status"] == "passed"
    assert manual.read_text(encoding="ascii") == "keep me"
    assert not (output / "failure" / "exact-prefix-result.json").exists()
    assert not exact_prefix.exists()
    assert not (source / "original.atlas").exists()
    assert not (source / "report.json").exists()
    assert not (source / "minimized.atlas").exists()
    assert not tuple(source.glob(".*.tmp"))


def test_predefined_rotating_campaign_requires_an_explicit_epoch(
    tmp_path: Path,
) -> None:
    with pytest.raises(SystemExit) as error:
        main(
            [
                "predefined",
                "--native",
                str(_executable()),
                "--output",
                str(tmp_path / "output"),
                "--tier",
                "main",
                "--no-shrink",
            ]
        )

    assert error.value.code == 2


@pytest.mark.parametrize("value", ["nan", "inf", "-inf"])
def test_cli_rejects_nonfinite_timeouts(value: str, tmp_path: Path) -> None:
    with pytest.raises(SystemExit) as error:
        main(
            [
                "fixture",
                "--native",
                str(_executable()),
                "--workload",
                str(tmp_path / "unused.atlas"),
                "--output",
                str(tmp_path / "output"),
                "--case-timeout",
                value,
            ]
        )

    assert error.value.code == 2


def test_fixture_cli_rejects_an_output_directory_containing_the_workload(
    tmp_path: Path,
) -> None:
    workload = tmp_path / "workload.atlas"
    generate_workload(
        resolve_workload_spec(WorkloadProfile.UNIFORM_SYNTHETIC, command_count=2),
        1,
        workload,
    )
    before = workload.read_bytes()

    with pytest.raises(SystemExit) as error:
        main(
            [
                "fixture",
                "--native",
                str(_executable()),
                "--workload",
                str(workload),
                "--output",
                str(tmp_path),
            ]
        )

    assert error.value.code == 2
    assert workload.read_bytes() == before


@pytest.mark.parametrize(
    ("mode", "source_directory", "exact_prefix_required"),
    [
        ("compact", "compact", True),
        ("exact", "original", False),
    ],
)
def test_final_divergence_exits_one_and_runs_a_full_exact_diagnosis(
    mode: OutputMode,
    source_directory: str,
    exact_prefix_required: bool,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    workload = tmp_path / "workload.atlas"
    generate_workload(
        resolve_workload_spec(
            WorkloadProfile.INVALID_MIX,
            command_count=20,
            snapshot_interval=5,
        ),
        7,
        workload,
    )
    output = tmp_path / "output"
    monkeypatch.setattr(cli, "run_fixture", _faulted_fixture_runner({mode}))

    status = main(
        [
            "fixture",
            "--native",
            str(_executable()),
            "--workload",
            str(workload),
            "--output",
            str(output),
            "--mode",
            mode,
            "--no-shrink",
        ]
    )

    assert status == 1
    result = json.loads((output / "result.json").read_text(encoding="ascii"))
    assert result["failure"]["command_index"] is None
    assert result["failure"]["exact_prefix_required"] is exact_prefix_required
    assert result["failure_bundle"] == f"failure/{source_directory}"
    assert result["diagnosis"] == {
        "status": "exact_replay_passed",
        "exact_replay_command_count": "20",
        "exact_replay_command_limit": None,
    }
    exact = json.loads(
        (output / "failure" / "exact-prefix-result.json").read_text(encoding="ascii")
    )
    assert exact["status"] == "passed"
    assert exact["workload_command_count"] == "20"
    assert (output / "failure" / source_directory / "report.json").is_file()


def test_exact_replay_command_limit_defers_a_terminal_mismatch(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    workload = tmp_path / "workload.atlas"
    generate_workload(
        resolve_workload_spec(
            WorkloadProfile.INVALID_MIX,
            command_count=20,
            snapshot_interval=5,
        ),
        71,
        workload,
    )
    output = tmp_path / "output"
    monkeypatch.setattr(cli, "run_fixture", _faulted_fixture_runner({"exact"}))

    status = main(
        [
            "fixture",
            "--native",
            str(_executable()),
            "--workload",
            str(workload),
            "--output",
            str(output),
            "--mode",
            "exact",
            "--exact-replay-command-limit",
            "10",
            "--no-shrink",
        ]
    )

    assert status == 1
    result = json.loads((output / "result.json").read_text(encoding="ascii"))
    assert result["diagnosis"] == {
        "status": "deferred_command_limit",
        "exact_replay_command_count": "20",
        "exact_replay_command_limit": "10",
    }
    assert not (output / "failure" / "exact-prefix-result.json").exists()
    report = json.loads(
        (output / "failure" / "original" / "report.json").read_text(encoding="ascii")
    )
    assert report["diagnosis"] == result["diagnosis"]
    assert report["shrink"]["automatic_command_limit"] is None


def test_exact_replay_command_limit_uses_the_failing_prefix_length(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    workload = tmp_path / "workload.atlas"
    generate_workload(
        resolve_workload_spec(
            WorkloadProfile.INVALID_MIX,
            command_count=20,
            snapshot_interval=5,
        ),
        72,
        workload,
    )
    output = tmp_path / "output"
    transformer = _corrupt_command_state_at(4)
    monkeypatch.setattr(
        cli,
        "run_fixture",
        _faulted_fixture_runner({"exact"}, transformer),
    )

    status = main(
        [
            "fixture",
            "--native",
            str(_executable()),
            "--workload",
            str(workload),
            "--output",
            str(output),
            "--mode",
            "exact",
            "--exact-replay-command-limit",
            "4",
            "--no-shrink",
        ]
    )

    assert status == 1
    result = json.loads((output / "result.json").read_text(encoding="ascii"))
    assert result["failure"]["command_index"] == "4"
    assert result["diagnosis"] == {
        "status": "deferred_command_limit",
        "exact_replay_command_count": "5",
        "exact_replay_command_limit": "4",
    }
    assert not (output / "failure" / "exact-prefix-result.json").exists()


def test_required_exact_replay_harness_error_exits_two(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    workload = tmp_path / "workload.atlas"
    generate_workload(
        resolve_workload_spec(
            WorkloadProfile.INVALID_MIX,
            command_count=20,
            snapshot_interval=5,
        ),
        8,
        workload,
    )
    output = tmp_path / "output"
    monkeypatch.setattr(cli, "run_fixture", _faulted_fixture_runner({"exact"}))

    def failed_exact_replay(
        _executable_path: Path,
        source_workload: Path,
        _failing_command_index: int | None,
        replay_output: Path,
        *,
        case_name: str = "exact-prefix",
        timeout: float | None = 60.0,
        capture_native_evidence: bool = True,
    ) -> CampaignResult:
        del capture_native_evidence
        return differential_run_fixture(
            replay_output / "missing-native",
            source_workload,
            replay_output / "failed-reference.jsonl",
            mode="exact",
            case_name=case_name,
            timeout=timeout,
        )

    monkeypatch.setattr(cli, "rerun_exact_prefix", failed_exact_replay)
    status = main(
        [
            "fixture",
            "--native",
            str(_executable()),
            "--workload",
            str(workload),
            "--output",
            str(output),
            "--mode",
            "exact",
            "--no-shrink",
        ]
    )

    assert status == 2
    result = json.loads((output / "result.json").read_text(encoding="ascii"))
    assert result["status"] == "harness_error"
    assert result["failure"] is not None
    assert result["failure_bundle"] == "failure/original"
    assert "FileNotFoundError" in result["harness_error"]
    exact = json.loads(
        (output / "failure" / "exact-prefix-result.json").read_text(encoding="ascii")
    )
    assert exact["status"] == "harness_error"
    assert (output / "failure" / "original" / "report.json").is_file()


def test_summary_only_retains_and_diagnoses_a_failing_case(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    workload = tmp_path / "failure-source.atlas"
    manifest = generate_workload(
        resolve_workload_spec(
            WorkloadProfile.INVALID_MIX,
            command_count=20,
            snapshot_interval=5,
        ),
        9,
        workload,
    )
    manifest_path = tmp_path / "failure-manifest.json"
    write_manifest(manifest_path, manifest)
    failure = differential_run_fixture(
        _executable(),
        workload,
        tmp_path / "failure-reference.jsonl",
        mode="exact",
        timeout=30,
        evidence_transformer=_corrupt_final_state,
        workload_manifest_path=manifest_path,
    )
    assert failure.status == "diverged"
    native_evidence = failure.native_evidence_path
    assert native_evidence is not None

    def failed_case(
        _executable_path: Path,
        _case: CampaignCase,
        _output_directory: Path,
        *,
        mode: OutputMode,
        timeout: float | None = 60.0,
        capture_native_evidence: bool = True,
    ) -> CampaignResult:
        del mode, timeout
        assert not capture_native_evidence
        return failure

    monkeypatch.setattr(cli, "run_case", failed_case)

    def observed_rerun(
        executable_path: Path,
        source_workload: Path,
        failing_command_index: int | None,
        replay_output: Path,
        *,
        case_name: str = "exact-prefix",
        timeout: float | None = 60.0,
        capture_native_evidence: bool = True,
    ) -> CampaignResult:
        assert not capture_native_evidence
        return differential_rerun_exact_prefix(
            executable_path,
            source_workload,
            failing_command_index,
            replay_output,
            case_name=case_name,
            timeout=timeout,
            capture_native_evidence=capture_native_evidence,
        )

    monkeypatch.setattr(cli, "rerun_exact_prefix", observed_rerun)
    output = tmp_path / "output"
    status = main(
        [
            "predefined",
            "--native",
            str(_executable()),
            "--output",
            str(output),
            "--tier",
            "pr",
            "--case-index",
            "0",
            "--summary-only",
            "--exact-replay-command-limit",
            "20",
            "--no-shrink",
        ]
    )

    assert status == 1
    summary = json.loads((output / "summary.json").read_text(encoding="ascii"))
    assert summary["cases"][0]["status"] == "diverged"
    assert summary["cases"][0]["failure_bundle"].endswith("/original")
    assert summary["cases"][0]["diagnosis"] == {
        "status": "exact_replay_passed",
        "exact_replay_command_count": "20",
        "exact_replay_command_limit": "20",
    }
    assert not failure.workload_path.exists()
    assert not failure.reference_evidence_path.exists()
    assert not native_evidence.exists()
    assert manifest_path.is_file()
    diagnosis = output / "failures" / "00-uniform_synthetic"
    assert (diagnosis / "exact-prefix-result.json").is_file()
    source_bundle = diagnosis / "original"
    source_report = json.loads((source_bundle / "report.json").read_text(encoding="ascii"))
    assert (source_bundle / "original.atlas").is_file()
    assert (source_bundle / "manifest.json").is_file()
    assert source_report["transcript_policy"] == "omitted"
    assert source_report["files"]["original_reference_output"] is None
    assert source_report["files"]["original_native_output"] is None
    assert source_report["artifact_sha256"]["original_reference_output"] is None
    assert source_report["artifact_sha256"]["original_native_output"] is None
    assert not (source_bundle / "reference-original.jsonl").exists()
    assert not (source_bundle / "native-original.jsonl").exists()
    exact_directory = diagnosis / "exact-prefix"
    assert not (exact_directory / "exact-prefix.atlas").exists()
    assert not (exact_directory / "exact-prefix-reference.jsonl").exists()
    assert not (exact_directory / "exact-prefix-reference-native.jsonl").exists()


def test_summary_only_deferred_failure_keeps_only_portable_source_bundle(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    workload = tmp_path / "deferred-source.atlas"
    manifest = generate_workload(
        resolve_workload_spec(
            WorkloadProfile.INVALID_MIX,
            command_count=20,
            snapshot_interval=5,
        ),
        91,
        workload,
    )
    manifest_path = tmp_path / "deferred-manifest.json"
    write_manifest(manifest_path, manifest)
    failure = differential_run_fixture(
        _executable(),
        workload,
        tmp_path / "deferred-reference.jsonl",
        mode="exact",
        timeout=30,
        evidence_transformer=_corrupt_final_state,
        workload_manifest_path=manifest_path,
    )
    native_evidence = failure.native_evidence_path
    assert failure.divergence is not None
    assert native_evidence is not None

    def failed_case(
        _executable_path: Path,
        _case: CampaignCase,
        _output_directory: Path,
        *,
        mode: OutputMode,
        timeout: float | None = 60.0,
        capture_native_evidence: bool = True,
    ) -> CampaignResult:
        del mode, timeout
        assert not capture_native_evidence
        return failure

    monkeypatch.setattr(cli, "run_case", failed_case)
    output = tmp_path / "output"
    prior_diagnosis = output / "failures" / "00-uniform_synthetic"
    prior_diagnosis.mkdir(parents=True)
    (prior_diagnosis / "exact-prefix-result.json").write_text(
        "stale exact replay result",
        encoding="ascii",
    )
    status = main(
        [
            "predefined",
            "--native",
            str(_executable()),
            "--output",
            str(output),
            "--tier",
            "pr",
            "--case-index",
            "0",
            "--summary-only",
            "--exact-replay-command-limit",
            "10",
            "--no-shrink",
        ]
    )

    assert status == 1
    summary = json.loads((output / "summary.json").read_text(encoding="ascii"))
    assert summary["cases"][0]["diagnosis"] == {
        "status": "deferred_command_limit",
        "exact_replay_command_count": "20",
        "exact_replay_command_limit": "10",
    }
    diagnosis = output / "failures" / "00-uniform_synthetic"
    source_bundle = diagnosis / "original"
    assert (source_bundle / "original.atlas").is_file()
    assert (source_bundle / "manifest.json").is_file()
    assert (source_bundle / "report.json").is_file()
    assert not (source_bundle / "reference-original.jsonl").exists()
    assert not (source_bundle / "native-original.jsonl").exists()
    assert not (diagnosis / "exact-prefix-result.json").exists()
    assert not failure.workload_path.exists()
    assert not failure.reference_evidence_path.exists()
    assert not native_evidence.exists()
    assert manifest_path.is_file()


def test_summary_only_does_not_delete_source_evidence_before_persistence(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    workload = tmp_path / "failure-source.atlas"
    generate_workload(
        resolve_workload_spec(
            WorkloadProfile.INVALID_MIX,
            command_count=5,
            snapshot_interval=5,
        ),
        10,
        workload,
    )
    failure = differential_run_fixture(
        _executable(),
        workload,
        tmp_path / "failure-reference.jsonl",
        mode="exact",
        timeout=30,
        evidence_transformer=_corrupt_final_state,
    )
    native_evidence = failure.native_evidence_path
    assert failure.divergence is not None
    assert native_evidence is not None

    def failed_case(
        _executable_path: Path,
        _case: CampaignCase,
        _output_directory: Path,
        *,
        mode: OutputMode,
        timeout: float | None = 60.0,
        capture_native_evidence: bool = True,
    ) -> CampaignResult:
        del mode, timeout, capture_native_evidence
        return failure

    def failed_persistence(*_args: object, **_kwargs: object) -> None:
        assert failure.workload_path.is_file()
        assert failure.reference_evidence_path.is_file()
        assert native_evidence.is_file()
        raise RuntimeError("injected persistence failure")

    monkeypatch.setattr(cli, "run_case", failed_case)
    monkeypatch.setattr(cli, "persist_initial_failure", failed_persistence)
    with pytest.raises(SystemExit) as error:
        main(
            [
                "predefined",
                "--native",
                str(_executable()),
                "--output",
                str(tmp_path / "output"),
                "--tier",
                "pr",
                "--case-index",
                "0",
                "--summary-only",
                "--no-shrink",
            ]
        )

    assert error.value.code == 2
    assert failure.workload_path.is_file()
    assert failure.reference_evidence_path.is_file()
    assert native_evidence.is_file()


def test_summary_only_harness_failure_keeps_reproducer_but_discards_transcripts(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    workload = tmp_path / "harness-source.atlas"
    manifest = generate_workload(
        resolve_workload_spec(
            WorkloadProfile.INVALID_MIX,
            command_count=5,
            snapshot_interval=5,
        ),
        11,
        workload,
    )
    manifest_path = tmp_path / "harness-manifest.json"
    write_manifest(manifest_path, manifest)
    failure = differential_run_fixture(
        tmp_path / "missing-native",
        workload,
        tmp_path / "harness-reference.jsonl",
        mode="exact",
        timeout=30,
        workload_manifest_path=manifest_path,
    )
    assert failure.status == "harness_error"
    assert failure.divergence is None
    assert failure.reference_evidence_path.is_file()

    def failed_case(
        _executable_path: Path,
        _case: CampaignCase,
        _output_directory: Path,
        *,
        mode: OutputMode,
        timeout: float | None = 60.0,
        capture_native_evidence: bool = True,
    ) -> CampaignResult:
        del mode, timeout, capture_native_evidence
        return failure

    monkeypatch.setattr(cli, "run_case", failed_case)
    output = tmp_path / "output"
    status = main(
        [
            "predefined",
            "--native",
            str(_executable()),
            "--output",
            str(output),
            "--tier",
            "pr",
            "--case-index",
            "0",
            "--summary-only",
            "--no-shrink",
        ]
    )

    assert status == 2
    summary = json.loads((output / "summary.json").read_text(encoding="ascii"))
    assert summary["cases"][0]["status"] == "harness_error"
    assert failure.workload_path.is_file()
    assert manifest_path.is_file()
    assert not failure.reference_evidence_path.exists()
    if failure.native_evidence_path is not None:
        assert not failure.native_evidence_path.exists()


@pytest.mark.campaign
def test_predefined_case_shard_retains_only_summary_and_manifest(
    tmp_path: Path,
) -> None:
    output = tmp_path / "output"
    stale_case = output / "cases" / "prior-case"
    stale_bundle = output / "failures" / "prior-case" / "original"
    stale_exact = output / "failures" / "prior-case" / "exact-prefix"
    stale_case.mkdir(parents=True)
    stale_bundle.mkdir(parents=True)
    stale_exact.mkdir()
    (output / "campaign.json").write_text("stale campaign", encoding="ascii")
    (output / "summary.json").write_text("stale summary", encoding="ascii")
    for name in ("workload.atlas", "manifest.json", "reference.jsonl", "reference-native.jsonl"):
        (stale_case / name).write_text("stale case", encoding="ascii")
        (stale_case / f".{name}.tmp").write_text("stale temporary", encoding="ascii")
    (stale_bundle.parent / "exact-prefix-result.json").write_text(
        "stale exact result",
        encoding="ascii",
    )
    for name in ("original.atlas", "report.json", "minimized.atlas"):
        (stale_bundle / name).write_text("stale bundle", encoding="ascii")
    for name in (
        "exact-prefix.atlas",
        "exact-prefix-reference.jsonl",
        "exact-prefix-reference-native.jsonl",
    ):
        (stale_exact / name).write_text("stale exact replay", encoding="ascii")
    manual = stale_bundle / "reproduction" / "notes.txt"
    manual.parent.mkdir()
    manual.write_text("keep me", encoding="ascii")

    status = main(
        [
            "predefined",
            "--native",
            str(_executable()),
            "--output",
            str(output),
            "--tier",
            "pr",
            "--case-index",
            "3",
            "--summary-only",
            "--case-timeout",
            "60",
        ]
    )

    assert status == 0
    summary = json.loads((output / "summary.json").read_text(encoding="ascii"))
    assert summary["artifact_policy"] == "summary_only"
    assert summary["campaign_case_count"] == "10"
    assert summary["selected_case_indices"] == ["3"]
    assert summary["expected_case_count"] == "1"
    assert summary["completed_case_count"] == "1"
    assert summary["passed"] is True
    assert [case["case_name"] for case in summary["cases"]] == ["03-sparse_wide"]
    manifest_path = summary["cases"][0]["manifest"]
    assert isinstance(manifest_path, str)
    assert manifest_path != "manifest.json"
    assert (output / manifest_path).is_file()

    case_directories = tuple((output / "cases").iterdir())
    assert len(case_directories) == 1
    assert (case_directories[0] / "manifest.json").is_file()
    assert not (case_directories[0] / "workload.atlas").exists()
    assert not (case_directories[0] / "reference.jsonl").exists()
    assert not tuple(case_directories[0].glob("*native*.jsonl"))
    assert not tuple(case_directories[0].glob(".*.tmp"))
    assert not stale_case.exists()
    assert manual.read_text(encoding="ascii") == "keep me"
    assert not (stale_bundle.parent / "exact-prefix-result.json").exists()
    assert not stale_exact.exists()
    assert not (stale_bundle / "original.atlas").exists()
    assert not (stale_bundle / "report.json").exists()
    assert not (stale_bundle / "minimized.atlas").exists()


def test_predefined_case_shard_rejects_an_out_of_range_index(
    tmp_path: Path,
) -> None:
    with pytest.raises(SystemExit) as error:
        main(
            [
                "predefined",
                "--native",
                str(_executable()),
                "--output",
                str(tmp_path / "output"),
                "--tier",
                "pr",
                "--case-index",
                "10",
                "--summary-only",
            ]
        )

    assert error.value.code == 2


def test_campaign_reuse_does_not_follow_a_runner_directory_symlink(
    tmp_path: Path,
) -> None:
    output = tmp_path / "output"
    output.mkdir()
    protected = tmp_path / "protected"
    protected.mkdir()
    sentinel = protected / "workload.atlas"
    sentinel.write_text("do not delete", encoding="ascii")
    try:
        (output / "cases").symlink_to(protected, target_is_directory=True)
    except OSError as exc:
        pytest.skip(f"directory symlinks are unavailable: {exc}")

    with pytest.raises(SystemExit) as error:
        main(
            [
                "predefined",
                "--native",
                str(_executable()),
                "--output",
                str(output),
                "--tier",
                "pr",
                "--case-index",
                "3",
                "--summary-only",
            ]
        )

    assert error.value.code == 2
    assert sentinel.read_text(encoding="ascii") == "do not delete"
    assert not (output / "campaign.json").exists()
