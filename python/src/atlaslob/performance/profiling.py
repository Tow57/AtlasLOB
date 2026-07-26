"""Linux perf capture kept separate from benchmark timing evidence."""

from __future__ import annotations

import re
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Final, Literal

from atlaslob.performance.schemas import observation_from_dict, read_canonical_document
from atlaslob.performance.suite import Runner, _prepare_runner, _run_bounded, run_suite

ProfileKind = Literal["stat", "record"]

PERF_COUNTERS: Final = (
    "cycles",
    "instructions",
    "branches",
    "branch-misses",
    "cache-references",
    "cache-misses",
    "page-faults",
    "context-switches",
)
_PRIVATE_UNIX_PATH: Final = re.compile(r"/(?:home|Users)/[^/\s]+(?:/[^\s]*)?")
_PRIVATE_WINDOWS_PATH: Final = re.compile(r"[A-Za-z]:[\\/][^\s]*")
_MAX_PERF_TEXT_BYTES: Final = 16 * 1024 * 1024


@dataclass(frozen=True, slots=True)
class ProfileSummary:
    kind: ProfileKind
    captures: int
    observations: tuple[Path, ...]


def capture_profile(
    manifest_path: Path,
    output_directory: Path,
    *,
    runner: Runner,
    perf_executable: Path,
    suite_label: str,
    kind: ProfileKind,
    observations: int,
    timeout_seconds: int = 900,
) -> ProfileSummary:
    """Capture fixed perf evidence while validating the runner result."""

    if kind not in {"stat", "record"}:
        raise ValueError("profile kind must be stat or record")
    if isinstance(observations, bool) or not 1 <= observations <= 10:
        raise ValueError("profile observations must be in [1, 10]")
    if kind == "record" and observations != 1:
        raise ValueError("perf record requires exactly one observation")
    if not perf_executable.is_file():
        raise ValueError("perf executable does not exist")
    if runner.variant != "standalone" or runner.wheel is not None or runner.worker is not None:
        raise ValueError("profiling requires a standalone native runner")
    _prepare_runner(runner, "throughput")
    if output_directory.exists() and any(output_directory.iterdir()):
        raise ValueError("profile output directory must be empty")
    output_directory.mkdir(parents=True, exist_ok=True)

    paths: list[Path] = []
    counter_captures: list[dict[str, Decimal | None]] = []
    for attempt in range(1, observations + 1):
        attempt_directory = output_directory / f"attempt-{attempt:05d}"
        artifact = (
            attempt_directory / "perf-stat.txt"
            if kind == "stat"
            else attempt_directory / "perf.data"
        )
        prefix = perf_prefix(
            perf_executable,
            kind=kind,
            output=artifact,
        )
        profiled_runner = Runner(
            runner.executable,
            runner.environment_path,
            runner.variant,
            command_prefix=prefix,
        )
        result_paths = run_suite(
            manifest_path,
            attempt_directory,
            baseline=profiled_runner,
            suite_label=suite_label,
            mode="throughput",
            observations=1,
            block_start=attempt,
            timeout_seconds=timeout_seconds,
        )
        observation_path = result_paths[0]
        observation = read_canonical_document(observation_path, observation_from_dict)
        paths.append(observation_path)
        if not observation.valid:
            raise ValueError(
                f"perf {kind} retained an invalid runner observation: {observation.failure_reason}"
            )
        if not artifact.is_file():
            raise ValueError(f"perf {kind} did not publish its output")
        if kind == "stat":
            counter_captures.append(_validate_perf_stat(artifact))
        else:
            _render_perf_report(
                perf_executable,
                artifact,
                attempt_directory / "perf-report.txt",
                timeout_seconds,
            )
    if kind == "stat":
        _write_counter_summary(
            output_directory / "perf-counter-summary.txt",
            tuple(counter_captures),
        )
    return ProfileSummary(kind, observations, tuple(paths))


def perf_prefix(
    perf_executable: Path,
    *,
    kind: ProfileKind,
    output: Path,
) -> tuple[str, ...]:
    """Return the frozen command prefix for one runner process."""

    if kind == "stat":
        return (
            str(perf_executable),
            "stat",
            "--no-big-num",
            "-x",
            ";",
            "-e",
            ",".join(PERF_COUNTERS),
            "-o",
            str(output),
            "--",
        )
    if kind == "record":
        return (
            str(perf_executable),
            "record",
            "--quiet",
            "-g",
            "--call-graph",
            "dwarf",
            "-o",
            str(output),
            "--",
        )
    raise ValueError("profile kind must be stat or record")


