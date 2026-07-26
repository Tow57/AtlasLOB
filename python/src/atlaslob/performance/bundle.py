"""Cross-file and SHA-256 inventory verification for Phase 5 evidence."""

from __future__ import annotations

import hashlib
import json
import os
import re
import stat
from collections.abc import Generator
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import BinaryIO

from atlaslob.performance.analysis import (
    ExperimentPlan,
    analyze_observations,
    render_report_markdown,
    render_report_svg,
)
from atlaslob.performance.schemas import (
    BenchmarkReport,
    EnvironmentManifest,
    Observation,
    WorkloadManifest,
    canonical_json_bytes,
    file_sha256,
    read_any_canonical_document,
    report_to_dict,
    validate_observation_against_workload,
)
from atlaslob.performance.workloads import verify_workload_manifest

INVENTORY_SCHEMA = "ATLAS_BENCH_BUNDLE_V1"
_INVENTORY_MAX_BYTES = 16 * 1024 * 1024
_MARKDOWN_MAX_BYTES = 64 * 1024 * 1024
_SVG_MAX_BYTES = 16 * 1024 * 1024


@dataclass(frozen=True, slots=True)
class BundleSummary:
    workloads: int
    environments: int
    observations: int
    reports: int
    files: int


def write_inventory(directory: Path) -> Path:
    if not directory.is_dir():
        raise ValueError("bundle path is not a directory")
    _reject_symlinks(directory)
    inventory_path = directory / "inventory.json"
    files = _bundle_files(directory)
    value = {
        "schema": INVENTORY_SCHEMA,
        "files": [
            {
                "path": path.relative_to(directory).as_posix(),
                "sha256": file_sha256(path),
            }
            for path in files
        ],
    }
    inventory_path.write_bytes(canonical_json_bytes(value))
    return inventory_path


