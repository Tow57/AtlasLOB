from __future__ import annotations

import io
import json
import os
import subprocess
import sys
import time
from collections.abc import Iterator, Mapping
from pathlib import Path
from typing import IO

import atlaslob.differential as differential
import pytest
from atlaslob.canonical import event_digest
from atlaslob.differential import (
    CampaignResult,
    first_depth_difference,
    first_value_difference,
    rerun_exact_prefix,
    run_case,
    run_fixture,
    run_suite,
)
from atlaslob.domain import (
    AcceptedEvent,
    Command,
    CommandType,
    EventBatch,
    EventHeader,
    ReferenceResult,
    RejectedEvent,
    RejectReason,
    Side,
    TradeEvent,
)
from atlaslob.generation import WorkloadProfile, resolve_workload_spec
from atlaslob.native import NativeProtocolError, OutputMode
from atlaslob.reference import ReferenceEngine
from atlaslob.workload import CampaignCase, CampaignSuite, WorkloadReader, generate_workload


def _executable() -> Path:
    configured = os.environ.get("ATLAS_DIFF_NATIVE")
    if configured is not None:
        candidate = Path(configured)
        if not candidate.is_file():
            raise FileNotFoundError(
                f"ATLAS_DIFF_NATIVE does not name a native evidence executable: {configured}"
            )
        return candidate.resolve()
    for candidate in (
        Path("build/dev-gcc/atlas_diff_native.exe"),
        Path("build/dev-gcc/atlas_diff_native"),
    ):
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError("atlas_diff_native has not been built")


def _workload(
    tmp_path: Path,
    *,
    profile: WorkloadProfile = WorkloadProfile.INVALID_MIX,
    command_count: int = 60,
    snapshot_interval: int = 7,
    seed: int = 123,
) -> Path:
    path = tmp_path / "workload.atlas"
    generate_workload(
        resolve_workload_spec(
            profile,
            command_count=command_count,
            snapshot_interval=snapshot_interval,
        ),
        seed,
        path,
    )
    return path


@pytest.mark.parametrize("mode", ["exact", "compact"])
def test_fixture_streams_equal_reference_and_native_evidence(
    tmp_path: Path,
    mode: OutputMode,
) -> None:
    workload = _workload(tmp_path)
    result = run_fixture(
        _executable(),
        workload,
        tmp_path / f"reference-{mode}.jsonl",
        mode=mode,
        timeout=30,
    )

    assert result.passed
    assert result.status == "passed"
    assert result.divergence is None
    assert result.harness_error is None
    assert result.commands_compared == 60
    assert result.workload_command_count == 60
    assert result.reference_classifications == result.native_classifications
    assert result.reference_evidence_digest == result.native_evidence_digest
    assert result.native_returncode == 0
    assert result.reference_evidence_path.is_file()
    assert result.native_evidence_path is None
    assert not (tmp_path / f"reference-{mode}-native.jsonl").exists()
    records = [
        json.loads(line)
        for line in result.reference_evidence_path.read_text(encoding="ascii").splitlines()
    ]
    assert records[0]["kind"] == "config"
    assert records[-1]["kind"] == "final"
    assert records[-1]["command_digest"] == result.command_digest
    assert records[-1]["evidence_digest"] == result.reference_evidence_digest


def test_compact_mode_compares_the_explicit_l0_rejection_reason(tmp_path: Path) -> None:
    workload = _workload(
        tmp_path,
        profile=WorkloadProfile.INVALID_MIX,
        command_count=100,
    )

    def replace_rejection_reason(
        _command: Command | None,
        record: Mapping[str, object],
    ) -> Mapping[str, object]:
        evidence = record.get("evidence")
        if not isinstance(evidence, dict) or evidence.get("outcome") != "rejected":
            return record
        changed = dict(evidence)
        changed["reject_reason"] = (
            "invalid_price"
            if changed.get("reject_reason") != "invalid_price"
            else "invalid_quantity"
        )
        return {**record, "evidence": changed}

    result = run_fixture(
        _executable(),
        workload,
        tmp_path / "compact-reference.jsonl",
        mode="compact",
        timeout=30,
        evidence_transformer=replace_rejection_reason,
    )

    assert result.status == "diverged"
    assert result.divergence is not None
    assert result.divergence.signature.category == "classification"
    assert result.divergence.difference.path.endswith("reject_reason")
    assert result.divergence.exact_prefix_required


