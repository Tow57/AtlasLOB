"""Disk-spooled differential campaigns for the Python and native engines.

The reference pass always completes and closes its versioned evidence stream
before the native process is spawned.  Native JSONL is then decoded and
compared incrementally so command-count-sized output is never retained.
"""

from __future__ import annotations

import hashlib
import json
import os
import queue
import re
import subprocess
import tempfile
import threading
import time
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass
from itertools import islice
from pathlib import Path
from typing import IO, Final, Literal, NoReturn, cast

from atlaslob.canonical import event_digest, state_digest
from atlaslob.domain import (
    AcceptedEvent,
    BookChangedEvent,
    BookSnapshot,
    CanceledEvent,
    Command,
    DoneEvent,
    Event,
    OrderSnapshot,
    PriceLevelSnapshot,
    ReferenceResult,
    RejectedEvent,
    ReplacedEvent,
    RestedEvent,
    TopOfBookLevel,
    TradeEvent,
    command_type,
)
from atlaslob.native import (
    NativeConfigRecord,
    NativeErrorRecord,
    NativeFinalRecord,
    NativeInputConfig,
    NativeProtocolError,
    NativeResultRecord,
    NativeState,
    NativeStreamDecoder,
    OutputMode,
    encode_command,
    encode_header,
)
from atlaslob.reference import ReferenceEngine
from atlaslob.workload import (
    CampaignCase,
    CampaignSuite,
    WorkloadReader,
    generate_workload,
    open_workload,
    read_manifest,
    verify_workload,
    write_manifest,
)

REFERENCE_EVIDENCE_SCHEMA: Final = "atlas_reference_evidence_v1"
_PUMP_DONE: Final = object()
_MISSING: Final = "<missing>"
_DIGEST_PATTERN: Final = re.compile(r"[0-9a-f]{64}\Z")
_RESULT_RECORD_KEY_ORDER: Final = (
    "kind",
    "command_index",
    "commands_processed",
    "evidence",
)
_RESULT_EVIDENCE_KEY_ORDER: Final = (
    "command_type",
    "outcome",
    "command_sequence",
    "engine_error",
    "reject_reason",
    "events",
    "event_digest",
    "state",
    "snapshot",
)
_FINAL_EVIDENCE_KEY_ORDER: Final = (
    "state",
    "snapshot",
)
_MAX_NATIVE_JSONL_RECORD_BYTES: Final = 8 * 1024 * 1024
_NATIVE_LINE_QUEUE_SIZE: Final = 8
CampaignStatus = Literal["passed", "diverged", "harness_error"]
EvidenceTransformer = Callable[
    [Command | None, Mapping[str, object]],
    Mapping[str, object],
]
FailureCategory = Literal[
    "classification",
    "event",
    "state",
    "snapshot",
    "stream",
    "final",
]


@dataclass(frozen=True, slots=True)
class ClassificationCounts:
    committed: int = 0
    rejected: int = 0
    engine_error: int = 0


@dataclass(frozen=True, slots=True)
class ReferenceCapture:
    path: Path
    mode: OutputMode
    config: NativeInputConfig
    workload_command_count: int
    commands_processed: int
    classifications: ClassificationCounts
    command_digest: str
    evidence_digest: str


@dataclass(frozen=True, slots=True)
class ValueDifference:
    path: str
    expected: object
    actual: object


@dataclass(frozen=True, slots=True)
class FailureSignature:
    category: FailureCategory
    field_path: str
    expected_kind: str
    actual_kind: str


@dataclass(frozen=True, slots=True)
class DepthDifference:
    side: Literal["bids", "asks"]
    level_index: int
    order_index: int | None
    expected_price: int | None
    actual_price: int | None
    difference: ValueDifference


@dataclass(frozen=True, slots=True)
class Divergence:
    command_index: int | None
    mode: OutputMode
    signature: FailureSignature
    difference: ValueDifference
    depth_difference: DepthDifference | None
    reference_state_digest: str | None
    native_state_digest: str | None
    reference_top: Mapping[str, object] | None
    native_top: Mapping[str, object] | None
    exact_prefix_required: bool


@dataclass(frozen=True, slots=True)
class CampaignResult:
    case_name: str
    mode: OutputMode
    status: CampaignStatus
    workload_path: Path
    workload_manifest_path: Path | None
    reference_evidence_path: Path
    native_evidence_path: Path | None
    workload_command_count: int
    commands_compared: int
    reference_classifications: ClassificationCounts
    native_classifications: ClassificationCounts
    command_digest: str
    reference_evidence_digest: str
    native_evidence_digest: str
    native_returncode: int | None
    divergence: Divergence | None = None
    harness_error: str | None = None

    @property
    def passed(self) -> bool:
        return self.status == "passed"


@dataclass(frozen=True, slots=True)
class CampaignSuiteResult:
    name: str
    cases: tuple[CampaignResult, ...]

    @property
    def passed(self) -> bool:
        return all(case.passed for case in self.cases)


@dataclass(frozen=True, slots=True)
class _ReferenceRecord:
    kind: Literal["config", "result", "final"]
    value: Mapping[str, object]


@dataclass(frozen=True, slots=True)
class _NativeComparison:
    status: CampaignStatus
    commands_compared: int
    classifications: ClassificationCounts
    evidence_digest: str
    returncode: int | None
    native_evidence_path: Path | None
    divergence: Divergence | None = None
    harness_error: str | None = None