def verify_bundle(directory: Path) -> BundleSummary:
    if not directory.is_dir():
        raise ValueError("bundle path is not a directory")
    _reject_symlinks(directory)
    files = _verify_inventory(directory)
    json_paths = tuple(path for path in files if path.suffix == ".json")
    if not json_paths:
        raise ValueError("bundle contains no evidence documents")

    workloads: dict[str, tuple[Path, WorkloadManifest]] = {}
    environments: dict[str, EnvironmentManifest] = {}
    observations: dict[str, Observation] = {}
    reports: list[tuple[Path, BenchmarkReport]] = []
    seen_documents: set[str] = set()
    for path in json_paths:
        value = read_any_canonical_document(path)
        digest = file_sha256(path)
        if digest in seen_documents:
            raise ValueError("bundle contains duplicate evidence documents")
        seen_documents.add(digest)
        if isinstance(value, WorkloadManifest):
            verified = verify_workload_manifest(path)
            if verified.stream_sha256 in workloads:
                raise ValueError("bundle contains duplicate workload stream evidence")
            workloads[verified.stream_sha256] = (path, verified)
        elif isinstance(value, EnvironmentManifest):
            environments[digest] = value
        elif isinstance(value, Observation):
            observations[digest] = value
        elif isinstance(value, BenchmarkReport):
            reports.append((path, value))
        else:  # pragma: no cover - exhaustive schema decoder
            raise AssertionError("unknown performance evidence value")
    if not workloads or not environments or not observations or not reports:
        raise ValueError("bundle requires workload, environment, observation, and report evidence")

    for observation in observations.values():
        workload_entry = workloads.get(observation.workload_sha256)
        environment = environments.get(observation.environment_sha256)
        if workload_entry is None or environment is None:
            raise ValueError("observation references missing workload/environment evidence")
        if (
            observation.binary_sha256 != environment.binary_sha256
            or observation.host_context_sha256 != environment.host_context_sha256
        ):
            raise ValueError("observation does not match its environment")
        validate_observation_against_workload(
            observation,
            workload_entry[1],
            manifest_sha256=file_sha256(workload_entry[0]),
        )

    workloads_by_document = {file_sha256(path): manifest for path, manifest in workloads.values()}
    if {item.workload_manifest_sha256 for item in observations.values()} != set(
        workloads_by_document
    ):
        raise ValueError("every workload manifest must be referenced by an observation")
    if {item.environment_sha256 for item in observations.values()} != set(environments):
        raise ValueError("every environment manifest must be referenced by an observation")
    for report_path, report in reports:
        if not set(report.source_observations).issubset(observations):
            raise ValueError("report references a missing observation")
        if not set(report.environment_sha256s).issubset(environments):
            raise ValueError("report references a missing environment")
        if not set(report.workload_manifest_sha256s).issubset(workloads_by_document):
            raise ValueError("report references a missing workload manifest")
        report_observations = tuple(observations[digest] for digest in report.source_observations)
        report_workload_digests = {item.workload_sha256 for item in report_observations}
        recomputed = analyze_observations(
            report_observations,
            workloads=tuple(
                (
                    file_sha256(workloads[digest][0]),
                    workloads[digest][1],
                )
                for digest in report_workload_digests
            ),
            environments=tuple(
                (digest, environments[digest]) for digest in report.environment_sha256s
            ),
            source_digests=report.source_observations,
            limitations=report.limitations,
            experiment_plans=tuple(
                ExperimentPlan(
                    experiment_id=item.experiment_id,
                    policy=item.policy,
                    target_comparison_id=item.target_comparison_id,
                    control_comparison_ids=item.control_comparison_ids,
                    correctness_gate=item.correctness_gate,
                    complexity_gate=item.complexity_gate,
                    note_path=item.note_path,
                    note_sha256=item.note_sha256,
                    rationale=item.rationale,
                )
                for item in report.experiments
            ),
        )
        if canonical_json_bytes(report_to_dict(recomputed)) != canonical_json_bytes(
            report_to_dict(report)
        ):
            raise ValueError("report does not regenerate from its source observations")
        markdown_path = report_path.with_suffix(".md")
        svg_path = report_path.with_suffix(".svg")
        try:
            markdown = _read_bounded_file(markdown_path, _MARKDOWN_MAX_BYTES)
            svg = _read_bounded_file(svg_path, _SVG_MAX_BYTES)
        except OSError as exc:
            raise ValueError("report is missing its deterministic Markdown/SVG rendering") from exc
        if markdown != render_report_markdown(report).encode("ascii"):
            raise ValueError("report Markdown does not regenerate byte-exactly")
        if svg != render_report_svg(report).encode("ascii"):
            raise ValueError("report SVG does not regenerate byte-exactly")
        for experiment in report.experiments:
            note = directory / Path(experiment.note_path)
            if (
                note not in files
                or not note.is_file()
                or file_sha256(note) != experiment.note_sha256
            ):
                raise ValueError("experiment note is missing or differs from its report")
    covered_observations = tuple(
        digest for _, report in reports for digest in report.source_observations
    )
    if set(covered_observations) != set(observations) or len(covered_observations) != len(
        set(covered_observations)
    ):
        raise ValueError("reports must cover every observation exactly once")

    referenced_streams = {entry[0].parent / entry[1].stream_file for entry in workloads.values()}
    actual_streams = {path for path in files if path.suffix == ".atlas"}
    if actual_streams != referenced_streams:
        raise ValueError("bundle has missing or unreferenced workload streams")
    referenced_logs: set[Path] = set()
    for manifest_path, manifest in workloads.values():
        if manifest.timed_input_file is None:
            continue
        timed_input = manifest_path.parent / manifest.timed_input_file
        if timed_input not in files or file_sha256(timed_input) != manifest.timed_input_sha256:
            raise ValueError("workload timed input is missing or differs from its manifest")
        _verify_atlslg01(
            timed_input,
            manifest,
            manifest_path.parent / manifest.stream_file,
        )
        referenced_logs.add(timed_input)
    actual_logs = {path for path in files if path.suffix == ".atlslg"}
    if actual_logs != referenced_logs:
        raise ValueError("bundle has missing or unreferenced replay logs")
    expected_svgs = {path.with_suffix(".svg") for path, _ in reports}
    if {path for path in files if path.suffix == ".svg"} != expected_svgs:
        raise ValueError("bundle contains a missing or orphan report SVG")
    expected_report_markdown = {path.with_suffix(".md") for path, _ in reports}
    expected_note_markdown = {
        directory / Path(experiment.note_path)
        for _, report in reports
        for experiment in report.experiments
    }
    if expected_report_markdown & expected_note_markdown:
        raise ValueError("an experiment note cannot replace a report rendering")
    if {path for path in files if path.suffix == ".md"} != (
        expected_report_markdown | expected_note_markdown
    ):
        raise ValueError("bundle contains a missing or orphan Markdown document")
    file_digests = {file_sha256(path) for path in files}
    for environment in environments.values():
        if environment.runtime_kind == "cpython" and environment.harness_sha256 not in file_digests:
            raise ValueError("CPython environment worker script is not inventoried")
    return BundleSummary(
        workloads=len(workloads),
        environments=len(environments),
        observations=len(observations),
        reports=len(reports),
        files=len(files),
    )


