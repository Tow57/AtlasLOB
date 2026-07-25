"""Versioned, portable differential failure bundles."""

from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import stat
from collections import deque
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Final, NoReturn

from atlaslob.differential import (
    CampaignResult,
    ClassificationCounts,
    Divergence,
    EvidenceTransformer,
    FailureSignature,
    run_fixture,
)
from atlaslob.domain import Command
from atlaslob.native import NativeInputConfig, encode_command, encode_header
from atlaslob.shrinking import ShrinkBudget, ShrinkResult
from atlaslob.workload import (
    canonical_json,
    manifest_to_dict,
    open_workload,
    read_manifest,
)

FAILURE_REPORT_SCHEMA: Final = "atlas_failure_report_v1"
DEFAULT_RECENT_COMMANDS: Final = 20
_BUNDLE_OWNED_FILES: Final = (
    "original.atlas",
    "manifest.json",
    "source-manifest.json",
    "reference-original.jsonl",
    "native-original.jsonl",
    "report.json",
    "minimized.atlas",
    "reference-minimized.jsonl",
    "native-minimized.jsonl",
)


@dataclass(frozen=True, slots=True)
class BuildMetadata:
    """Portable build identity recorded with a failure."""

    revision: str = "unknown"
    compiler: str = "unknown"
    build_type: str = "unknown"

    def __post_init__(self) -> None:
        for name, value in (
            ("revision", self.revision),
            ("compiler", self.compiler),
            ("build_type", self.build_type),
        ):
            if not value or "\n" in value or "\r" in value:
                raise ValueError(f"{name} must be a nonempty single-line value")


@dataclass(frozen=True, slots=True)
class FailureBundle:
    """Paths and immutable identity for an initialized failure bundle."""

    root: Path
    report_path: Path
    original_workload_path: Path
    original_reference_path: Path | None
    original_native_path: Path | None
    signature: FailureSignature


_DEFAULT_BUILD = BuildMetadata()