def test_supplied_workload_manifest_is_required_and_verified(tmp_path: Path) -> None:
    workload = _workload(tmp_path, command_count=5)
    missing_manifest = tmp_path / "missing-manifest.json"

    result = run_fixture(
        _executable(),
        workload,
        tmp_path / "reference.jsonl",
        mode="exact",
        timeout=30,
        workload_manifest_path=missing_manifest,
    )

    assert result.status == "harness_error"
    assert result.workload_manifest_path == missing_manifest.resolve()
    assert result.commands_compared == 0
    assert result.harness_error is not None
    assert "cannot read canonical JSON" in result.harness_error


def test_fixture_rejects_artifact_collisions_before_mutating_inputs(
    tmp_path: Path,
) -> None:
    workload = _workload(tmp_path, command_count=5)
    workload_before = workload.read_bytes()
    executable = _executable()
    executable_before = executable.read_bytes()

    with pytest.raises(ValueError, match="collides"):
        run_fixture(
            executable,
            workload,
            workload,
            mode="exact",
        )
    with pytest.raises(ValueError, match="collides"):
        run_fixture(
            executable,
            workload,
            tmp_path / "reference.jsonl",
            mode="exact",
            native_evidence_path=executable,
        )
    with pytest.raises(ValueError, match="collides"):
        run_fixture(
            executable,
            workload,
            tmp_path / "reference.jsonl",
            mode="exact",
            native_evidence_path=workload,
            capture_native_evidence=False,
        )

    assert workload.read_bytes() == workload_before
    assert executable.read_bytes() == executable_before


def test_failed_recapture_removes_stale_reference_and_native_outputs(
    tmp_path: Path,
) -> None:
    workload = _workload(tmp_path, command_count=5)
    reference = tmp_path / "reference.jsonl"
    native = tmp_path / "native.jsonl"
    reference.write_text("stale-reference", encoding="ascii")
    native.write_text("stale-native", encoding="ascii")

    result = run_fixture(
        _executable(),
        workload,
        reference,
        mode="exact",
        native_evidence_path=native,
        workload_manifest_path=tmp_path / "missing-manifest.json",
    )

    assert result.status == "harness_error"
    assert not reference.exists()
    assert not native.exists()


@pytest.mark.parametrize("explicit_native_path", [False, True])
def test_disabled_native_capture_removes_stale_native_outputs(
    tmp_path: Path,
    explicit_native_path: bool,
) -> None:
    workload = _workload(tmp_path, command_count=5)
    reference = tmp_path / "reference.jsonl"
    native = (
        tmp_path / "explicit-native.jsonl"
        if explicit_native_path
        else tmp_path / "reference-native.jsonl"
    )
    native_temporary = native.with_name(f".{native.name}.tmp")
    native.write_text("stale-native", encoding="ascii")
    native_temporary.write_text("stale-native-temporary", encoding="ascii")

    result = run_fixture(
        _executable(),
        workload,
        reference,
        mode="exact",
        native_evidence_path=(native if explicit_native_path else None),
        capture_native_evidence=False,
    )

    assert result.passed
    assert result.native_evidence_path is None
    assert not native.exists()
    assert not native_temporary.exists()