def _verify_inventory(directory: Path) -> tuple[Path, ...]:
    inventory_path = directory / "inventory.json"
    try:
        data = _read_bounded_file(inventory_path, _INVENTORY_MAX_BYTES)
        text = data.decode("ascii")
        raw = json.loads(text, object_pairs_hook=_unique_object)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("bundle inventory is missing or invalid") from exc
    if not isinstance(raw, dict) or set(raw) != {"schema", "files"}:
        raise ValueError("bundle inventory has unexpected fields")
    if raw["schema"] != INVENTORY_SCHEMA or not isinstance(raw["files"], list):
        raise ValueError("unsupported bundle inventory")
    expected_records: list[dict[str, str]] = []
    paths: list[Path] = []
    seen_paths: set[str] = set()
    for record in raw["files"]:
        if not isinstance(record, dict) or set(record) != {"path", "sha256"}:
            raise ValueError("bundle inventory record has unexpected fields")
        relative = record["path"]
        digest = record["sha256"]
        if (
            not isinstance(relative, str)
            or not relative
            or "\\" in relative
            or relative.startswith("/")
            or ":" in relative
            or PurePosixPath(relative).as_posix() != relative
            or any(part in {"", ".", ".."} for part in PurePosixPath(relative).parts)
            or relative in seen_paths
        ):
            raise ValueError("bundle inventory path is unsafe or duplicated")
        if (
            not isinstance(digest, str)
            or len(digest) != 64
            or any(character not in "0123456789abcdef" for character in digest)
        ):
            raise ValueError("bundle inventory digest is invalid")
        seen_paths.add(relative)
        path = directory / Path(relative)
        if path.is_symlink() or not path.is_file() or file_sha256(path) != digest:
            raise ValueError("bundle inventory file digest differs")
        expected_records.append({"path": relative, "sha256": digest})
        paths.append(path)
    canonical = canonical_json_bytes({"schema": INVENTORY_SCHEMA, "files": expected_records})
    if data != canonical:
        raise ValueError("bundle inventory is not canonical")
    if tuple(paths) != _bundle_files(directory):
        raise ValueError("bundle inventory does not cover the exact file set")
    return tuple(paths)


def _bundle_files(directory: Path) -> tuple[Path, ...]:
    files = tuple(path for path in _walk_bundle(directory) if path != directory / "inventory.json")
    return tuple(sorted(files, key=lambda path: path.relative_to(directory).as_posix()))


def _read_bounded_file(path: Path, maximum: int) -> bytes:
    try:
        size = path.stat().st_size
        if size < 0 or size > maximum:
            raise ValueError("bundle document exceeds its bound")
        with path.open("rb") as source:
            data = source.read(maximum + 1)
    except OSError as exc:
        raise ValueError("bundle document is not readable") from exc
    if len(data) != size or len(data) > maximum:
        raise ValueError("bundle document changed or exceeds its bound")
    return data