def _validate_perf_stat(path: Path) -> dict[str, Decimal | None]:
    text = _read_public_text(path)
    values: dict[str, Decimal | None] = {}
    for line in text.splitlines():
        fields = tuple(field.strip() for field in line.split(";"))
        matching = tuple(counter for counter in PERF_COUNTERS if counter in fields)
        if not matching:
            continue
        if len(matching) != 1 or matching[0] in values:
            raise ValueError("perf stat output contains duplicate or ambiguous counters")
        raw_value = fields[0]
        if raw_value.startswith("<") and raw_value.endswith(">"):
            values[matching[0]] = None
            continue
        try:
            value = Decimal(raw_value)
        except InvalidOperation as exc:
            raise ValueError("perf stat output contains a malformed counter value") from exc
        if not value.is_finite() or value < 0:
            raise ValueError("perf stat output contains an invalid counter value")
        values[matching[0]] = value
    for counter in PERF_COUNTERS:
        if counter not in values:
            raise ValueError(f"perf stat output omits {counter}")
    path.write_text(text, encoding="utf-8", newline="\n")
    return values


def _write_counter_summary(
    path: Path,
    captures: tuple[dict[str, Decimal | None], ...],
) -> None:
    lines = [
        "AtlasLOB perf counter summary",
        f"captures={len(captures)}",
        "counter;available;unavailable;minimum;median;maximum",
    ]
    limitations: list[str] = []
    for counter in PERF_COUNTERS:
        numeric_values: list[Decimal] = []
        for capture in captures:
            value = capture[counter]
            if value is not None:
                numeric_values.append(value)
        values = sorted(numeric_values)
        available = len(values)
        unavailable = len(captures) - available
        if values:
            middle = available // 2
            median = values[middle] if available % 2 else (values[middle - 1] + values[middle]) / 2
            minimum_text = _decimal_text(values[0])
            median_text = _decimal_text(median)
            maximum_text = _decimal_text(values[-1])
        else:
            minimum_text = median_text = maximum_text = "-"
        lines.append(
            f"{counter};{available};{unavailable};{minimum_text};{median_text};{maximum_text}"
        )
        if unavailable:
            limitations.append(
                f"limitation={counter} unavailable in {unavailable} of {len(captures)} captures"
            )
    lines.extend(limitations)
    path.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")


def _decimal_text(value: Decimal) -> str:
    text = format(value, "f")
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return text or "0"


def _render_perf_report(
    perf_executable: Path,
    data_path: Path,
    report_path: Path,
    timeout_seconds: int,
) -> None:
    completed, failure = _run_bounded(
        [
            str(perf_executable),
            "report",
            "--stdio",
            "--no-children",
            "--percent-limit",
            "0.1",
            "--input",
            str(data_path),
        ],
        timeout_seconds,
    )
    if completed is None:
        raise ValueError(f"perf report failed: {failure}")
    if completed.returncode != 0 or completed.stderr:
        raise ValueError("perf report process failed")
    try:
        text = completed.stdout.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise ValueError("perf report is not UTF-8 text") from exc
    sanitized = _sanitize_public_text(text)
    report_path.write_text(sanitized, encoding="utf-8", newline="\n")


def _read_public_text(path: Path) -> str:
    try:
        size = path.stat().st_size
        if size <= 0 or size > _MAX_PERF_TEXT_BYTES:
            raise ValueError("perf text output is empty or exceeds 16 MiB")
        data = path.read_bytes()
    except OSError as exc:
        raise ValueError("perf text output is unreadable") from exc
    try:
        text = data.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise ValueError("perf text output is not UTF-8") from exc
    return _sanitize_public_text(text)


def _sanitize_public_text(text: str) -> str:
    normalized = text.replace("\r\n", "\n").replace("\r", "\n")
    normalized = _PRIVATE_UNIX_PATH.sub("<private-path>", normalized)
    normalized = _PRIVATE_WINDOWS_PATH.sub("<private-path>", normalized)
    if "\x00" in normalized:
        raise ValueError("perf text output contains NUL")
    if normalized and not normalized.endswith("\n"):
        normalized += "\n"
    return normalized