def test_reference_generation_is_included_in_the_case_timeout(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    workload = _workload(tmp_path, command_count=5)
    reference = tmp_path / "reference.jsonl"
    original_execute = ReferenceEngine.execute

    def slow_execute(self: ReferenceEngine, command: Command) -> ReferenceResult:
        time.sleep(0.01)
        return original_execute(self, command)

    monkeypatch.setattr(ReferenceEngine, "execute", slow_execute)

    result = run_fixture(
        _executable(),
        workload,
        reference,
        mode="exact",
        timeout=0.001,
    )

    assert result.status == "harness_error"
    assert result.harness_error is not None
    assert "timed out" in result.harness_error
    assert not reference.exists()


def test_native_line_pump_rejects_an_oversized_record_without_unbounded_read(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(differential, "_MAX_NATIVE_JSONL_RECORD_BYTES", 32)
    pump = differential._LinePump(io.BytesIO(b"x" * 33 + b"\n"))
    pump.start()
    item = pump.next(1)
    pump.stop()
    pump.join()

    assert isinstance(item, NativeProtocolError)
    assert "byte-size limit" in str(item)


def test_reference_spool_is_complete_before_native_spawn(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    workload = _workload(tmp_path, command_count=20)
    evidence = tmp_path / "reference.jsonl"
    original_spawn = differential._spawn_native
    observations: list[str] = []

    def checked_spawn(
        executable: Path,
        mode: OutputMode,
        native_input: IO[bytes],
        native_stderr: IO[bytes],
    ) -> subprocess.Popen[bytes]:
        assert evidence.is_file()
        records = evidence.read_text(encoding="ascii").splitlines()
        assert json.loads(records[-1])["kind"] == "final"
        observations.append("complete-reference-before-spawn")
        return original_spawn(executable, mode, native_input, native_stderr)

    monkeypatch.setattr(differential, "_spawn_native", checked_spawn)
    result = run_fixture(
        _executable(),
        workload,
        evidence,
        mode="exact",
        timeout=30,
    )

    assert result.passed
    assert observations == ["complete-reference-before-spawn"]


def test_reference_failure_prevents_native_spawn_and_removes_partial_spool(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    workload = _workload(tmp_path, command_count=10)
    evidence = tmp_path / "reference.jsonl"
    spawned = False

    def fail_execute(
        _engine: ReferenceEngine,
        _command: object,
    ) -> object:
        raise RuntimeError("injected reference failure")

    def unexpected_spawn(
        _executable: Path,
        _mode: OutputMode,
        _native_input: IO[bytes],
        _native_stderr: IO[bytes],
    ) -> subprocess.Popen[bytes]:
        nonlocal spawned
        spawned = True
        raise AssertionError("native process must not start")

    monkeypatch.setattr(ReferenceEngine, "execute", fail_execute)
    monkeypatch.setattr(differential, "_spawn_native", unexpected_spawn)
    result = run_fixture(
        _executable(),
        workload,
        evidence,
        mode="exact",
    )

    assert result.status == "harness_error"
    assert "injected reference failure" in (result.harness_error or "")
    assert not spawned
    assert not evidence.exists()
    assert not (tmp_path / ".reference.jsonl.tmp").exists()


def test_protocol_failure_is_structured_and_process_is_reaped(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    workload = _workload(tmp_path, command_count=10)
    processes: list[subprocess.Popen[bytes]] = []

    def invalid_spawn(
        _executable: Path,
        _mode: OutputMode,
        native_input: IO[bytes],
        native_stderr: IO[bytes],
    ) -> subprocess.Popen[bytes]:
        process = subprocess.Popen(
            [
                sys.executable,
                "-c",
                "import sys; sys.stdout.buffer.write(b'not-json\\n')",
            ],
            stdin=native_input,
            stdout=subprocess.PIPE,
            stderr=native_stderr,
        )
        processes.append(process)
        return process

    monkeypatch.setattr(differential, "_spawn_native", invalid_spawn)
    result = run_fixture(
        _executable(),
        workload,
        tmp_path / "reference.jsonl",
        mode="compact",
        timeout=10,
    )

    assert result.status == "harness_error"
    assert "NativeProtocolError" in (result.harness_error or "")
    assert processes and processes[0].poll() is not None
    assert result.native_evidence_path is not None
    assert result.native_evidence_path.read_bytes() == b"not-json\n"


def test_native_capture_can_be_disabled_for_bounded_storage(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    workload = _workload(tmp_path, command_count=10)

    def invalid_spawn(
        _executable: Path,
        _mode: OutputMode,
        native_input: IO[bytes],
        native_stderr: IO[bytes],
    ) -> subprocess.Popen[bytes]:
        return subprocess.Popen(
            [
                sys.executable,
                "-c",
                "import sys; sys.stdout.buffer.write(b'not-json\\n')",
            ],
            stdin=native_input,
            stdout=subprocess.PIPE,
            stderr=native_stderr,
        )

    monkeypatch.setattr(differential, "_spawn_native", invalid_spawn)
    native_evidence = tmp_path / "native.jsonl"
    result = run_fixture(
        _executable(),
        workload,
        tmp_path / "reference.jsonl",
        mode="compact",
        timeout=10,
        native_evidence_path=native_evidence,
        capture_native_evidence=False,
    )

    assert result.status == "harness_error"
    assert "NativeProtocolError" in (result.harness_error or "")
    assert result.native_evidence_path is None
    assert not native_evidence.exists()
    assert not (tmp_path / ".native.jsonl.tmp").exists()


def test_spawn_failure_occurs_only_after_reference_evidence_is_durable(
    tmp_path: Path,
) -> None:
    workload = _workload(tmp_path, command_count=10)
    evidence = tmp_path / "reference.jsonl"
    result = run_fixture(
        tmp_path / "missing-native-executable",
        workload,
        evidence,
        mode="exact",
    )

    assert result.status == "harness_error"
    assert "FileNotFoundError" in (result.harness_error or "")
    assert evidence.is_file()
    assert json.loads(evidence.read_text(encoding="ascii").splitlines()[-1])["kind"] == "final"


def test_structured_first_divergence_stops_and_reaps_native(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    workload = _workload(tmp_path, command_count=100)
    original_capture = differential.capture_reference
    original_spawn = differential._spawn_native
    processes: list[subprocess.Popen[bytes]] = []

    def tampered_capture(
        workload_path: Path,
        evidence_path: Path,
        *,
        mode: OutputMode,
        deadline: float | None = None,
    ) -> differential.ReferenceCapture:
        capture = original_capture(
            workload_path,
            evidence_path,
            mode=mode,
            deadline=deadline,
        )
        records = [
            json.loads(line) for line in capture.path.read_text(encoding="ascii").splitlines()
        ]
        for record in records:
            if record["kind"] == "result":
                record["evidence"]["state"]["active_order_count"] = "999999"
                break
        capture.path.write_text(
            "".join(
                json.dumps(
                    record,
                    ensure_ascii=True,
                    separators=(",", ":"),
                    sort_keys=True,
                )
                + "\n"
                for record in records
            ),
            encoding="ascii",
            newline="\n",
        )
        return capture

    def observed_spawn(
        executable: Path,
        mode: OutputMode,
        native_input: IO[bytes],
        native_stderr: IO[bytes],
    ) -> subprocess.Popen[bytes]:
        process = original_spawn(executable, mode, native_input, native_stderr)
        processes.append(process)
        return process

    monkeypatch.setattr(differential, "capture_reference", tampered_capture)
    monkeypatch.setattr(differential, "_spawn_native", observed_spawn)
    result = run_fixture(
        _executable(),
        workload,
        tmp_path / "reference.jsonl",
        mode="exact",
        timeout=30,
    )

    assert result.status == "diverged"
    assert result.divergence is not None
    assert result.divergence.command_index == 0
    assert result.divergence.signature.category == "state"
    assert result.divergence.difference.path.endswith("state.active_order_count")
    assert not result.divergence.exact_prefix_required
    assert processes and processes[0].poll() is not None


def test_compact_prefix_can_be_rerun_with_exact_events_and_divergent_checkpoint(
    tmp_path: Path,
) -> None:
    workload = _workload(tmp_path, command_count=25, snapshot_interval=10)
    compact = run_fixture(
        _executable(),
        workload,
        tmp_path / "compact-reference.jsonl",
        mode="compact",
        timeout=30,
    )
    exact = rerun_exact_prefix(
        _executable(),
        workload,
        12,
        tmp_path / "exact-rerun",
        timeout=30,
    )

    assert compact.passed
    assert exact.passed
    assert exact.mode == "exact"
    assert exact.workload_command_count == 13
    assert exact.reference_evidence_digest == exact.native_evidence_digest
    records = [
        json.loads(line)
        for line in exact.reference_evidence_path.read_text(encoding="ascii").splitlines()
    ]
    result_records = [record for record in records if record["kind"] == "result"]
    assert len(result_records) == 13
    assert all(record["evidence"]["events"] is not None for record in result_records)
    assert all(record["evidence"]["snapshot"] is None for record in result_records[:-1])
    assert result_records[-1]["evidence"]["snapshot"] is not None


def test_compact_final_divergence_can_replay_the_complete_workload_exactly(
    tmp_path: Path,
) -> None:
    workload = _workload(tmp_path, command_count=25, snapshot_interval=10)

    exact = rerun_exact_prefix(
        _executable(),
        workload,
        None,
        tmp_path / "exact-full-replay",
        timeout=30,
    )

    assert exact.passed
    assert exact.mode == "exact"
    assert exact.workload_command_count == 25
    assert exact.commands_compared == 25
    assert exact.reference_evidence_digest == exact.native_evidence_digest


def test_exact_prefix_timeout_covers_prefix_construction_and_removes_stale_outputs(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    workload = _workload(tmp_path, command_count=25, snapshot_interval=10)
    output = tmp_path / "exact-timeout"
    output.mkdir()
    stale_paths = (
        output / "exact-prefix.atlas",
        output / "exact-prefix-reference.jsonl",
        output / "exact-prefix-reference-native.jsonl",
    )
    for path in stale_paths:
        path.write_text("stale", encoding="ascii")
    original_commands = WorkloadReader.commands

    def delayed_commands(self: WorkloadReader) -> Iterator[Command]:
        for command in original_commands(self):
            time.sleep(0.01)
            yield command

    monkeypatch.setattr(WorkloadReader, "commands", delayed_commands)
    result = rerun_exact_prefix(
        _executable(),
        workload,
        12,
        output,
        timeout=0.001,
    )

    assert result.status == "harness_error"
    assert result.harness_error is not None
    assert "timed out" in result.harness_error
    assert all(not path.exists() for path in stale_paths)
    assert not (output / ".exact-prefix.atlas.tmp").exists()


def test_rolling_digests_and_classification_counts_replay_deterministically(
    tmp_path: Path,
) -> None:
    workload = _workload(tmp_path, command_count=80, seed=987)
    first = run_fixture(
        _executable(),
        workload,
        tmp_path / "first-reference.jsonl",
        mode="exact",
        timeout=30,
    )
    second = run_fixture(
        _executable(),
        workload,
        tmp_path / "second-reference.jsonl",
        mode="exact",
        timeout=30,
    )

    assert first.passed and second.passed
    assert first.command_digest == second.command_digest
    assert first.reference_evidence_digest == second.reference_evidence_digest
    assert first.native_evidence_digest == second.native_evidence_digest
    assert first.reference_classifications == second.reference_classifications
    assert first.reference_classifications == first.native_classifications
    counts = first.reference_classifications
    assert counts.committed + counts.rejected + counts.engine_error == 80


def test_structured_value_and_depth_differences_are_stable() -> None:
    expected = {
        "snapshot": {
            "bids": [
                {
                    "price": "100",
                    "aggregate_quantity": "5",
                    "orders": [{"order_id": "1", "remaining_quantity": "5"}],
                }
            ],
            "asks": [],
        }
    }
    actual = {
        "snapshot": {
            "bids": [
                {
                    "price": "100",
                    "aggregate_quantity": "4",
                    "orders": [{"order_id": "1", "remaining_quantity": "4"}],
                }
            ],
            "asks": [],
        }
    }

    difference = first_value_difference(expected, actual)
    depth = first_depth_difference(expected["snapshot"], actual["snapshot"])

    assert difference is not None
    assert difference.path == "$.snapshot.bids[0].aggregate_quantity"
    assert depth is not None
    assert depth.side == "bids"
    assert depth.level_index == 0
    assert depth.order_index is None
    assert depth.expected_price == 100
    assert depth.actual_price == 100


def test_comparison_prioritizes_l0_classification_over_coherent_event_changes() -> None:
    rejected = RejectedEvent(
        EventHeader(1, 0, 7),
        CommandType.NEW,
        RejectReason.INVALID_QUANTITY,
        1,
    )
    accepted = AcceptedEvent(EventHeader(1, 0, 7), CommandType.NEW)
    rejected_batch = EventBatch((rejected,))
    accepted_batch = EventBatch((accepted,))
    expected = {
        "kind": "result",
        "command_index": "0",
        "evidence": {
            "command_type": "new",
            "outcome": "rejected",
            "command_sequence": "1",
            "engine_error": None,
            "reject_reason": "invalid_quantity",
            "events": [differential._event_to_mapping(rejected)],
            "event_digest": event_digest(rejected_batch),
        },
    }
    actual = {
        "kind": "result",
        "command_index": "0",
        "evidence": {
            "command_type": "new",
            "outcome": "committed",
            "command_sequence": "1",
            "engine_error": None,
            "reject_reason": None,
            "events": [differential._event_to_mapping(accepted)],
            "event_digest": event_digest(accepted_batch),
        },
    }

    difference = first_value_difference(expected, actual)

    assert difference is not None
    assert difference.path == "$.evidence.outcome"


def test_final_comparison_prioritizes_l2_state_before_l3_snapshot() -> None:
    expected = {
        "kind": "final",
        "commands_processed": "1",
        "evidence": {
            "state": {"active_order_count": "1"},
            "snapshot": {"active_order_count": "1"},
        },
    }
    actual = {
        "kind": "final",
        "commands_processed": "1",
        "evidence": {
            "state": {"active_order_count": "2"},
            "snapshot": {"active_order_count": "2"},
        },
    }

    difference = first_value_difference(expected, actual)

    assert difference is not None
    assert difference.path == "$.evidence.state.active_order_count"


def test_exact_comparison_prioritizes_coherent_event_payload_over_its_digest() -> None:
    accepted = AcceptedEvent(EventHeader(1, 0, 7), CommandType.NEW)
    expected_trade = TradeEvent(
        EventHeader(1, 1, 7),
        10,
        20,
        1,
        2,
        Side.BUY,
        100,
        3,
        0,
        2,
    )
    actual_trade = TradeEvent(
        EventHeader(1, 1, 7),
        10,
        20,
        1,
        2,
        Side.BUY,
        101,
        3,
        0,
        2,
    )
    expected_batch = EventBatch((accepted, expected_trade))
    actual_batch = EventBatch((accepted, actual_trade))
    expected = {
        "command_type": "new",
        "outcome": "committed",
        "command_sequence": "1",
        "engine_error": None,
        "reject_reason": None,
        "events": [differential._event_to_mapping(event) for event in expected_batch.events],
        "event_digest": event_digest(expected_batch),
    }
    actual = {
        "command_type": "new",
        "outcome": "committed",
        "command_sequence": "1",
        "engine_error": None,
        "reject_reason": None,
        "events": [differential._event_to_mapping(event) for event in actual_batch.events],
        "event_digest": event_digest(actual_batch),
    }

    difference = first_value_difference(expected, actual)

    assert difference is not None
    assert difference.path == "$.events[1].execution_price"


def test_case_and_suite_helpers_preserve_declared_order(
    tmp_path: Path,
) -> None:
    cases = tuple(
        CampaignCase(
            f"case-{index}",
            index,
            resolve_workload_spec(
                profile,
                command_count=12,
                snapshot_interval=4,
            ),
        )
        for index, profile in enumerate(
            (
                WorkloadProfile.UNIFORM_SYNTHETIC,
                WorkloadProfile.CANCEL_HEAVY,
            ),
            start=1,
        )
    )
    suite = CampaignSuite("runner-smoke", "pr", "compact", cases)
    one = run_case(
        _executable(),
        cases[0],
        tmp_path / "single",
        mode="compact",
        timeout=30,
    )
    result = run_suite(
        _executable(),
        suite,
        tmp_path / "suite",
        timeout_per_case=30,
    )

    assert one.passed
    assert one.workload_manifest_path is not None
    assert one.workload_manifest_path.is_file()
    assert result.passed
    assert tuple(case.case_name for case in result.cases) == ("case-1", "case-2")
    assert all(isinstance(case, CampaignResult) for case in result.cases)
    assert all(
        case.workload_manifest_path is not None and case.workload_manifest_path.is_file()
        for case in result.cases
    )