def _reject_symlinks(directory: Path) -> None:
    tuple(_walk_bundle(directory))


def _walk_bundle(directory: Path) -> tuple[Path, ...]:
    if _is_link_or_reparse(directory):
        raise ValueError("bundle root must not be a symbolic link or junction")
    files: list[Path] = []
    pending = [directory]
    while pending:
        current = pending.pop()
        try:
            with os.scandir(current) as iterator:
                entries = tuple(iterator)
        except OSError as exc:
            raise ValueError("bundle directory is not readable") from exc
        for entry in entries:
            path = Path(entry.path)
            if _is_link_or_reparse(path):
                raise ValueError("bundle must not contain symbolic links or junctions")
            if entry.is_dir(follow_symlinks=False):
                pending.append(path)
            elif entry.is_file(follow_symlinks=False):
                files.append(path)
    return tuple(files)


def _is_link_or_reparse(path: Path) -> bool:
    junction = getattr(path, "is_junction", None)
    if path.is_symlink() or (callable(junction) and bool(junction())):
        return True
    try:
        attributes = getattr(path.lstat(), "st_file_attributes", 0)
    except OSError as exc:
        raise ValueError("bundle entry cannot be inspected") from exc
    return bool(attributes & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0))


def _verify_atlslg01(
    path: Path,
    manifest: WorkloadManifest,
    workload_path: Path,
) -> None:
    """Validate the bounded V1 log structure used as replay timing input."""

    expected_commands = _iter_v2_commands(workload_path, manifest)
    try:
        _verify_atlslg01_records(path, manifest, expected_commands)
    finally:
        expected_commands.close()