def persist_initial_failure(
    result: CampaignResult,
    output_directory: Path,
    native_executable: Path,
    *,
    build: BuildMetadata = _DEFAULT_BUILD,
    recent_limit: int = DEFAULT_RECENT_COMMANDS,
    injected_fault: str | None = None,
    shrink_budget: ShrinkBudget | None = None,
    shrink_status: str = "not_requested",
    shrink_command_limit: int | None = None,
    source_manifest_path: Path | None = None,
    source_bundle: str | None = None,
    retain_transcripts: bool = True,
    diagnosis_status: str | None = None,
    exact_replay_command_count: int | None = None,
    exact_replay_command_limit: int | None = None,
) -> FailureBundle:
    """Persist the original reproducer and initial report before shrinking."""

    if result.status == "passed":
        raise ValueError("a passing campaign cannot create a failure bundle")
    if result.divergence is None:
        raise ValueError("failure persistence currently requires a semantic divergence")
    if isinstance(recent_limit, bool) or not isinstance(recent_limit, int) or recent_limit < 1:
        raise ValueError("recent_limit must be a positive integer")
    if not isinstance(retain_transcripts, bool):
        raise ValueError("retain_transcripts must be a bool")
    if diagnosis_status is not None and (
        not isinstance(diagnosis_status, str)
        or not diagnosis_status
        or "\n" in diagnosis_status
        or "\r" in diagnosis_status
    ):
        raise ValueError("diagnosis_status must be a nonempty single-line string or None")
    if exact_replay_command_count is not None and (
        isinstance(exact_replay_command_count, bool)
        or not isinstance(exact_replay_command_count, int)
        or exact_replay_command_count < 0
    ):
        raise ValueError("exact_replay_command_count must be a nonnegative integer or None")
    if exact_replay_command_limit is not None and (
        isinstance(exact_replay_command_limit, bool)
        or not isinstance(exact_replay_command_limit, int)
        or exact_replay_command_limit < 1
    ):
        raise ValueError("exact_replay_command_limit must be a positive integer or None")
    if diagnosis_status is None and (
        exact_replay_command_count is not None or exact_replay_command_limit is not None
    ):
        raise ValueError("exact replay metadata requires diagnosis_status")
    if diagnosis_status == "deferred_command_limit" and (
        exact_replay_command_count is None or exact_replay_command_limit is None
    ):
        raise ValueError("deferred_command_limit requires exact replay count and limit")

    requested_root = Path(output_directory).absolute()
    _require_no_link_components(requested_root)
    root = requested_root.resolve()
    protected = [
        Path(native_executable),
        result.workload_path,
        result.reference_evidence_path,
        *([result.native_evidence_path] if result.native_evidence_path is not None else []),
        *([result.workload_manifest_path] if result.workload_manifest_path is not None else []),
        *([source_manifest_path] if source_manifest_path is not None else []),
    ]
    _require_output_outside_inputs(root, protected)
    root.mkdir(parents=True, exist_ok=True)
    _reset_bundle_owned_files(root)
    original_workload = root / "original.atlas"
    _copy(result.workload_path, original_workload)

    manifest_name: str | None = None
    manifest_mapping: Mapping[str, object] | None = None
    if result.workload_manifest_path is not None:
        manifest = read_manifest(result.workload_manifest_path)
        manifest_name = "manifest.json"
        _copy(result.workload_manifest_path, root / manifest_name)
        manifest_mapping = manifest_to_dict(manifest)

    source_manifest_name: str | None = None
    source_manifest_mapping: Mapping[str, object] | None = None
    if source_manifest_path is not None:
        source_manifest = read_manifest(source_manifest_path)
        source_manifest_name = "source-manifest.json"
        _copy(source_manifest_path, root / source_manifest_name)
        source_manifest_mapping = manifest_to_dict(source_manifest)

    if not retain_transcripts:
        (root / "reference-original.jsonl").unlink(missing_ok=True)
        (root / "native-original.jsonl").unlink(missing_ok=True)
    reference_path = (
        _copy_optional(
            result.reference_evidence_path,
            root / "reference-original.jsonl",
        )
        if retain_transcripts
        else None
    )
    native_path = (
        _copy_optional(
            result.native_evidence_path,
            root / "native-original.jsonl",
        )
        if retain_transcripts
        else None
    )
    report_path = root / "report.json"
    report = _initial_report(
        result=result,
        native_executable=Path(native_executable),
        build=build,
        recent_commands=_recent_commands(
            result.workload_path,
            result.divergence.command_index,
            recent_limit,
        ),
        manifest=manifest_mapping,
        manifest_name=manifest_name,
        reference_name=reference_path.name if reference_path is not None else None,
        native_name=native_path.name if native_path is not None else None,
        injected_fault=injected_fault,
        shrink_budget=shrink_budget,
        shrink_status=shrink_status,
        shrink_command_limit=shrink_command_limit,
        source_manifest=source_manifest_mapping,
        source_manifest_name=source_manifest_name,
        source_bundle=source_bundle,
        retain_transcripts=retain_transcripts,
        diagnosis_status=diagnosis_status,
        exact_replay_command_count=exact_replay_command_count,
        exact_replay_command_limit=exact_replay_command_limit,
    )
    report["artifact_sha256"] = {
        "original_workload": _file_digest(original_workload),
        "manifest": (_file_digest(root / manifest_name) if manifest_name is not None else None),
        "source_manifest": (
            _file_digest(root / source_manifest_name) if source_manifest_name is not None else None
        ),
        "original_reference_output": (
            _file_digest(reference_path) if reference_path is not None else None
        ),
        "original_native_output": (_file_digest(native_path) if native_path is not None else None),
        "minimized_workload": None,
        "minimized_reference_output": None,
        "minimized_native_output": None,
    }
    _write_report(report_path, report)
    return FailureBundle(
        root=root.resolve(),
        report_path=report_path.resolve(),
        original_workload_path=original_workload.resolve(),
        original_reference_path=reference_path.resolve() if reference_path is not None else None,
        original_native_path=native_path.resolve() if native_path is not None else None,
        signature=result.divergence.signature,
    )


