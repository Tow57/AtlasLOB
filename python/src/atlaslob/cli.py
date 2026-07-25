"""Command-line entry point for deterministic Phase 3 evidence campaigns."""

from __future__ import annotations

import argparse
import errno
import hashlib
import math
import os
import shutil
import stat
import subprocess
import tempfile
import time
from collections.abc import Sequence
from dataclasses import asdict, dataclass
from pathlib import Path

from atlaslob.campaigns import (
    PredefinedCampaign,
    campaign_from_json,
    campaign_json,
    main_campaign,
    nightly_campaign,
    pr_campaign,
    release_campaign,
    release_sanitizer_campaign,
)
from atlaslob.differential import (
    CampaignResult,
    FailureSignature,
    rerun_exact_prefix,
    run_case,
    run_fixture,
)
from atlaslob.domain import Command
from atlaslob.native import NativeInputConfig, encode_command, encode_header
from atlaslob.reporting import (
    BuildMetadata,
    persist_initial_failure,
    persist_minimized_failure,
)
from atlaslob.shrinking import ShrinkBudget, ShrinkContext, shrink_failure
from atlaslob.workload import canonical_json, open_workload

_MAX_AUTO_SHRINK_COMMANDS = 100_000
_BUNDLE_OWNED_FILES = (
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
_CASE_OWNED_FILES = (
    "workload.atlas",
    "manifest.json",
    "reference.jsonl",
    "reference-native.jsonl",
)
_EXACT_PREFIX_OWNED_FILES = (
    "exact-prefix.atlas",
    "exact-prefix-reference.jsonl",
    "exact-prefix-reference-native.jsonl",
)


@dataclass(frozen=True, slots=True)
class _Diagnosis:
    bundle: Path | None = None
    harness_error: str | None = None
    status: str | None = None
    exact_replay_command_count: int | None = None
    exact_replay_command_limit: int | None = None


def main(arguments: Sequence[str] | None = None) -> int:
    """Run the evidence CLI and return its documented process status."""

    parser = _parser()
    options = parser.parse_args(arguments)
    try:
        if options.command == "fixture":
            return _fixture(options)
        if options.command == "campaign":
            return _campaign(options, _read_campaign(options.campaign))
        if options.command == "predefined":
            return _campaign(options, _predefined(options.tier, options.epoch))
        parser.error("a command is required")
    except (OSError, RuntimeError, ValueError) as exc:
        parser.exit(2, f"atlaslob-diff: {type(exc).__name__}: {exc}\n")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="atlaslob-diff",
        description="Run deterministic AtlasLOB differential evidence.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    fixture = subparsers.add_parser("fixture", help="compare one persisted workload")
    _common_execution_arguments(fixture)
    fixture.add_argument("--workload", type=Path, required=True)
    fixture.add_argument("--mode", choices=("exact", "compact"), default="exact")
    fixture.add_argument("--case-name", default="fixture")

    campaign = subparsers.add_parser("campaign", help="run a checked campaign JSON file")
    _common_execution_arguments(campaign)
    campaign.add_argument("--campaign", type=Path, required=True)
    _campaign_execution_arguments(campaign)

    predefined = subparsers.add_parser("predefined", help="run a versioned built-in campaign")
    _common_execution_arguments(predefined)
    predefined.add_argument(
        "--tier",
        choices=("pr", "main", "nightly", "release", "release-sanitizer"),
        required=True,
    )
    predefined.add_argument("--epoch", type=_u64)
    _campaign_execution_arguments(predefined)
    return parser


def _common_execution_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case-timeout", type=_positive_float, default=120.0)
    parser.add_argument(
        "--exact-replay-command-limit",
        type=_positive_int,
        help=(
            "defer exact-prefix diagnosis when its command count exceeds this limit; "
            "the default is unbounded"
        ),
    )
    parser.add_argument("--no-shrink", action="store_true")
    parser.add_argument("--shrink-evaluations", type=_positive_int, default=2_000)
    parser.add_argument("--shrink-timeout", type=_positive_float, default=120.0)


def _campaign_execution_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--retain-success-streams", action="store_true")
    parser.add_argument(
        "--case-index",
        type=_nonnegative_int,
        help="run only the zero-based case at this index",
    )
    parser.add_argument(
        "--summary-only",
        action="store_true",
        help=(
            "bound passing-case storage by disabling native stream capture and retaining only "
            "summaries and manifests; failures retain inputs and follow the configured "
            "exact-diagnosis policy"
        ),
    )


def _fixture(options: argparse.Namespace) -> int:
    executable = _native_path(options.native)
    workload = Path(options.workload).resolve()
    requested_output = Path(options.output).absolute()
    _validate_output_boundary(
        requested_output,
        {"native executable": executable, "workload": workload},
    )
    _require_no_link_components(requested_output)
    output = requested_output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    _reset_fixture_output(output)
    result = run_fixture(
        executable,
        workload,
        output / "reference.jsonl",
        mode=options.mode,
        case_name=options.case_name,
        timeout=options.case_timeout,
        native_evidence_path=output / "native.jsonl",
    )
    diagnosis = _Diagnosis()
    if result.divergence is not None:
        diagnosis = _diagnose(
            result,
            executable,
            output / "failure",
            shrink=not options.no_shrink,
            shrink_evaluations=options.shrink_evaluations,
            shrink_timeout=options.shrink_timeout,
            case_timeout=options.case_timeout,
            exact_replay_command_limit=options.exact_replay_command_limit,
            summary_only=False,
        )
    _write_json(
        output / "result.json",
        _result_mapping(
            result,
            artifact_root=output,
            diagnosis=diagnosis,
        ),
    )
    if result.passed:
        return 0
    if result.status == "harness_error" or diagnosis.harness_error is not None:
        return 2
    return 1


def _campaign(options: argparse.Namespace, campaign: PredefinedCampaign) -> int:
    if options.summary_only and options.retain_success_streams:
        raise ValueError("--summary-only cannot be combined with --retain-success-streams")
    executable = _native_path(options.native)
    selected_indices = _selected_case_indices(campaign, options.case_index)
    requested_output = Path(options.output).absolute()
    protected_inputs = {"native executable": executable}
    if options.command == "campaign":
        protected_inputs["campaign"] = Path(options.campaign).resolve()
    _validate_output_boundary(requested_output, protected_inputs)
    _require_no_link_components(requested_output)
    output = requested_output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    _reset_campaign_output(output)
    (output / "campaign.json").write_text(
        campaign_json(campaign),
        encoding="ascii",
        newline="\n",
    )

    case_summaries: list[object] = []
    status = 0
    suite_directory = output / "cases"
    for case_index in selected_indices:
        case = campaign.suite.cases[case_index]
        result = run_case(
            executable,
            case,
            suite_directory,
            mode=campaign.suite.mode,
            timeout=options.case_timeout,
            capture_native_evidence=not options.summary_only,
        )
        diagnosis = _Diagnosis()
        if not result.passed:
            diagnosis = _diagnose(
                result,
                executable,
                output / "failures" / _safe_name(case.name),
                shrink=not options.no_shrink,
                shrink_evaluations=options.shrink_evaluations,
                shrink_timeout=options.shrink_timeout,
                case_timeout=options.case_timeout,
                exact_replay_command_limit=options.exact_replay_command_limit,
                summary_only=options.summary_only,
            )
        if result.status == "harness_error" or diagnosis.harness_error is not None:
            status = 2
        elif not result.passed and status == 0:
            status = 1
        case_summaries.append(
            _result_mapping(
                result,
                artifact_root=output,
                diagnosis=diagnosis,
            )
        )
        _write_campaign_summary(
            output,
            campaign,
            selected_indices,
            case_summaries,
            summary_only=options.summary_only,
        )
        if result.passed and (options.summary_only or not options.retain_success_streams):
            _discard_case_streams(result)
        elif options.summary_only and result.divergence is None:
            _discard_evidence_streams(result)

    _write_campaign_summary(
        output,
        campaign,
        selected_indices,
        case_summaries,
        summary_only=options.summary_only,
    )
    return status


def _diagnose(
    result: CampaignResult,
    executable: Path,
    output: Path,
    *,
    shrink: bool,
    shrink_evaluations: int,
    shrink_timeout: float,
    case_timeout: float,
    exact_replay_command_limit: int | None,
    summary_only: bool,
) -> _Diagnosis:
    if result.divergence is None:
        return _Diagnosis()
    if exact_replay_command_limit is not None and (
        isinstance(exact_replay_command_limit, bool)
        or not isinstance(exact_replay_command_limit, int)
        or exact_replay_command_limit < 1
    ):
        raise ValueError("exact_replay_command_limit must be a positive integer or None")
    if not isinstance(summary_only, bool):
        raise ValueError("summary_only must be a bool")

    exact_replay_command_count = (
        result.workload_command_count
        if result.divergence.command_index is None
        else result.divergence.command_index + 1
    )
    exact_replay_deferred = (
        exact_replay_command_limit is not None
        and exact_replay_command_count > exact_replay_command_limit
    )

    source_name = "compact" if result.mode == "compact" else "original"
    source_status = (
        "not_applicable_compact_source"
        if result.mode == "compact"
        else "not_applicable_replay_source"
    )
    source_bundle = persist_initial_failure(
        result,
        output / source_name,
        executable,
        build=_build_metadata(),
        shrink_status=source_status,
        retain_transcripts=not summary_only,
        diagnosis_status=("deferred_command_limit" if exact_replay_deferred else None),
        exact_replay_command_count=(exact_replay_command_count if exact_replay_deferred else None),
        exact_replay_command_limit=(exact_replay_command_limit if exact_replay_deferred else None),
    )
    replay_source = result.workload_path
    if summary_only:
        replay_source = source_bundle.original_workload_path
        _discard_case_streams(result)
    if exact_replay_deferred:
        return _Diagnosis(
            bundle=source_bundle.root,
            status="deferred_command_limit",
            exact_replay_command_count=exact_replay_command_count,
            exact_replay_command_limit=exact_replay_command_limit,
        )

    diagnostic = rerun_exact_prefix(
        executable,
        replay_source,
        result.divergence.command_index,
        output / "exact-prefix",
        case_name=f"{result.case_name}-exact-prefix",
        timeout=case_timeout,
        capture_native_evidence=not summary_only,
    )
    _write_json(
        output / "exact-prefix-result.json",
        _result_mapping(diagnostic, artifact_root=output),
    )
    if diagnostic.status == "harness_error":
        if summary_only:
            _discard_case_streams(diagnostic)
        return _Diagnosis(
            bundle=source_bundle.root,
            harness_error=diagnostic.harness_error or "exact-prefix diagnosis failed",
            status="exact_replay_harness_error",
            exact_replay_command_count=exact_replay_command_count,
            exact_replay_command_limit=exact_replay_command_limit,
        )
    if diagnostic.divergence is None:
        if summary_only:
            _discard_case_streams(diagnostic)
        return _Diagnosis(
            bundle=source_bundle.root,
            status="exact_replay_passed",
            exact_replay_command_count=exact_replay_command_count,
            exact_replay_command_limit=exact_replay_command_limit,
        )

    requested_budget = (
        ShrinkBudget(
            max_evaluations=shrink_evaluations,
            timeout_seconds=shrink_timeout,
        )
        if shrink
        else None
    )
    if not shrink:
        shrink_status = "disabled"
    elif diagnostic.workload_command_count > _MAX_AUTO_SHRINK_COMMANDS:
        shrink_status = "skipped_command_limit"
    else:
        shrink_status = "pending"

    bundle = persist_initial_failure(
        diagnostic,
        output / "bundle",
        executable,
        build=_build_metadata(),
        shrink_budget=requested_budget,
        shrink_status=shrink_status,
        shrink_command_limit=_MAX_AUTO_SHRINK_COMMANDS,
        source_manifest_path=result.workload_manifest_path,
        source_bundle=f"../{source_name}",
        retain_transcripts=not summary_only,
    )
    diagnostic_workload = diagnostic.workload_path
    if summary_only:
        diagnostic_workload = bundle.original_workload_path
        _discard_case_streams(diagnostic)
    if shrink_status != "pending":
        return _Diagnosis(
            bundle=bundle.root,
            status="exact_replay_diverged",
            exact_replay_command_count=exact_replay_command_count,
            exact_replay_command_limit=exact_replay_command_limit,
        )

    reader = open_workload(diagnostic_workload)
    commands = tuple(reader.commands())
    if diagnostic.divergence is None:
        return _Diagnosis(
            bundle=bundle.root,
            status="exact_replay_diverged",
            exact_replay_command_count=exact_replay_command_count,
            exact_replay_command_limit=exact_replay_command_limit,
        )
    if requested_budget is None:
        raise RuntimeError("shrink budget is unavailable for a pending reduction")
    signature = diagnostic.divergence.signature
    with tempfile.TemporaryDirectory(prefix="atlaslob-shrink-") as temporary:
        evaluation_root = Path(temporary)

        def evaluate(
            candidate: tuple[Command, ...],
            remaining: float | None,
        ) -> FailureSignature | None:
            evaluation_timeout = (
                case_timeout if remaining is None else min(case_timeout, max(remaining, 0.000_001))
            )
            return _candidate_signature(
                executable,
                reader.config,
                candidate,
                evaluation_root,
                evaluation_timeout,
            )

        reduced = shrink_failure(
            commands,
            evaluate,
            signature,
            context=ShrinkContext(
                routed_instrument=reader.config.instrument_id,
                tick_increment=reader.config.engine.tick_increment,
                max_quantity=reader.config.engine.max_order_quantity,
            ),
            budget=requested_budget,
        )
    persist_minimized_failure(
        bundle,
        reduced,
        executable,
        reader.config,
        timeout=case_timeout,
    )
    return _Diagnosis(
        bundle=bundle.root,
        status="exact_replay_diverged",
        exact_replay_command_count=exact_replay_command_count,
        exact_replay_command_limit=exact_replay_command_limit,
    )


def _candidate_signature(
    executable: Path,
    config: NativeInputConfig,
    commands: Sequence[Command],
    root: Path,
    timeout: float,
) -> FailureSignature | None:
    deadline = time.monotonic() + timeout
    directory = root / _fixture_digest(config, commands, deadline=deadline)
    directory.mkdir(parents=True, exist_ok=True)
    workload = directory / "candidate.atlas"
    _write_fixture(workload, config, commands, deadline=deadline)
    result = run_fixture(
        executable,
        workload,
        directory / "reference.jsonl",
        mode="exact",
        timeout=_remaining_time(deadline),
        native_evidence_path=directory / "native.jsonl",
    )
    signature = result.divergence.signature if result.divergence is not None else None
    if result.status == "harness_error":
        raise RuntimeError(result.harness_error or "candidate comparison failed")
    shutil.rmtree(directory, ignore_errors=True)
    return signature


def _fixture_digest(
    config: NativeInputConfig,
    commands: Sequence[Command],
    *,
    deadline: float,
) -> str:
    digest = hashlib.sha256()
    digest.update((encode_header(config) + "\n").encode("ascii"))
    for command in commands:
        _remaining_time(deadline)
        digest.update((encode_command(command) + "\n").encode("ascii"))
    return digest.hexdigest()


def _write_fixture(
    path: Path,
    config: NativeInputConfig,
    commands: Sequence[Command],
    *,
    deadline: float,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    with temporary.open("wb") as output:
        output.write((encode_header(config) + "\n").encode("ascii"))
        for command in commands:
            _remaining_time(deadline)
            output.write((encode_command(command) + "\n").encode("ascii"))
        output.flush()
    temporary.replace(path)


def _result_mapping(
    result: CampaignResult,
    *,
    artifact_root: Path | None = None,
    diagnosis: _Diagnosis | None = None,
) -> dict[str, object]:
    divergence = result.divergence
    diagnosis = diagnosis or _Diagnosis()
    return {
        "case_name": result.case_name,
        "mode": result.mode,
        "status": ("harness_error" if diagnosis.harness_error is not None else result.status),
        "workload_command_count": str(result.workload_command_count),
        "commands_compared": str(result.commands_compared),
        "classifications": {
            "reference": asdict(result.reference_classifications),
            "native": asdict(result.native_classifications),
        },
        "digests": {
            "command_records": result.command_digest,
            "reference_evidence": result.reference_evidence_digest,
            "native_evidence": result.native_evidence_digest,
        },
        "native_returncode": result.native_returncode,
        "harness_error": diagnosis.harness_error or result.harness_error,
        "manifest": (
            _relative_artifact(result.workload_manifest_path, artifact_root)
            if result.workload_manifest_path is not None
            else None
        ),
        "failure_bundle": (
            _relative_artifact(diagnosis.bundle, artifact_root)
            if diagnosis.bundle is not None
            else None
        ),
        "diagnosis": (
            None
            if diagnosis.status is None
            else {
                "status": diagnosis.status,
                "exact_replay_command_count": (
                    None
                    if diagnosis.exact_replay_command_count is None
                    else str(diagnosis.exact_replay_command_count)
                ),
                "exact_replay_command_limit": (
                    None
                    if diagnosis.exact_replay_command_limit is None
                    else str(diagnosis.exact_replay_command_limit)
                ),
            }
        ),
        "failure": (
            None
            if divergence is None
            else {
                "command_index": (
                    None if divergence.command_index is None else str(divergence.command_index)
                ),
                "category": divergence.signature.category,
                "first_field": divergence.signature.field_path,
                "expected_kind": divergence.signature.expected_kind,
                "actual_kind": divergence.signature.actual_kind,
                "exact_prefix_required": divergence.exact_prefix_required,
            }
        ),
    }


def _write_campaign_summary(
    output: Path,
    campaign: PredefinedCampaign,
    selected_indices: tuple[int, ...],
    cases: list[object],
    *,
    summary_only: bool,
) -> None:
    passed = all(isinstance(case, dict) and case.get("status") == "passed" for case in cases)
    _write_json(
        output / "summary.json",
        {
            "schema": "atlas_campaign_summary_v1",
            "campaign_policy_version": campaign.policy_version,
            "suite": campaign.suite.name,
            "tier": campaign.suite.tier,
            "mode": campaign.suite.mode,
            "seed_set_id": campaign.provenance.seed_set_id,
            "epoch": (
                None if campaign.provenance.epoch is None else str(campaign.provenance.epoch)
            ),
            "artifact_policy": "summary_only" if summary_only else "diagnostic",
            "campaign_case_count": str(len(campaign.suite.cases)),
            "selected_case_indices": [str(index) for index in selected_indices],
            "expected_case_count": str(len(selected_indices)),
            "completed_case_count": str(len(cases)),
            "passed": passed and len(cases) == len(selected_indices),
            "cases": cases,
        },
    )


def _selected_case_indices(
    campaign: PredefinedCampaign,
    case_index: int | None,
) -> tuple[int, ...]:
    if case_index is None:
        return tuple(range(len(campaign.suite.cases)))
    if case_index >= len(campaign.suite.cases):
        raise ValueError(
            f"--case-index {case_index} is outside campaign range "
            f"0..{len(campaign.suite.cases) - 1}"
        )
    return (case_index,)


def _discard_case_streams(result: CampaignResult) -> None:
    result.workload_path.unlink(missing_ok=True)
    _discard_evidence_streams(result)


def _discard_evidence_streams(result: CampaignResult) -> None:
    result.reference_evidence_path.unlink(missing_ok=True)
    if result.native_evidence_path is not None:
        result.native_evidence_path.unlink(missing_ok=True)


def _reset_fixture_output(output: Path) -> None:
    files: list[Path] = []
    directories: list[Path] = []
    _plan_owned_files(
        files,
        output,
        ("result.json", "reference.jsonl", "native.jsonl"),
    )
    _plan_diagnosis_directory(files, directories, output / "failure")
    _apply_cleanup(files, directories)


def _reset_campaign_output(output: Path) -> None:
    files: list[Path] = []
    directories: list[Path] = []
    _plan_owned_files(files, output, ("campaign.json", "summary.json"))
    _plan_case_container(files, directories, output / "cases")
    _plan_failure_container(files, directories, output / "failures")
    _apply_cleanup(files, directories)


def _plan_case_container(
    files: list[Path],
    directories: list[Path],
    root: Path,
) -> None:
    if not _plan_directory(root):
        return
    for child in root.iterdir():
        if _is_link_or_junction(child):
            raise ValueError(f"runner-owned case container contains a link: {child.name}")
        try:
            metadata = child.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISDIR(metadata.st_mode):
            _plan_owned_files(files, child, _CASE_OWNED_FILES)
            directories.append(child)
    directories.append(root)


def _plan_failure_container(
    files: list[Path],
    directories: list[Path],
    root: Path,
) -> None:
    if not _plan_directory(root):
        return
    for child in root.iterdir():
        if _is_link_or_junction(child):
            raise ValueError(f"runner-owned failure container contains a link: {child.name}")
        try:
            metadata = child.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISDIR(metadata.st_mode):
            _plan_diagnosis_contents(files, directories, child)
            directories.append(child)
    directories.append(root)


def _plan_diagnosis_directory(
    files: list[Path],
    directories: list[Path],
    root: Path,
) -> None:
    if not _plan_directory(root):
        return
    _plan_diagnosis_contents(files, directories, root)
    directories.append(root)


def _plan_diagnosis_contents(
    files: list[Path],
    directories: list[Path],
    root: Path,
) -> None:
    _plan_owned_files(files, root, ("exact-prefix-result.json",))
    for name in ("compact", "original", "bundle"):
        child = root / name
        if _plan_directory(child):
            _plan_owned_files(files, child, _BUNDLE_OWNED_FILES)
            directories.append(child)
    exact_prefix = root / "exact-prefix"
    if _plan_directory(exact_prefix):
        _plan_owned_files(files, exact_prefix, _EXACT_PREFIX_OWNED_FILES)
        directories.append(exact_prefix)


def _plan_owned_files(files: list[Path], root: Path, names: Sequence[str]) -> None:
    for name in names:
        for path in (root / name, root / f".{name}.tmp"):
            try:
                metadata = path.lstat()
            except FileNotFoundError:
                continue
            if _is_link_or_junction(path) or stat.S_ISDIR(metadata.st_mode):
                raise ValueError(f"runner-owned output file has an unexpected type: {path.name}")
            files.append(path)


def _plan_directory(path: Path) -> bool:
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return False
    if _is_link_or_junction(path):
        raise ValueError(f"runner-owned output directory must not be a link: {path.name}")
    if not stat.S_ISDIR(metadata.st_mode):
        raise ValueError(f"runner-owned output directory has an unexpected type: {path.name}")
    return True


def _apply_cleanup(files: Sequence[Path], directories: Sequence[Path]) -> None:
    for path in files:
        path.unlink(missing_ok=True)
    for path in directories:
        try:
            path.rmdir()
        except FileNotFoundError:
            continue
        except OSError as exc:
            if exc.errno not in (errno.EEXIST, errno.ENOTEMPTY):
                raise


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
            raise ValueError(f"output path contains a symbolic link or junction: {current.name}")


def _is_link_or_junction(path: Path) -> bool:
    if path.is_symlink():
        return True
    is_junction = getattr(path, "is_junction", None)
    return bool(is_junction is not None and is_junction())


def _predefined(tier: str, epoch: int | None) -> PredefinedCampaign:
    if tier == "pr":
        if epoch is not None:
            raise ValueError("the fixed PR campaign does not accept an epoch")
        return pr_campaign()
    if tier == "main":
        if epoch is None:
            raise ValueError("the rotating main campaign requires --epoch")
        return main_campaign(epoch)
    if tier == "nightly":
        if epoch is None:
            raise ValueError("the rotating nightly campaign requires --epoch")
        return nightly_campaign(epoch)
    if tier == "release":
        if epoch is not None:
            raise ValueError("the published release campaign does not accept an epoch")
        return release_campaign()
    if tier == "release-sanitizer":
        if epoch is not None:
            raise ValueError("the release sanitizer campaign does not accept an epoch")
        return release_sanitizer_campaign()
    raise ValueError(f"unknown predefined tier: {tier}")


def _read_campaign(path: Path) -> PredefinedCampaign:
    return campaign_from_json(Path(path).read_text(encoding="ascii"))


def _native_path(path: Path) -> Path:
    resolved = Path(path).resolve()
    if not resolved.is_file():
        raise ValueError(f"native evidence executable does not exist: {path}")
    return resolved


def _build_metadata() -> BuildMetadata:
    revision = os.environ.get("GITHUB_SHA") or _git_revision()
    compiler_value = os.environ.get("ATLAS_COMPILER_ID") or os.environ.get("CXX") or "unknown"
    compiler = Path(compiler_value).name if compiler_value != "unknown" else compiler_value
    return BuildMetadata(
        revision=revision,
        compiler=compiler,
        build_type=os.environ.get("ATLAS_BUILD_TYPE", "unknown"),
    )


def _git_revision() -> str:
    try:
        completed = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            text=True,
            capture_output=True,
            check=False,
            timeout=5,
        )
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    value = completed.stdout.strip()
    return value if completed.returncode == 0 and value else "unknown"


def _write_json(path: Path, value: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_text(canonical_json(value), encoding="ascii", newline="\n")
    temporary.replace(path)


def _safe_name(value: str) -> str:
    normalized = "".join(
        character if character.isalnum() or character in "-_" else "-" for character in value
    )
    return normalized or "case"


def _positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("value must be a positive integer")
    return parsed


def _nonnegative_int(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("value must be nonnegative")
    return parsed


def _positive_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or parsed <= 0:
        raise argparse.ArgumentTypeError("value must be finite and positive")
    return parsed


def _u64(value: str) -> int:
    parsed = int(value)
    if not 0 <= parsed <= (1 << 64) - 1:
        raise argparse.ArgumentTypeError("epoch must be a u64")
    return parsed


def _validate_output_boundary(output: Path, protected: dict[str, Path]) -> None:
    resolved_output = Path(output).resolve()
    for name, path in protected.items():
        resolved_input = Path(path).resolve()
        if resolved_input == resolved_output or resolved_output in resolved_input.parents:
            raise ValueError(f"output directory contains the {name}: {resolved_input.name}")


def _relative_artifact(path: Path, root: Path | None) -> str:
    resolved = Path(path).resolve()
    if root is None:
        return resolved.name
    try:
        return resolved.relative_to(Path(root).resolve()).as_posix()
    except ValueError:
        return Path(os.path.relpath(resolved, Path(root).resolve())).as_posix()


def _remaining_time(deadline: float) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("candidate evaluation timed out")
    return remaining


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