def _verify_atlslg01_records(
    path: Path,
    manifest: WorkloadManifest,
    expected_commands: Generator[tuple[int | str, ...], None, None],
) -> None:
    try:
        with path.open("rb") as stream:
            prefix = stream.read(60)
            if len(prefix) != 60 or prefix[:8] != b"ATLSLG01":
                raise ValueError("replay input lacks a complete ATLSLG01 header")
            format_version = int.from_bytes(prefix[8:10], "big")
            semantic_version = int.from_bytes(prefix[10:12], "big")
            byte_order = int.from_bytes(prefix[12:16], "big")
            header_length = int.from_bytes(prefix[16:20], "big")
            catalog_length = int.from_bytes(prefix[20:24], "big")
            first_sequence = int.from_bytes(prefix[40:48], "big")
            max_total_active_orders = prefix[48:56]
            catalog_count = int.from_bytes(prefix[56:60], "big")
            if (
                format_version != 1
                or semantic_version != 6
                or byte_order != 0x01020304
                or first_sequence != 1
                or catalog_count == 0
                or header_length > 1024 * 1024
                or catalog_length != 28 * catalog_count
                or header_length != 96 + catalog_length
                or int.from_bytes(max_total_active_orders, "big")
                != manifest.max_total_active_orders
                or catalog_count != len(manifest.catalog)
                or prefix[24:40] != bytes.fromhex(manifest.stream_sha256)[:16]
            ):
                raise ValueError("replay input has an invalid ATLSLG01 header schema")
            suffix = stream.read(header_length - len(prefix))
            if len(suffix) != header_length - len(prefix):
                raise ValueError("replay input has a truncated ATLSLG01 header")
            header = prefix + suffix
            if _crc32c(header[:-4]) != int.from_bytes(header[-4:], "big"):
                raise ValueError("replay input ATLSLG01 header checksum differs")
            catalog = header[60 : 60 + catalog_length]
            prior_instrument = 0
            for index, offset in enumerate(range(0, len(catalog), 28)):
                entry = catalog[offset : offset + 28]
                instrument = int.from_bytes(entry[:4], "big")
                maximum_quantity = int.from_bytes(entry[4:12], "big")
                tick = int.from_bytes(entry[12:20], "big", signed=True)
                if instrument <= prior_instrument or maximum_quantity == 0 or tick <= 0:
                    raise ValueError("replay input has an invalid ATLSLG01 catalog")
                expected = manifest.catalog[index]
                if (
                    instrument != expected.instrument_id
                    or maximum_quantity != expected.max_order_quantity
                    or tick != expected.tick_increment
                    or int.from_bytes(entry[20:28], "big") != expected.max_active_orders
                ):
                    raise ValueError("replay input catalog differs from its workload")
                prior_instrument = instrument
            configuration_input = (
                b"ATLSCF01"
                + prefix[10:12]
                + max_total_active_orders
                + catalog_count.to_bytes(8, "big")
                + catalog
            )
            stored_configuration = header[60 + catalog_length : 92 + catalog_length]
            if hashlib.sha256(configuration_input).digest() != stored_configuration:
                raise ValueError("replay input configuration digest differs")
            records = 0
            events = 0
            committed = 0
            rejected = 0
            event_digest = hashlib.sha256()
            expected_sequence = 1
            while True:
                total_prefix = stream.read(4)
                if not total_prefix:
                    break
                if len(total_prefix) != 4:
                    raise ValueError("replay input has a torn record length")
                total_length = int.from_bytes(total_prefix, "big")
                if not 66 <= total_length <= 64 * 1024:
                    raise ValueError("replay input has an invalid record length")
                suffix = stream.read(total_length - 4)
                if len(suffix) != total_length - 4:
                    raise ValueError("replay input has a torn record")
                record = total_prefix + suffix
                payload_length = int.from_bytes(record[4:8], "big")
                record_version = int.from_bytes(record[8:10], "big")
                command_type = record[10]
                outcome = record[11]
                sequence = int.from_bytes(record[12:20], "big")
                rejection_reason = int.from_bytes(record[20:22], "big")
                event_count = int.from_bytes(record[22:30], "big")
                expected_payload = {1: 36, 2: 16, 3: 40}.get(command_type)
                if (
                    total_length != 66 + payload_length
                    or expected_payload != payload_length
                    or record_version != 1
                    or event_count == 0
                    or sequence != expected_sequence
                    or outcome not in {1, 2}
                    or (outcome == 1) != (rejection_reason == 0)
                    or _crc32c(record[:-4]) != int.from_bytes(record[-4:], "big")
                ):
                    raise ValueError("replay input has an invalid ATLSLG01 record")
                if command_type == 1:
                    price_presence = record[81]
                    price_slot = record[82:90]
                    if price_presence not in {0, 1} or (price_presence == 0 and any(price_slot)):
                        raise ValueError("replay input has an invalid new-order payload")
                decoded_command = _decode_log_command(command_type, record[62:-4])
                try:
                    expected_command = next(expected_commands)
                except StopIteration as exc:
                    raise ValueError("replay log contains more commands than its workload") from exc
                if decoded_command != expected_command:
                    raise ValueError("replay input command differs from its frozen workload stream")
                events += event_count
                committed += int(outcome == 1)
                rejected += int(outcome == 2)
                event_digest.update(record[30:62])
                expected_sequence += 1
                records += 1
    except OSError as exc:
        raise ValueError("replay input cannot be read") from exc
    try:
        next(expected_commands)
    except StopIteration:
        pass
    else:
        raise ValueError("replay log contains fewer commands than its workload")
    if records != manifest.timed_input_records:
        raise ValueError("replay input record count differs from its workload manifest")
    if (
        events != manifest.expected_events
        or committed != manifest.expected_committed
        or rejected != manifest.expected_rejected
        or manifest.expected_engine_errors != 0
        or event_digest.hexdigest() != manifest.expected_event_digest
    ):
        raise ValueError("replay input outcomes differ from its workload manifest")