def capture_reference(
    workload_path: Path,
    evidence_path: Path,
    *,
    mode: OutputMode,
    deadline: float | None = None,
) -> ReferenceCapture:
    """Run and persist the complete reference pass atomically."""

    if mode not in ("exact", "compact"):
        raise ValueError(f"unsupported differential mode: {mode}")
    workload_path = Path(workload_path).resolve()
    evidence_path = Path(evidence_path).resolve()
    temporary = evidence_path.with_name(f".{evidence_path.name}.tmp")
    _require_distinct_paths(
        {
            "workload": workload_path,
            "reference evidence": evidence_path,
            "reference temporary": temporary,
        }
    )
    workload = open_workload(workload_path)
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    evidence_path.unlink(missing_ok=True)
    temporary.unlink(missing_ok=True)
    command_hasher = hashlib.sha256()
    evidence_hasher = hashlib.sha256()
    counts = {"committed": 0, "rejected": 0, "engine_error": 0}
    workload_count = 0
    processed = 0
    engine_terminal = False
    engine = ReferenceEngine(workload.config.instrument_id, workload.config.engine)

    try:
        with temporary.open("wb") as output:
            _write_record(
                output,
                {
                    "schema": REFERENCE_EVIDENCE_SCHEMA,
                    "kind": "config",
                    "mode": mode,
                    "input": _input_config_to_mapping(workload.config),
                },
            )
            for command in workload.commands():
                _remaining(deadline)
                command_hasher.update((encode_command(command) + "\n").encode("ascii"))
                workload_count += 1
                if engine_terminal:
                    continue

                result = engine.execute(command)
                snapshot = engine.snapshot()
                evidence = _reference_result_evidence(
                    command,
                    result,
                    snapshot,
                    mode=mode,
                    checkpoint=_is_checkpoint(
                        processed,
                        workload.config.snapshot_interval,
                    ),
                )
                envelope: dict[str, object] = {
                    "command_index": str(processed),
                    "evidence": evidence,
                }
                digest_record = {"kind": "result", **envelope}
                _update_mapping_digest(evidence_hasher, digest_record)
                _write_record(
                    output,
                    {
                        "schema": REFERENCE_EVIDENCE_SCHEMA,
                        "kind": "result",
                        **envelope,
                    },
                )
                processed += 1
                outcome = cast(str, evidence["outcome"])
                counts[outcome] += 1
                engine_terminal = result.error is not None

            final_snapshot = engine.snapshot()
            _remaining(deadline)
            final_evidence = _reference_final_evidence(final_snapshot)
            final_digest_record = {
                "kind": "final",
                "commands_processed": str(processed),
                "evidence": final_evidence,
            }
            _update_mapping_digest(evidence_hasher, final_digest_record)
            evidence_digest = evidence_hasher.hexdigest()
            command_digest = command_hasher.hexdigest()
            _write_record(
                output,
                {
                    "schema": REFERENCE_EVIDENCE_SCHEMA,
                    "kind": "final",
                    "workload_command_count": str(workload_count),
                    "commands_processed": str(processed),
                    "command_digest": command_digest,
                    "evidence_digest": evidence_digest,
                    "classification_counts": {name: str(value) for name, value in counts.items()},
                    "evidence": final_evidence,
                },
            )
            output.flush()
        temporary.replace(evidence_path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise

    return ReferenceCapture(
        path=evidence_path.resolve(),
        mode=mode,
        config=workload.config,
        workload_command_count=workload_count,
        commands_processed=processed,
        classifications=_counts_from_mutable(counts),
        command_digest=command_digest,
        evidence_digest=evidence_digest,
    )


def run_fixture(
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
    """Capture reference evidence, then stream and compare one native fixture."""

    if not isinstance(capture_native_evidence, bool):
        raise ValueError("capture_native_evidence must be a bool")
    deadline = _deadline(timeout)
    executable = Path(executable).resolve()
    workload_path = Path(workload_path).resolve()
    evidence_path = Path(evidence_path).resolve()
    resolved_manifest_path = (
        Path(workload_manifest_path).resolve() if workload_manifest_path is not None else None
    )
    requested_native_evidence = (
        Path(native_evidence_path).resolve()
        if native_evidence_path is not None
        else evidence_path.with_name(f"{evidence_path.stem}-native.jsonl")
    )
    resolved_native_evidence = requested_native_evidence if capture_native_evidence else None
    managed_paths = {
        "reference evidence": evidence_path,
        "reference temporary": evidence_path.with_name(f".{evidence_path.name}.tmp"),
        "native evidence": requested_native_evidence,
        "native temporary": requested_native_evidence.with_name(
            f".{requested_native_evidence.name}.tmp"
        ),
    }
    _require_distinct_paths(
        {
            "native executable": executable,
            "workload": workload_path,
            **(
                {"workload manifest": resolved_manifest_path}
                if resolved_manifest_path is not None
                else {}
            ),
            **managed_paths,
        }
    )
    try:
        _remove_stale_outputs(managed_paths.values())
        if resolved_manifest_path is not None:
            verify_workload(
                workload_path,
                read_manifest(resolved_manifest_path),
                deadline=deadline,
            )
        capture = capture_reference(
            workload_path,
            evidence_path,
            mode=mode,
            deadline=deadline,
        )
    except Exception as exc:
        return _capture_failure_result(
            case_name=case_name,
            mode=mode,
            workload_path=workload_path,
            evidence_path=evidence_path,
            workload_manifest_path=resolved_manifest_path,
            error=exc,
        )

    try:
        comparison = _compare_native(
            executable=executable,
            workload=open_workload(workload_path),
            capture=capture,
            timeout=_remaining(deadline),
            evidence_transformer=evidence_transformer,
            native_evidence_path=resolved_native_evidence,
        )
    except Exception as exc:
        comparison = _NativeComparison(
            status="harness_error",
            commands_compared=0,
            classifications=ClassificationCounts(),
            evidence_digest=hashlib.sha256().hexdigest(),
            returncode=None,
            native_evidence_path=None,
            harness_error=_exception_text(exc),
        )
    return CampaignResult(
        case_name=case_name,
        mode=mode,
        status=comparison.status,
        workload_path=workload_path,
        workload_manifest_path=resolved_manifest_path,
        reference_evidence_path=capture.path,
        native_evidence_path=comparison.native_evidence_path,
        workload_command_count=capture.workload_command_count,
        commands_compared=comparison.commands_compared,
        reference_classifications=capture.classifications,
        native_classifications=comparison.classifications,
        command_digest=capture.command_digest,
        reference_evidence_digest=capture.evidence_digest,
        native_evidence_digest=comparison.evidence_digest,
        native_returncode=comparison.returncode,
        divergence=comparison.divergence,
        harness_error=comparison.harness_error,
    )


def rerun_exact_prefix(
    executable: Path,
    workload_path: Path,
    failing_command_index: int | None,
    output_directory: Path,
    *,
    case_name: str = "exact-prefix",
    timeout: float | None = 60.0,
    capture_native_evidence: bool = True,
) -> CampaignResult:
    """Rerun a divergent prefix, or the complete workload, from empty state."""

    if not isinstance(capture_native_evidence, bool):
        raise ValueError("capture_native_evidence must be a bool")
    deadline = _deadline(timeout)
    if failing_command_index is not None and (
        isinstance(failing_command_index, bool)
        or not isinstance(failing_command_index, int)
        or failing_command_index < 0
        or failing_command_index >= (1 << 64) - 1
    ):
        raise ValueError("failing_command_index must be a valid u64 command index")
    executable = Path(executable).resolve()
    source_path = Path(workload_path).resolve()
    output_directory = Path(output_directory).resolve()
    prefix_path = output_directory / "exact-prefix.atlas"
    temporary = prefix_path.with_name(f".{prefix_path.name}.tmp")
    reference_path = output_directory / "exact-prefix-reference.jsonl"
    native_path = reference_path.with_name(f"{reference_path.stem}-native.jsonl")
    managed_paths = {
        "exact replay workload": prefix_path,
        "exact replay temporary": temporary,
        "exact replay reference": reference_path,
        "exact replay reference temporary": reference_path.with_name(f".{reference_path.name}.tmp"),
        "exact replay native": native_path,
        "exact replay native temporary": native_path.with_name(f".{native_path.name}.tmp"),
    }
    _require_distinct_paths(
        {
            "native executable": executable,
            "source workload": source_path,
            **managed_paths,
        }
    )
    try:
        output_directory.mkdir(parents=True, exist_ok=True)
        _remove_stale_outputs(managed_paths.values())
        _remaining(deadline)
        source = open_workload(source_path)
        prefix_config = NativeInputConfig(
            instrument_id=source.config.instrument_id,
            engine=source.config.engine,
            snapshot_interval=(
                source.config.snapshot_interval
                if failing_command_index is None
                else failing_command_index + 1
            ),
        )
        written = 0
        with temporary.open("wb") as output:
            output.write((encode_header(prefix_config) + "\n").encode("ascii"))
            commands = source.commands()
            selected_commands = (
                commands
                if failing_command_index is None
                else islice(commands, failing_command_index + 1)
            )
            for command in selected_commands:
                _remaining(deadline)
                output.write((encode_command(command) + "\n").encode("ascii"))
                written += 1
            _remaining(deadline)
            output.flush()
        if failing_command_index is not None and written != failing_command_index + 1:
            raise ValueError("failing command index is outside the workload")
        _remaining(deadline)
        temporary.replace(prefix_path)
        remaining = _remaining(deadline)
    except (KeyboardInterrupt, SystemExit):
        temporary.unlink(missing_ok=True)
        raise
    except Exception as exc:
        temporary.unlink(missing_ok=True)
        return _capture_failure_result(
            case_name=case_name,
            mode="exact",
            workload_path=prefix_path,
            evidence_path=reference_path,
            workload_manifest_path=None,
            error=exc,
        )
    return run_fixture(
        executable,
        prefix_path,
        reference_path,
        mode="exact",
        case_name=case_name,
        timeout=remaining,
        capture_native_evidence=capture_native_evidence,
    )


def run_case(
    executable: Path,
    case: CampaignCase,
    output_directory: Path,
    *,
    mode: OutputMode,
    timeout: float | None = 60.0,
    capture_native_evidence: bool = True,
) -> CampaignResult:
    """Generate and run one named campaign case."""

    safe_name = _safe_case_name(case.name)
    case_directory = Path(output_directory) / safe_name
    workload_path = case_directory / "workload.atlas"
    manifest_path = case_directory / "manifest.json"
    manifest = generate_workload(case.spec, case.seed, workload_path)
    write_manifest(manifest_path, manifest)
    return run_fixture(
        executable,
        workload_path,
        case_directory / "reference.jsonl",
        mode=mode,
        case_name=case.name,
        timeout=timeout,
        workload_manifest_path=manifest_path,
        capture_native_evidence=capture_native_evidence,
    )


def run_suite(
    executable: Path,
    suite: CampaignSuite,
    output_directory: Path,
    *,
    timeout_per_case: float | None = 60.0,
) -> CampaignSuiteResult:
    """Generate and run every case in suite order."""

    cases = tuple(
        run_case(
            executable,
            case,
            Path(output_directory) / _safe_case_name(suite.name),
            mode=suite.mode,
            timeout=timeout_per_case,
        )
        for case in suite.cases
    )
    return CampaignSuiteResult(suite.name, cases)


def first_value_difference(
    expected: object,
    actual: object,
    *,
    path: str = "$",
) -> ValueDifference | None:
    """Return the first stable, recursively ordered value difference."""

    if type(expected) is not type(actual):
        return ValueDifference(path, expected, actual)
    if isinstance(expected, Mapping):
        actual_mapping = cast(Mapping[object, object], actual)
        keys = _comparison_key_order(expected, actual_mapping)
        for key in keys:
            child_path = f"{path}.{key}"
            if key not in expected:
                return ValueDifference(child_path, _MISSING, actual_mapping[key])
            if key not in actual_mapping:
                return ValueDifference(child_path, expected[key], _MISSING)
            difference = first_value_difference(
                expected[key],
                actual_mapping[key],
                path=child_path,
            )
            if difference is not None:
                return difference
        return None
    if isinstance(expected, list):
        actual_list = cast(list[object], actual)
        if len(expected) != len(actual_list):
            return ValueDifference(f"{path}.length", len(expected), len(actual_list))
        for index, (expected_item, actual_item) in enumerate(
            zip(expected, actual_list, strict=True)
        ):
            difference = first_value_difference(
                expected_item,
                actual_item,
                path=f"{path}[{index}]",
            )
            if difference is not None:
                return difference
        return None
    if expected != actual:
        return ValueDifference(path, expected, actual)
    return None


def _comparison_key_order(
    expected: Mapping[object, object],
    actual: Mapping[object, object],
) -> list[object]:
    keys = set(expected) | set(actual)
    priority: tuple[str, ...]
    if "evidence" in keys and "kind" in keys:
        priority = _RESULT_RECORD_KEY_ORDER
    elif "outcome" in keys and "command_type" in keys:
        priority = _RESULT_EVIDENCE_KEY_ORDER
    elif "state" in keys and "snapshot" in keys:
        priority = _FINAL_EVIDENCE_KEY_ORDER
    else:
        priority = ()
    ordered: list[object] = [key for key in priority if key in keys]
    ordered.extend(sorted(keys.difference(ordered), key=lambda value: str(value)))
    return ordered


def first_depth_difference(
    expected: object,
    actual: object,
) -> DepthDifference | None:
    """Locate the smallest first bid/ask level or order difference."""

    if not isinstance(expected, Mapping) or not isinstance(actual, Mapping):
        return None
    for side in ("bids", "asks"):
        expected_levels = expected.get(side)
        actual_levels = actual.get(side)
        if not isinstance(expected_levels, list) or not isinstance(actual_levels, list):
            continue
        maximum = max(len(expected_levels), len(actual_levels))
        for level_index in range(maximum):
            if level_index >= len(expected_levels):
                difference = ValueDifference(
                    f"$.snapshot.{side}[{level_index}]",
                    _MISSING,
                    actual_levels[level_index],
                )
                return DepthDifference(
                    side,
                    level_index,
                    None,
                    None,
                    _mapping_price(actual_levels[level_index]),
                    difference,
                )
            if level_index >= len(actual_levels):
                difference = ValueDifference(
                    f"$.snapshot.{side}[{level_index}]",
                    expected_levels[level_index],
                    _MISSING,
                )
                return DepthDifference(
                    side,
                    level_index,
                    None,
                    _mapping_price(expected_levels[level_index]),
                    None,
                    difference,
                )
            expected_level = expected_levels[level_index]
            actual_level = actual_levels[level_index]
            level_difference = first_value_difference(
                expected_level,
                actual_level,
                path=f"$.snapshot.{side}[{level_index}]",
            )
            if level_difference is not None:
                order_match = re.search(r"\.orders\[(\d+)\]", level_difference.path)
                return DepthDifference(
                    side,
                    level_index,
                    int(order_match.group(1)) if order_match is not None else None,
                    _mapping_price(expected_level),
                    _mapping_price(actual_level),
                    level_difference,
                )
    return None


def _compare_native(
    *,
    executable: Path,
    workload: WorkloadReader,
    capture: ReferenceCapture,
    timeout: float | None,
    evidence_transformer: EvidenceTransformer | None,
    native_evidence_path: Path | None,
) -> _NativeComparison:
    deadline = _deadline(timeout)
    native_hasher = hashlib.sha256()
    native_counts = {"committed": 0, "rejected": 0, "engine_error": 0}
    compared = 0
    process: subprocess.Popen[bytes] | None = None
    pump: _LinePump | None = None
    returncode: int | None = None
    native_capture = _NativeOutputCapture(native_evidence_path)

    try:
        native_capture.open()
        with (
            workload.path.open("rb") as native_input,
            capture.path.open("r", encoding="ascii", newline="") as reference_file,
            tempfile.TemporaryFile(mode="w+b") as native_stderr,
        ):
            reference = _ReferenceCursor(reference_file, capture)
            process = _spawn_native(
                executable,
                capture.mode,
                native_input,
                native_stderr,
            )
            if process.stdout is None:
                raise RuntimeError("native process stdout pipe is unavailable")
            decoder = NativeStreamDecoder(
                expected_mode=capture.mode,
                expected_input=workload.config,
                expected_commands=workload.commands(),
            )
            pump = _LinePump(process.stdout)
            pump.start()
            adapter_error: NativeErrorRecord | None = None
            transform_commands = workload.commands() if evidence_transformer is not None else None

            while True:
                item = pump.next(_remaining(deadline))
                if item is _PUMP_DONE:
                    break
                if isinstance(item, BaseException):
                    raise item
                raw_line = cast(bytes, item)
                native_capture.write(raw_line)
                if not raw_line.endswith(b"\n"):
                    raise NativeProtocolError("native JSONL record is not LF terminated")
                try:
                    line = raw_line.decode("utf-8")
                except UnicodeDecodeError as exc:
                    raise NativeProtocolError("native process output is not valid UTF-8") from exc
                record = decoder.feed_line(line)
                if isinstance(record, NativeConfigRecord):
                    continue
                if isinstance(record, NativeErrorRecord):
                    adapter_error = record
                    continue

                expected_record = reference.next()
                actual_record = _native_record_mapping(record)
                if evidence_transformer is not None:
                    command = (
                        next(transform_commands)
                        if isinstance(record, NativeResultRecord) and transform_commands is not None
                        else None
                    )
                    actual_record = dict(evidence_transformer(command, actual_record))
                difference = first_value_difference(
                    expected_record.value,
                    actual_record,
                )
                if difference is not None:
                    divergence = _make_divergence(
                        command_index=(
                            record.command_index if isinstance(record, NativeResultRecord) else None
                        ),
                        mode=capture.mode,
                        expected=expected_record.value,
                        actual=actual_record,
                        difference=difference,
                    )
                    _stop_process(process, pump)
                    returncode = process.returncode
                    return _NativeComparison(
                        status="diverged",
                        commands_compared=compared,
                        classifications=_counts_from_mutable(native_counts),
                        evidence_digest=native_hasher.hexdigest(),
                        returncode=returncode,
                        native_evidence_path=native_capture.retain(),
                        divergence=divergence,
                    )

                _update_mapping_digest(native_hasher, actual_record)
                if isinstance(record, NativeResultRecord):
                    native_counts[record.outcome] += 1
                    compared += 1
                else:
                    reference.ensure_eof()

            returncode = process.wait(timeout=_remaining(deadline))
            summary = decoder.finish(returncode)
            pump.stop()
            process.stdout.close()
            pump.join()
            native_stderr.seek(0)
            stderr_bytes = native_stderr.read()
            if stderr_bytes:
                raise NativeProtocolError("native process wrote unexpected standard-error output")
            if adapter_error is not None or summary.error is not None:
                if adapter_error is not None:
                    error = adapter_error
                elif summary.error is not None:
                    error = summary.error
                else:
                    raise RuntimeError("unreachable adapter error state")
                raise NativeProtocolError(
                    f"native adapter failed at line {error.line}: {error.code}"
                )
            if summary.final is None:
                raise NativeProtocolError("native process omitted its final record")
            if native_hasher.hexdigest() != capture.evidence_digest:
                raise NativeProtocolError(
                    "native evidence digest differs after value-by-value equality"
                )
            return _NativeComparison(
                status="passed",
                commands_compared=compared,
                classifications=_counts_from_mutable(native_counts),
                evidence_digest=native_hasher.hexdigest(),
                returncode=returncode,
                native_evidence_path=None,
            )
    except (KeyboardInterrupt, SystemExit):
        if process is not None:
            _stop_process(process, pump)
        raise
    except Exception as exc:
        if process is not None:
            _stop_process(process, pump)
            returncode = process.returncode
        return _NativeComparison(
            status="harness_error",
            commands_compared=compared,
            classifications=_counts_from_mutable(native_counts),
            evidence_digest=native_hasher.hexdigest(),
            returncode=returncode,
            native_evidence_path=native_capture.retain(),
            harness_error=_exception_text(exc),
        )
    finally:
        native_capture.discard()


def _spawn_native(
    executable: Path,
    mode: OutputMode,
    native_input: IO[bytes],
    native_stderr: IO[bytes],
) -> subprocess.Popen[bytes]:
    return subprocess.Popen(
        [str(executable), mode],
        stdin=native_input,
        stdout=subprocess.PIPE,
        stderr=native_stderr,
        shell=False,
        text=False,
        bufsize=0,
    )


class _NativeOutputCapture:
    """Atomically retain native JSONL only when a comparison does not pass."""

    __slots__ = ("_file", "_open", "_path", "_temporary")

    def __init__(self, path: Path | None) -> None:
        self._path = Path(path) if path is not None else None
        self._temporary = (
            self._path.with_name(f".{self._path.name}.tmp") if self._path is not None else None
        )
        self._file: IO[bytes] | None = None
        self._open = False

    def open(self) -> None:
        if self._open:
            raise RuntimeError("native evidence capture is already open")
        self._open = True
        if self._path is None or self._temporary is None:
            return
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._path.unlink(missing_ok=True)
        self._temporary.unlink(missing_ok=True)
        self._file = self._temporary.open("wb")

    def write(self, value: bytes) -> None:
        if not self._open:
            raise RuntimeError("native evidence capture is not open")
        if self._file is None:
            return
        self._file.write(value)

    def retain(self) -> Path | None:
        if not self._open:
            raise RuntimeError("native evidence capture is not open")
        if self._file is not None:
            self._file.flush()
            self._file.close()
            self._file = None
        self._open = False
        if self._temporary is not None and self._temporary.exists():
            if self._path is None:
                raise RuntimeError("native evidence capture path is unavailable")
            self._temporary.replace(self._path)
        return self._path.resolve() if self._path is not None else None

    def discard(self) -> None:
        if self._file is not None:
            self._file.close()
            self._file = None
        self._open = False
        if self._temporary is not None:
            self._temporary.unlink(missing_ok=True)


class _LinePump:
    __slots__ = ("_output", "_queue", "_stop", "_thread")

    def __init__(self, output: IO[bytes]) -> None:
        self._output = output
        self._queue: queue.Queue[bytes | BaseException | object] = queue.Queue(
            maxsize=_NATIVE_LINE_QUEUE_SIZE
        )
        self._stop = threading.Event()
        self._thread = threading.Thread(
            target=self._run,
            name="atlas-native-jsonl",
            daemon=True,
        )

    def start(self) -> None:
        self._thread.start()

    def next(self, timeout: float | None) -> bytes | BaseException | object:
        try:
            return self._queue.get(timeout=timeout)
        except queue.Empty as exc:
            raise TimeoutError("native differential process timed out") from exc

    def stop(self) -> None:
        self._stop.set()

    def join(self) -> None:
        self._thread.join(timeout=5.0)

    def _run(self) -> None:
        try:
            while not self._stop.is_set():
                line = self._output.readline(_MAX_NATIVE_JSONL_RECORD_BYTES + 1)
                if not line:
                    break
                if len(line) > _MAX_NATIVE_JSONL_RECORD_BYTES:
                    raise NativeProtocolError("native JSONL record exceeds the byte-size limit")
                if not self._put(line):
                    return
        except BaseException as exc:
            self._put(exc)
        finally:
            self._put(_PUMP_DONE)

    def _put(self, item: bytes | BaseException | object) -> bool:
        while not self._stop.is_set():
            try:
                self._queue.put(item, timeout=0.1)
            except queue.Full:
                continue
            return True
        return False


class _ReferenceCursor:
    __slots__ = (
        "_capture",
        "_counts",
        "_hasher",
        "_input",
        "_line",
        "_result_count",
    )

    def __init__(self, input_file: IO[str], capture: ReferenceCapture) -> None:
        self._input = input_file
        self._capture = capture
        self._line = 0
        self._result_count = 0
        self._counts = {"committed": 0, "rejected": 0, "engine_error": 0}
        self._hasher = hashlib.sha256()
        config = self._read()
        if config.kind != "config":
            raise ValueError("reference evidence does not begin with config")
        if config.value.get("mode") != capture.mode:
            raise ValueError("reference evidence mode differs from capture")
        if config.value.get("input") != _input_config_to_mapping(capture.config):
            raise ValueError("reference evidence config differs from capture")

    def next(self) -> _ReferenceRecord:
        record = self._read()
        if record.kind == "config":
            raise ValueError("reference evidence contains a second config")
        if record.kind == "result":
            comparable = _ReferenceRecord(
                "result",
                {
                    "kind": "result",
                    "command_index": record.value.get("command_index"),
                    "evidence": record.value.get("evidence"),
                },
            )
            if comparable.value["command_index"] != str(self._result_count):
                raise ValueError("reference result indices are not contiguous")
            evidence = _nested_mapping(comparable.value, "evidence")
            outcome = evidence.get("outcome")
            if outcome not in self._counts:
                raise ValueError("reference result has invalid outcome")
            self._counts[outcome] += 1
            self._result_count += 1
            _update_mapping_digest(self._hasher, comparable.value)
            return comparable
        comparable = _ReferenceRecord(
            "final",
            {
                "kind": "final",
                "commands_processed": record.value.get("commands_processed"),
                "evidence": record.value.get("evidence"),
            },
        )
        _update_mapping_digest(self._hasher, comparable.value)
        self._validate_final(record.value)
        return comparable

    def ensure_eof(self) -> None:
        extra = self._input.readline()
        if extra:
            raise ValueError("reference evidence contains records after final")

    def _read(self) -> _ReferenceRecord:
        raw = self._input.readline()
        self._line += 1
        if not raw:
            raise ValueError("reference evidence ended before native output")
        if not raw.endswith("\n"):
            raise ValueError(f"reference evidence line {self._line} is not LF terminated")
        if raw.endswith("\r\n"):
            raise ValueError(f"reference evidence line {self._line} uses CRLF")
        try:
            value = json.loads(
                raw,
                object_pairs_hook=_unique_reference_object,
                parse_constant=_reject_reference_constant,
            )
        except (json.JSONDecodeError, ValueError) as exc:
            raise ValueError(f"reference evidence line {self._line} is not valid JSON") from exc
        record = _string_mapping(value, f"reference evidence line {self._line}")
        try:
            encoded = raw.encode("ascii")
        except UnicodeEncodeError as exc:
            raise ValueError("reference evidence is not canonical ASCII") from exc
        if encoded != _canonical_json(record):
            raise ValueError("reference evidence is not canonical JSON")
        if record.get("schema") != REFERENCE_EVIDENCE_SCHEMA:
            raise ValueError("reference evidence has unsupported schema")
        kind = record.get("kind")
        if kind not in {"config", "result", "final"}:
            raise ValueError("reference evidence has unknown record kind")
        expected_keys = {
            "config": {"schema", "kind", "mode", "input"},
            "result": {"schema", "kind", "command_index", "evidence"},
            "final": {
                "schema",
                "kind",
                "workload_command_count",
                "commands_processed",
                "command_digest",
                "evidence_digest",
                "classification_counts",
                "evidence",
            },
        }
        if set(record) != expected_keys[kind]:
            raise ValueError("reference evidence record fields differ from schema")
        return _ReferenceRecord(
            kind,
            record,
        )

    def _validate_final(self, record: Mapping[str, object]) -> None:
        for name in ("command_digest", "evidence_digest"):
            value = record.get(name)
            if not isinstance(value, str) or _DIGEST_PATTERN.fullmatch(value) is None:
                raise ValueError(f"reference {name} is not lowercase SHA-256")
        if record.get("workload_command_count") != str(self._capture.workload_command_count):
            raise ValueError("reference workload command count differs from capture")
        if record.get("commands_processed") != str(self._result_count):
            raise ValueError("reference processed count differs from result records")
        if record.get("commands_processed") != str(self._capture.commands_processed):
            raise ValueError("reference processed count differs from capture")
        if record.get("command_digest") != self._capture.command_digest:
            raise ValueError("reference command digest differs from capture")
        if record.get("evidence_digest") != self._capture.evidence_digest:
            raise ValueError("reference evidence digest differs from capture")
        if self._hasher.hexdigest() != self._capture.evidence_digest:
            raise ValueError("reference evidence records differ from their digest")
        raw_counts = _nested_mapping(record, "classification_counts")
        expected_counts = {name: str(value) for name, value in self._counts.items()}
        if raw_counts != expected_counts:
            raise ValueError("reference classification counts differ from result records")
        if _counts_from_mutable(self._counts) != self._capture.classifications:
            raise ValueError("reference classification counts differ from capture")


def _reference_result_evidence(
    command: Command,
    result: ReferenceResult,
    snapshot: BookSnapshot,
    *,
    mode: OutputMode,
    checkpoint: bool,
) -> dict[str, object]:
    if result.batch is None:
        outcome = "engine_error"
        command_sequence: str | None = None
        engine_error = result.error.name.lower() if result.error is not None else None
        reject_reason: str | None = None
        digest: str | None = None
        events: list[object] | None = None
    else:
        outcome = "rejected" if result.batch.rejected else "committed"
        command_sequence = str(result.batch.command_sequence)
        engine_error = None
        rejection = result.batch.events[0] if result.batch.rejected else None
        reject_reason = (
            rejection.reason.name.lower() if isinstance(rejection, RejectedEvent) else None
        )
        digest = event_digest(result.batch)
        events = (
            [_event_to_mapping(event) for event in result.batch.events] if mode == "exact" else None
        )
    return {
        "command_type": command_type(command).name.lower(),
        "outcome": outcome,
        "command_sequence": command_sequence,
        "engine_error": engine_error,
        "reject_reason": reject_reason,
        "event_digest": digest,
        "events": events,
        "state": _reference_state_mapping(snapshot),
        "snapshot": _snapshot_to_mapping(snapshot) if checkpoint else None,
    }


def _reference_final_evidence(snapshot: BookSnapshot) -> dict[str, object]:
    return {
        "state": _reference_state_mapping(snapshot),
        "snapshot": _snapshot_to_mapping(snapshot),
    }


def _native_record_mapping(
    record: NativeResultRecord | NativeFinalRecord,
) -> dict[str, object]:
    if isinstance(record, NativeResultRecord):
        evidence: dict[str, object] = {
            "command_type": record.command_type.name.lower(),
            "outcome": record.outcome,
            "command_sequence": (
                str(record.command_sequence) if record.command_sequence is not None else None
            ),
            "engine_error": (
                record.engine_error.name.lower() if record.engine_error is not None else None
            ),
            "reject_reason": (
                record.reject_reason.name.lower() if record.reject_reason is not None else None
            ),
            "event_digest": record.event_digest,
            "events": (
                [_event_to_mapping(event) for event in record.events]
                if record.events is not None
                else None
            ),
            "state": _native_state_mapping(record.state),
            "snapshot": (
                _snapshot_to_mapping(record.snapshot) if record.snapshot is not None else None
            ),
        }
        return {
            "kind": "result",
            "command_index": str(record.command_index),
            "evidence": evidence,
        }
    return {
        "kind": "final",
        "commands_processed": str(record.commands_processed),
        "evidence": {
            "state": _native_state_mapping(record.state),
            "snapshot": _snapshot_to_mapping(record.snapshot),
        },
    }


def _reference_state_mapping(snapshot: BookSnapshot) -> dict[str, object]:
    return {
        "active_order_count": str(snapshot.active_order_count),
        "empty": snapshot.active_order_count == 0,
        "next_sequence": str(0 if snapshot.sequence_exhausted else snapshot.last_sequence + 1),
        "sequence_exhausted": snapshot.sequence_exhausted,
        "best_bid": _snapshot_top(snapshot.bids),
        "best_ask": _snapshot_top(snapshot.asks),
        **_side_summary_mapping(snapshot),
        "state_digest": state_digest(snapshot),
    }


def _native_state_mapping(state: NativeState) -> dict[str, object]:
    return {
        "active_order_count": str(state.active_order_count),
        "empty": state.empty,
        "next_sequence": str(state.next_sequence),
        "sequence_exhausted": state.sequence_exhausted,
        "best_bid": _top_to_mapping(state.best_bid),
        "best_ask": _top_to_mapping(state.best_ask),
        "bid_level_count": str(state.bid_level_count),
        "bid_order_count": str(state.bid_order_count),
        "bid_aggregate_quantity": str(state.bid_aggregate_quantity),
        "ask_level_count": str(state.ask_level_count),
        "ask_order_count": str(state.ask_order_count),
        "ask_aggregate_quantity": str(state.ask_aggregate_quantity),
        "state_digest": state.state_digest,
    }


def _side_summary_mapping(snapshot: BookSnapshot) -> dict[str, object]:
    return {
        "bid_level_count": str(len(snapshot.bids)),
        "bid_order_count": str(sum(len(level.orders) for level in snapshot.bids)),
        "bid_aggregate_quantity": str(sum(level.aggregate_quantity for level in snapshot.bids)),
        "ask_level_count": str(len(snapshot.asks)),
        "ask_order_count": str(sum(len(level.orders) for level in snapshot.asks)),
        "ask_aggregate_quantity": str(sum(level.aggregate_quantity for level in snapshot.asks)),
    }


def _snapshot_to_mapping(snapshot: BookSnapshot) -> dict[str, object]:
    return {
        "semantics_version": snapshot.semantics_version,
        "instrument_id": str(snapshot.instrument_id),
        "last_sequence": str(snapshot.last_sequence),
        "sequence_exhausted": snapshot.sequence_exhausted,
        "active_order_count": str(snapshot.active_order_count),
        "bids": [_level_to_mapping(level) for level in snapshot.bids],
        "asks": [_level_to_mapping(level) for level in snapshot.asks],
    }


def _level_to_mapping(level: PriceLevelSnapshot) -> dict[str, object]:
    return {
        "price": str(level.price),
        "aggregate_quantity": str(level.aggregate_quantity),
        "orders": [_order_to_mapping(order) for order in level.orders],
    }


def _order_to_mapping(order: OrderSnapshot) -> dict[str, object]:
    return {
        "order_id": str(order.order_id),
        "client_id": str(order.client_id),
        "instrument_id": str(order.instrument_id),
        "side": order.side.name.lower(),
        "price": str(order.price),
        "remaining_quantity": str(order.remaining_quantity),
        "priority_sequence": str(order.priority_sequence),
    }


def _event_to_mapping(event: Event) -> dict[str, object]:
    value: dict[str, object] = {
        "type": _event_name(event),
        "header": {
            "command_sequence": str(event.header.command_sequence),
            "event_index": event.header.event_index,
            "instrument_id": str(event.header.instrument_id),
        },
    }
    if isinstance(event, AcceptedEvent):
        value["command_type"] = event.command_type.name.lower()
    elif isinstance(event, RejectedEvent):
        value.update(
            {
                "command_type": event.command_type.name.lower(),
                "reason": event.reason.name.lower(),
                "order_id": str(event.order_id) if event.order_id is not None else None,
            }
        )
    elif isinstance(event, TradeEvent):
        value.update(
            {
                "aggressor_order_id": str(event.aggressor_order_id),
                "resting_order_id": str(event.resting_order_id),
                "aggressor_client_id": str(event.aggressor_client_id),
                "resting_client_id": str(event.resting_client_id),
                "aggressor_side": event.aggressor_side.name.lower(),
                "execution_price": str(event.execution_price),
                "execution_quantity": str(event.execution_quantity),
                "aggressor_remaining": str(event.aggressor_remaining),
                "resting_remaining": str(event.resting_remaining),
            }
        )
    elif isinstance(event, RestedEvent):
        value.update(
            {
                "order_id": str(event.order_id),
                "client_id": str(event.client_id),
                "side": event.side.name.lower(),
                "price": str(event.price),
                "remaining_quantity": str(event.remaining_quantity),
            }
        )
    elif isinstance(event, CanceledEvent):
        value.update(
            {
                "order_id": str(event.order_id),
                "canceled_quantity": str(event.canceled_quantity),
            }
        )
    elif isinstance(event, ReplacedEvent):
        value.update(
            {
                "old_order_id": str(event.old_order_id),
                "new_order_id": str(event.new_order_id),
            }
        )
    elif isinstance(event, DoneEvent):
        value.update(
            {
                "order_id": str(event.order_id),
                "reason": event.reason.name.lower(),
                "remaining_quantity": str(event.remaining_quantity),
            }
        )
    elif isinstance(event, BookChangedEvent):
        value.update(
            {
                "best_bid": _top_to_mapping(event.best_bid),
                "best_ask": _top_to_mapping(event.best_ask),
            }
        )
    return value


def _event_name(event: Event) -> str:
    if isinstance(event, AcceptedEvent):
        return "accepted"
    if isinstance(event, RejectedEvent):
        return "rejected"
    if isinstance(event, TradeEvent):
        return "trade"
    if isinstance(event, RestedEvent):
        return "rested"
    if isinstance(event, CanceledEvent):
        return "canceled"
    if isinstance(event, ReplacedEvent):
        return "replaced"
    if isinstance(event, DoneEvent):
        return "done"
    return "book_changed"


def _snapshot_top(
    levels: tuple[PriceLevelSnapshot, ...],
) -> dict[str, object] | None:
    if not levels:
        return None
    return {
        "price": str(levels[0].price),
        "aggregate_quantity": str(levels[0].aggregate_quantity),
    }


def _top_to_mapping(level: TopOfBookLevel | None) -> dict[str, object] | None:
    if level is None:
        return None
    return {
        "price": str(level.price),
        "aggregate_quantity": str(level.aggregate_quantity),
    }


def _input_config_to_mapping(config: NativeInputConfig) -> dict[str, object]:
    return {
        "instrument_id": str(config.instrument_id),
        "max_order_quantity": str(config.engine.max_order_quantity),
        "tick_increment": str(config.engine.tick_increment),
        "max_active_orders": str(config.engine.max_active_orders),
        "snapshot_interval": str(config.snapshot_interval),
    }


def _make_divergence(
    *,
    command_index: int | None,
    mode: OutputMode,
    expected: Mapping[str, object],
    actual: Mapping[str, object],
    difference: ValueDifference,
) -> Divergence:
    category = _failure_category(difference.path)
    expected_evidence = _nested_mapping(expected, "evidence")
    actual_evidence = _nested_mapping(actual, "evidence")
    expected_state = _nested_mapping(expected_evidence, "state")
    actual_state = _nested_mapping(actual_evidence, "state")
    expected_snapshot = expected_evidence.get("snapshot")
    actual_snapshot = actual_evidence.get("snapshot")
    return Divergence(
        command_index=command_index,
        mode=mode,
        signature=FailureSignature(
            category=category,
            field_path=difference.path,
            expected_kind=_signature_kind(difference.path, difference.expected),
            actual_kind=_signature_kind(difference.path, difference.actual),
        ),
        difference=difference,
        depth_difference=first_depth_difference(
            expected_snapshot,
            actual_snapshot,
        ),
        reference_state_digest=_optional_string(expected_state.get("state_digest")),
        native_state_digest=_optional_string(actual_state.get("state_digest")),
        reference_top=_state_top(expected_state),
        native_top=_state_top(actual_state),
        exact_prefix_required=mode == "compact",
    )


def _failure_category(path: str) -> FailureCategory:
    if ".events" in path or ".event_digest" in path:
        return "event"
    if ".snapshot" in path:
        return "snapshot"
    if ".state" in path:
        return "state"
    if any(
        field in path
        for field in (
            ".outcome",
            ".command_sequence",
            ".engine_error",
            ".reject_reason",
            ".command_type",
        )
    ):
        return "classification"
    if ".kind" in path or ".command_index" in path or ".commands_processed" in path:
        return "stream"
    return "final"


def _signature_kind(path: str, value: object) -> str:
    if path.endswith(
        (
            ".type",
            ".reason",
            ".reject_reason",
            ".outcome",
            ".command_type",
            ".engine_error",
            ".kind",
        )
    ) and isinstance(value, str):
        return value
    return type(value).__name__


def _state_top(state: Mapping[str, object]) -> Mapping[str, object] | None:
    if not state:
        return None
    return {
        "best_bid": state.get("best_bid"),
        "best_ask": state.get("best_ask"),
    }


def _write_record(output: IO[bytes], value: Mapping[str, object]) -> None:
    output.write(_canonical_json(value))


def _canonical_json(value: Mapping[str, object]) -> bytes:
    return (
        json.dumps(
            value,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode("ascii")


def _update_mapping_digest(
    hasher: hashlib._Hash,
    value: Mapping[str, object],
) -> None:
    hasher.update(_canonical_json(value))


def _counts_from_mutable(values: Mapping[str, int]) -> ClassificationCounts:
    return ClassificationCounts(
        committed=values["committed"],
        rejected=values["rejected"],
        engine_error=values["engine_error"],
    )


def _is_checkpoint(command_index: int, interval: int) -> bool:
    return interval != 0 and (command_index + 1) % interval == 0


def _remaining(deadline: float | None) -> float | None:
    if deadline is None:
        return None
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("native differential process timed out")
    return remaining


def _deadline(timeout: float | None) -> float | None:
    if timeout is not None and (
        isinstance(timeout, bool)
        or not isinstance(timeout, (int, float))
        or not 0 < timeout < float("inf")
    ):
        raise ValueError("timeout must be finite and positive or None")
    return None if timeout is None else time.monotonic() + timeout


def _path_identity(path: Path) -> str:
    return os.path.normcase(str(Path(path).resolve()))


def _require_distinct_paths(paths: Mapping[str, Path]) -> None:
    identities: dict[str, str] = {}
    for name, path in paths.items():
        identity = _path_identity(path)
        previous = identities.get(identity)
        if previous is not None:
            raise ValueError(f"{name} collides with {previous}: {path}")
        identities[identity] = name


def _remove_stale_outputs(paths: Iterable[Path]) -> None:
    for path in paths:
        Path(path).unlink(missing_ok=True)


def _stop_process(
    process: subprocess.Popen[bytes],
    pump: _LinePump | None,
) -> None:
    if pump is not None:
        pump.stop()
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5.0)
    if process.stdout is not None:
        process.stdout.close()
    if pump is not None:
        pump.join()


def _capture_failure_result(
    *,
    case_name: str,
    mode: OutputMode,
    workload_path: Path,
    evidence_path: Path,
    workload_manifest_path: Path | None,
    error: BaseException,
) -> CampaignResult:
    return CampaignResult(
        case_name=case_name,
        mode=mode,
        status="harness_error",
        workload_path=workload_path,
        workload_manifest_path=(
            Path(workload_manifest_path).resolve() if workload_manifest_path is not None else None
        ),
        reference_evidence_path=evidence_path,
        native_evidence_path=None,
        workload_command_count=0,
        commands_compared=0,
        reference_classifications=ClassificationCounts(),
        native_classifications=ClassificationCounts(),
        command_digest=hashlib.sha256().hexdigest(),
        reference_evidence_digest=hashlib.sha256().hexdigest(),
        native_evidence_digest=hashlib.sha256().hexdigest(),
        native_returncode=None,
        harness_error=_exception_text(error),
    )


def _exception_text(error: BaseException) -> str:
    message = str(error)
    return f"{type(error).__name__}: {message}" if message else type(error).__name__


def _safe_case_name(value: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9._-]+", "_", value).strip("._")
    prefix = safe or "case"
    suffix = hashlib.sha256(value.encode("utf-8")).hexdigest()[:8]
    return f"{prefix}-{suffix}"


def _string_mapping(value: object, name: str) -> Mapping[str, object]:
    if not isinstance(value, dict) or any(not isinstance(key, str) for key in value):
        raise ValueError(f"{name} must be an object with string keys")
    return cast(Mapping[str, object], value)


def _nested_mapping(
    value: Mapping[str, object],
    key: str,
) -> Mapping[str, object]:
    nested = value.get(key)
    if not isinstance(nested, dict):
        return {}
    return cast(Mapping[str, object], nested)


def _optional_string(value: object) -> str | None:
    return value if isinstance(value, str) else None


def _mapping_price(value: object) -> int | None:
    if not isinstance(value, Mapping):
        return None
    price = value.get("price")
    if not isinstance(price, str):
        return None
    try:
        return int(price)
    except ValueError:
        return None


def _unique_reference_object(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"reference JSON contains duplicate field: {key}")
        result[key] = value
    return result


def _reject_reference_constant(value: str) -> NoReturn:
    raise ValueError(f"reference JSON contains non-finite constant: {value}")