def persist_minimized_failure(
    bundle: FailureBundle,
    shrink: ShrinkResult[FailureSignature],
    native_executable: Path,
    config: NativeInputConfig,
    *,
    evidence_transformer: EvidenceTransformer | None = None,
    timeout: float = 30.0,
) -> CampaignResult:
    """Persist and verify the reducer output, then finalize the report."""

    if shrink.signature != bundle.signature:
        raise ValueError("shrink result signature differs from the initialized failure")
    minimized_workload = bundle.root / "minimized.atlas"
    _write_fixture(minimized_workload, config, shrink.commands)
    minimized = run_fixture(
        native_executable,
        minimized_workload,
        bundle.root / "reference-minimized.jsonl",
        mode="exact",
        case_name="minimized-reproducer",
        timeout=timeout,
        evidence_transformer=evidence_transformer,
        native_evidence_path=bundle.root / "native-minimized.jsonl",
    )
    if minimized.status == "harness_error":
        raise RuntimeError(minimized.harness_error or "minimized comparison harness failed")
    if minimized.divergence is None:
        raise ValueError("minimized workload no longer reproduces a divergence")
    if minimized.divergence.signature != bundle.signature:
        raise ValueError("minimized workload reproduces a different failure signature")

    initial = _read_report(bundle.report_path)
    final = dict(initial)
    final["stage"] = "minimized"
    final["minimized_failure"] = _divergence_mapping(minimized.divergence)
    initial_shrink = _mapping(final.get("shrink"), "shrink")
    final["shrink"] = {
        **initial_shrink,
        "status": "budget_exhausted" if shrink.budget_exhausted else "completed",
        "original_command_count": str(shrink.original_count),
        "minimized_command_count": str(len(shrink.commands)),
        "evaluations": str(shrink.evaluations),
        "cache_hits": str(shrink.cache_hits),
        "configured_max_evaluations": str(shrink.max_evaluations),
        "configured_timeout_seconds": _optional_seconds(shrink.timeout_seconds),
        "elapsed_seconds": _seconds(shrink.elapsed_seconds),
        "budget_exhausted": shrink.budget_exhausted,
        "completed_stages": list(shrink.completed_stages),
    }
    final["minimized_digests"] = {
        "command_records": minimized.command_digest,
        "reference_evidence": minimized.reference_evidence_digest,
        "native_evidence_before_failure": minimized.native_evidence_digest,
    }
    files = _mapping(final.get("files"), "files")
    final["files"] = {
        **files,
        "minimized_workload": "minimized.atlas",
        "minimized_reference_output": "reference-minimized.jsonl",
        "minimized_native_output": (
            minimized.native_evidence_path.name
            if minimized.native_evidence_path is not None
            else None
        ),
    }
    artifact_digests = _mapping(final.get("artifact_sha256"), "artifact_sha256")
    final["artifact_sha256"] = {
        **artifact_digests,
        "minimized_workload": _file_digest(minimized_workload),
        "minimized_reference_output": _file_digest(bundle.root / "reference-minimized.jsonl"),
        "minimized_native_output": (
            _file_digest(minimized.native_evidence_path)
            if minimized.native_evidence_path is not None
            else None
        ),
    }
    final["reproduction"] = {
        "working_directory": ".",
        "exact_command": (
            "atlaslob-diff fixture --native <atlas_diff_native> "
            "--workload minimized.atlas --output reproduction --mode exact"
        ),
    }
    _write_report(bundle.report_path, final)
    return minimized