def _iter_v2_commands(
    path: Path,
    manifest: WorkloadManifest,
) -> Generator[tuple[int | str, ...], None, None]:
    try:
        with path.open("rb") as source:
            header = _bounded_ascii_fields(source, "workload header")
            if len(header) != 5 or header[0] != "ATLAS_DIFF_V2":
                raise ValueError("replay workload stream has an invalid V2 header")
            maximum_active, catalog_count, command_count, checkpoint_interval = (
                _canonical_integer(value) for value in header[1:]
            )
            if (
                maximum_active != manifest.max_total_active_orders
                or catalog_count != len(manifest.catalog)
                or command_count != manifest.command_count
                or checkpoint_interval != 0
            ):
                raise ValueError("replay workload header differs from its manifest")
            for expected in manifest.catalog:
                fields = _bounded_ascii_fields(source, "instrument record")
                if (
                    len(fields) != 5
                    or fields[0] != "I"
                    or tuple(_canonical_integer(value) for value in fields[1:])
                    != (
                        expected.instrument_id,
                        expected.max_order_quantity,
                        expected.tick_increment,
                        expected.max_active_orders,
                    )
                ):
                    raise ValueError("replay workload catalog differs from its manifest")
            expected_fields = {"N": 10, "C": 4, "R": 7}
            for _ in range(command_count):
                fields = _bounded_ascii_fields(source, "command record")
                if fields[0] not in expected_fields or len(fields) != expected_fields[fields[0]]:
                    raise ValueError("replay workload command has an invalid shape")
                yield (
                    fields[0],
                    *(_canonical_integer(value) for value in fields[1:]),
                )
            if source.read(1):
                raise ValueError("replay workload stream has trailing records")
    except OSError as exc:
        raise ValueError("replay workload stream cannot be read") from exc


def _bounded_ascii_fields(source: BinaryIO, name: str) -> list[str]:
    raw = source.readline(1025)
    if (
        not isinstance(raw, bytes)
        or not raw
        or len(raw) > 1024
        or not raw.endswith(b"\n")
        or b"\r" in raw
    ):
        raise ValueError(f"{name} is missing, oversized, or not canonical LF text")
    try:
        text = raw[:-1].decode("ascii")
    except UnicodeDecodeError as exc:
        raise ValueError(f"{name} is not ASCII") from exc
    if not text or "  " in text or text.startswith(" ") or text.endswith(" "):
        raise ValueError(f"{name} is not canonically spaced")
    return text.split(" ")


def _canonical_integer(value: str) -> int:
    if re.fullmatch(r"(?:0|-[1-9][0-9]*|[1-9][0-9]*)", value) is None:
        raise ValueError("workload integer is not canonical")
    return int(value)


def _decode_log_command(
    command_type: int,
    payload: bytes,
) -> tuple[int | str, ...]:
    if command_type == 1:
        return (
            "N",
            int.from_bytes(payload[0:4], "big"),
            int.from_bytes(payload[4:12], "big"),
            int.from_bytes(payload[12:16], "big"),
            payload[16],
            payload[17],
            payload[18],
            payload[19],
            int.from_bytes(payload[20:28], "big", signed=True),
            int.from_bytes(payload[28:36], "big"),
        )
    if command_type == 2:
        return (
            "C",
            int.from_bytes(payload[0:4], "big"),
            int.from_bytes(payload[4:12], "big"),
            int.from_bytes(payload[12:16], "big"),
        )
    if command_type == 3:
        return (
            "R",
            int.from_bytes(payload[0:4], "big"),
            int.from_bytes(payload[4:12], "big"),
            int.from_bytes(payload[12:20], "big"),
            int.from_bytes(payload[20:24], "big"),
            int.from_bytes(payload[24:32], "big", signed=True),
            int.from_bytes(payload[32:40], "big"),
        )
    raise ValueError("replay input command type is unsupported")


def _crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def _unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate inventory key: {key}")
        result[key] = value
    return result