def _initial_report(
    *,
    result: CampaignResult,
    native_executable: Path,
    build: BuildMetadata,
    recent_commands: list[object],
    manifest: Mapping[str, object] | None,
    manifest_name: str | None,
    reference_name: str | None,
    native_name: str | None,
    injected_fault: str | None,
    shrink_budget: ShrinkBudget | None,
    shrink_status: str,
    shrink_command_limit: int | None,
    source_manifest: Mapping[str, object] | None,
    source_manifest_name: str | None,
    source_bundle: str | None,
    retain_transcripts: bool,
    diagnosis_status: str | None,
    exact_replay_command_count: int | None,
    exact_replay_command_limit: int | None,
) -> dict[str, object]:
    divergence = result.divergence
    if divergence is None:
        raise ValueError("divergence is required")
    return {
        "schema": FAILURE_REPORT_SCHEMA,
        "stage": "initial",
        "case_name": result.case_name,
        "mode": result.mode,
        "status": result.status,
        "transcript_policy": "retained" if retain_transcripts else "omitted",
        "diagnosis": (
            None
            if diagnosis_status is None
            else {
                "status": diagnosis_status,
                "exact_replay_command_count": (
                    None if exact_replay_command_count is None else str(exact_replay_command_count)
                ),
                "exact_replay_command_limit": (
                    None if exact_replay_command_limit is None else str(exact_replay_command_limit)
                ),
            }
        ),
        "injected_fault": injected_fault,
        "failure": _divergence_mapping(divergence),
        "manifest": manifest,
        "source_provenance": (
            None
            if source_manifest is None and source_bundle is None
            else {
                "manifest": source_manifest,
                "manifest_file": source_manifest_name,
                "source_bundle": source_bundle,
                "relationship": (
                    "derived_exact_prefix_of_manifest_workload"
                    if source_manifest is not None
                    else "derived_exact_prefix_of_source_bundle"
                ),
            }
        ),
        "commands": {
            "workload_count": str(result.workload_command_count),
            "compared_before_failure": str(result.commands_compared),
            "recent": recent_commands,
        },
        "classifications": {
            "reference": _counts_mapping(result.reference_classifications),
            "native": _counts_mapping(result.native_classifications),
        },
        "digests": {
            "command_records": result.command_digest,
            "reference_evidence": result.reference_evidence_digest,
            "native_evidence_before_failure": result.native_evidence_digest,
        },
        "build": {
            "revision": build.revision,
            "compiler": build.compiler,
            "build_type": build.build_type,
            "native_binary_name": native_executable.name,
            "native_binary_sha256": _file_digest(native_executable),
        },
        "runtime": {
            "python_implementation": platform.python_implementation(),
            "python_version": platform.python_version(),
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
        },
        "files": {
            "original_workload": "original.atlas",
            "manifest": manifest_name,
            "source_manifest": source_manifest_name,
            "original_reference_output": reference_name,
            "original_native_output": native_name,
            "minimized_workload": None,
            "minimized_reference_output": None,
            "minimized_native_output": None,
        },
        "shrink": {
            "status": shrink_status,
            "configured_max_evaluations": (
                None if shrink_budget is None else str(shrink_budget.max_evaluations)
            ),
            "configured_timeout_seconds": (
                None if shrink_budget is None else _optional_seconds(shrink_budget.timeout_seconds)
            ),
            "automatic_command_limit": (
                None if shrink_command_limit is None else str(shrink_command_limit)
            ),
            "elapsed_seconds": None,
            "evaluations": None,
            "cache_hits": None,
            "budget_exhausted": None,
        },
        "reproduction": {
            "working_directory": ".",
            "exact_command": (
                "atlaslob-diff fixture --native <atlas_diff_native> "
                "--workload original.atlas --output reproduction --mode exact"
            ),
        },
    }


def _divergence_mapping(divergence: Divergence) -> dict[str, object]:
    depth = divergence.depth_difference
    return {
        "command_index": (
            None if divergence.command_index is None else str(divergence.command_index)
        ),
        "category": divergence.signature.category,
        "first_field": divergence.signature.field_path,
        "expected_kind": divergence.signature.expected_kind,
        "actual_kind": divergence.signature.actual_kind,
        "expected": divergence.difference.expected,
        "actual": divergence.difference.actual,
        "reference_state_digest": divergence.reference_state_digest,
        "native_state_digest": divergence.native_state_digest,
        "reference_top": divergence.reference_top,
        "native_top": divergence.native_top,
        "exact_prefix_required": divergence.exact_prefix_required,
        "depth_difference": (
            None
            if depth is None
            else {
                "side": depth.side,
                "level_index": str(depth.level_index),
                "order_index": None if depth.order_index is None else str(depth.order_index),
                "expected_price": (
                    None if depth.expected_price is None else str(depth.expected_price)
                ),
                "actual_price": None if depth.actual_price is None else str(depth.actual_price),
                "field": depth.difference.path,
                "expected": depth.difference.expected,
                "actual": depth.difference.actual,
            }
        ),
    }


def _counts_mapping(value: ClassificationCounts) -> dict[str, object]:
    return {
        "committed": str(value.committed),
        "rejected": str(value.rejected),
        "engine_error": str(value.engine_error),
    }


def _recent_commands(path: Path, command_index: int | None, limit: int) -> list[object]:
    recent: deque[tuple[int, str]] = deque(maxlen=limit)
    for index, command in enumerate(open_workload(path).commands()):
        if command_index is not None and index > command_index:
            break
        recent.append((index, encode_command(command)))
    return [{"index": str(index), "command": command} for index, command in recent]


def _write_fixture(
    path: Path,
    config: NativeInputConfig,
    commands: Sequence[Command],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    with temporary.open("wb") as output:
        output.write((encode_header(config) + "\n").encode("ascii"))
        for command in commands:
            output.write((encode_command(command) + "\n").encode("ascii"))
        output.flush()
    temporary.replace(path)


def _copy(source: Path, destination: Path) -> None:
    source = Path(source)
    destination = Path(destination)
    if not source.is_file():
        raise ValueError(f"failure artifact source does not exist: {source.name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if source.resolve() != destination.resolve():
        temporary = destination.with_name(f".{destination.name}.tmp")
        temporary.unlink(missing_ok=True)
        try:
            os.link(source, temporary)
        except OSError:
            shutil.copyfile(source, temporary)
        temporary.replace(destination)


def _reset_bundle_owned_files(root: Path) -> None:
    owned = tuple(
        path for name in _BUNDLE_OWNED_FILES for path in (root / name, root / f".{name}.tmp")
    )
    for path in owned:
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            continue
        if _is_link_or_junction(path) or stat.S_ISDIR(metadata.st_mode):
            raise ValueError(f"runner-owned bundle file has an unexpected type: {path.name}")
    for path in owned:
        path.unlink(missing_ok=True)


def _is_link_or_junction(path: Path) -> bool:
    if path.is_symlink():
        return True
    is_junction = getattr(path, "is_junction", None)
    return bool(is_junction is not None and is_junction())


def _require_no_link_components(path: Path) -> None:
    absolute = Path(path).absolute()
    current = Path(absolute.anchor)
    for part in absolute.parts[1:]:
        current /= part
        try:
            current.lstat()
        except FileNotFoundError:
            continue
        if _is_link_or_junction(current):
            raise ValueError(f"failure bundle path contains a link or junction: {current.name}")


def _copy_optional(source: Path | None, destination: Path) -> Path | None:
    if source is None:
        return None
    _copy(source, destination)
    return destination


def _file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as input_file:
        while chunk := input_file.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _write_report(path: Path, value: Mapping[str, object]) -> None:
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_text(canonical_json(value), encoding="ascii", newline="\n")
    temporary.replace(path)


def _read_report(path: Path) -> Mapping[str, object]:
    try:
        text = Path(path).read_text(encoding="ascii")
        value = json.loads(
            text,
            object_pairs_hook=_unique_object,
            parse_constant=_reject_json_constant,
        )
    except (OSError, UnicodeError, json.JSONDecodeError, RecursionError) as exc:
        raise ValueError("failure report cannot be read") from exc
    mapping = _mapping(value, "failure report")
    if text != canonical_json(mapping):
        raise ValueError("failure report is not canonical")
    if mapping.get("schema") != FAILURE_REPORT_SCHEMA:
        raise ValueError("unsupported failure report schema")
    return mapping


def _mapping(value: object, name: str) -> Mapping[str, object]:
    if not isinstance(value, dict) or any(not isinstance(key, str) for key in value):
        raise ValueError(f"{name} must be an object with string keys")
    return value


def _seconds(value: float) -> str:
    return f"{value:.9f}".rstrip("0").rstrip(".") or "0"


def _optional_seconds(value: float | None) -> str | None:
    return None if value is None else _seconds(value)


def _unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"failure report contains duplicate key {key!r}")
        value[key] = item
    return value


def _reject_json_constant(value: str) -> NoReturn:
    raise ValueError(f"failure report contains invalid constant {value!r}")


def _require_output_outside_inputs(root: Path, inputs: Sequence[Path]) -> None:
    resolved_root = Path(root).resolve()
    for path in inputs:
        resolved = Path(path).resolve()
        if resolved == resolved_root or resolved_root in resolved.parents:
            raise ValueError(f"failure bundle would contain an input: {resolved.name}")
