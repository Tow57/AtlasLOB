"""Standalone target-wheel worker for AtlasLOB Python batch measurements.

This file intentionally imports only the standard library and the installed
``atlaslob`` distribution.  The source-only performance package is not present
in production wheels and is never imported here.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib
import importlib.metadata
import json
import os
import platform
import re
import sys
import time
import zipfile
from collections.abc import Iterable, Sequence
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, BinaryIO, Protocol, TypeVar

if TYPE_CHECKING:
    from atlaslob.domain import Command, InstrumentConfig
    from atlaslob.engine import Engine

_IDENTITY_SCHEMA = "ATLAS_PYTHON_TARGET_IDENTITY_V1"
_OBSERVATION_SCHEMA = "ATLAS_BENCH_OBSERVATION_V1"
_DIGEST = re.compile(r"[0-9a-f]{64}\Z")
_IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\Z")
_U64_MAX = (1 << 64) - 1
_MAX_CATALOG_ENTRIES = 4_096
_MAX_COMMANDS = 100_000_000
T = TypeVar("T")


class _Digest(Protocol):
    def update(self, data: bytes) -> None: ...


def _remove_source_shadowing() -> None:
    blocked: set[Path] = set()
    for entry in os.environ.pop("PYTHONPATH", "").split(os.pathsep):
        if entry:
            blocked.add(Path(entry).resolve())
    filtered: list[str] = []
    for entry in sys.path:
        if not entry:
            continue
        try:
            resolved = Path(entry).resolve()
        except OSError:
            continue
        if resolved not in blocked:
            filtered.append(entry)
    sys.path[:] = filtered


_remove_source_shadowing()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _canonical_json(value: dict[str, object]) -> bytes:
    return (
        json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode("ascii")


def _package_digest(distribution: importlib.metadata.Distribution) -> str:
    records: list[tuple[str, str]] = []
    for relative in distribution.files or ():
        name = relative.as_posix()
        if name.endswith((".pyc", ".pyo")) or "/__pycache__/" in f"/{name}":
            continue
        path = Path(str(distribution.locate_file(relative)))
        if path.is_file():
            records.append((name, _sha256(path)))
    payload = _canonical_json(
        {
            "schema": "ATLAS_INSTALLED_PACKAGE_V1",
            "files": [{"path": name, "sha256": digest} for name, digest in sorted(records)],
        }
    )
    return hashlib.sha256(payload).hexdigest()


def _identity(wheel: Path, include_private_path: bool) -> dict[str, object]:
    if not wheel.is_file():
        raise ValueError("wheel does not exist")
    distribution = importlib.metadata.distribution("atlaslob")
    package = importlib.import_module("atlaslob")
    wrapper = importlib.import_module("atlaslob.engine")
    extension = importlib.import_module("atlaslob._native_engine")
    package_file = Path(str(package.__file__)).resolve()
    expected_package = Path(str(distribution.locate_file("atlaslob/__init__.py"))).resolve()
    if package_file != expected_package:
        raise ValueError("atlaslob import is shadowed by a non-wheel package")
    wrapper_path = Path(str(wrapper.__file__)).resolve()
    extension_path = Path(str(extension.__file__)).resolve()
    wheel_version = _verify_wheel_install(
        wheel,
        distribution,
        wrapper_path,
        extension_path,
    )
    if wheel_version != distribution.version:
        raise ValueError("installed package version differs from supplied wheel")
    value: dict[str, object] = {
        "schema": _IDENTITY_SCHEMA,
        "python_implementation": platform.python_implementation(),
        "python_version": platform.python_version(),
        "python_cache_tag": sys.implementation.cache_tag,
        "interpreter_sha256": _sha256(Path(sys.executable)),
        "wheel_sha256": _sha256(wheel),
        "package_sha256": _package_digest(distribution),
        "wrapper_sha256": _sha256(wrapper_path),
        "extension_sha256": _sha256(extension_path),
        "package_version": distribution.version,
    }
    if include_private_path:
        value["extension_path"] = str(extension_path)
    return value


def _verify_wheel_install(
    wheel: Path,
    distribution: importlib.metadata.Distribution,
    wrapper_path: Path,
    extension_path: Path,
) -> str:
    try:
        with zipfile.ZipFile(wheel) as archive:
            names = tuple(archive.namelist())
            if len(names) != len(set(names)):
                raise ValueError("wheel contains duplicate members")
            for name in names:
                pure = PurePosixPath(name)
                if (
                    not name
                    or "\\" in name
                    or name.startswith("/")
                    or any(part in {"", ".", ".."} for part in pure.parts)
                ):
                    raise ValueError("wheel contains an unsafe member")
            package_members = tuple(
                name for name in names if name.startswith("atlaslob/") and not name.endswith("/")
            )
            if not package_members:
                raise ValueError("wheel contains no atlaslob package")
            installed_members = {
                relative.as_posix()
                for relative in distribution.files or ()
                if relative.as_posix().startswith("atlaslob/")
                and not relative.as_posix().endswith((".pyc", ".pyo"))
                and "/__pycache__/" not in f"/{relative.as_posix()}"
            }
            if set(package_members) != installed_members:
                raise ValueError("installed atlaslob package file set differs from supplied wheel")
            for name in package_members:
                installed = Path(str(distribution.locate_file(name)))
                if not installed.is_file():
                    raise ValueError("installed package omits a supplied wheel member")
                wheel_hash = hashlib.sha256(archive.read(name)).hexdigest()
                if _sha256(installed) != wheel_hash:
                    raise ValueError("installed package differs from supplied wheel bytes")
            metadata_names = tuple(name for name in names if name.endswith(".dist-info/METADATA"))
            if len(metadata_names) != 1:
                raise ValueError("wheel must contain exactly one METADATA member")
            metadata = archive.read(metadata_names[0]).decode("utf-8")
            wrapper_member = "atlaslob/engine.py"
            extension_members = tuple(
                name
                for name in package_members
                if PurePosixPath(name).name.startswith("_native_engine.")
            )
            if wrapper_member not in package_members or len(extension_members) != 1:
                raise ValueError("wheel omits the Python wrapper or native extension")
            wheel_wrapper_digest = hashlib.sha256(archive.read(wrapper_member)).hexdigest()
            wheel_extension_digest = hashlib.sha256(archive.read(extension_members[0])).hexdigest()
    except (OSError, UnicodeDecodeError, zipfile.BadZipFile, KeyError) as exc:
        raise ValueError("wheel cannot be verified") from exc
    name_value: str | None = None
    version_value: str | None = None
    for line in metadata.splitlines():
        if line.startswith("Name: ") and name_value is None:
            name_value = line.removeprefix("Name: ").strip()
        elif line.startswith("Version: ") and version_value is None:
            version_value = line.removeprefix("Version: ").strip()
    if name_value != "atlaslob" or not version_value:
        raise ValueError("wheel metadata does not identify atlaslob")
    if wheel_wrapper_digest != _sha256(wrapper_path) or wheel_extension_digest != _sha256(
        extension_path
    ):
        raise ValueError("imported wrapper/extension differs from supplied wheel")
    return version_value


def _unsigned(value: str, name: str, maximum: int = _U64_MAX) -> int:
    if re.fullmatch(r"(?:0|[1-9][0-9]*)", value) is None:
        raise ValueError(f"{name} is not canonical unsigned decimal")
    parsed = int(value)
    if parsed > maximum:
        raise ValueError(f"{name} exceeds its representation")
    return parsed


def _signed(value: str, name: str) -> int:
    if re.fullmatch(r"(?:0|-[1-9][0-9]*|[1-9][0-9]*)", value) is None:
        raise ValueError(f"{name} is not canonical signed decimal")
    parsed = int(value)
    if not -(1 << 63) <= parsed <= (1 << 63) - 1:
        raise ValueError(f"{name} exceeds i64")
    return parsed


def _parse_workload(
    path: Path,
    expected_sha256: str,
) -> tuple[tuple[int, tuple[InstrumentConfig, ...]], tuple[Command, ...]]:
    from atlaslob.domain import (
        CancelOrder,
        InstrumentConfig,
        MatchingConfig,
        NewOrder,
        ReplaceOrder,
    )

    digest = hashlib.sha256()
    try:
        source = path.open("rb")
    except OSError as exc:
        raise ValueError("workload cannot be opened") from exc
    with source:
        header = _workload_fields(source, digest, "workload header")
        if len(header) != 5 or header[0] != "ATLAS_DIFF_V2":
            raise ValueError("workload header differs")
        maximum_active = _unsigned(header[1], "max_total_active_orders")
        catalog_count = _unsigned(header[2], "catalog_count")
        command_count = _unsigned(header[3], "command_count")
        checkpoint_interval = _unsigned(header[4], "checkpoint_interval")
        if not 1 <= catalog_count <= _MAX_CATALOG_ENTRIES:
            raise ValueError("catalog count exceeds the V1 bound")
        if not 1 <= command_count <= _MAX_COMMANDS:
            raise ValueError("command count exceeds the V1 bound")
        if checkpoint_interval != 0:
            raise ValueError("benchmark workload checkpoint interval must be zero")
        catalog: list[InstrumentConfig] = []
        prior_instrument = 0
        for _ in range(catalog_count):
            fields = _workload_fields(source, digest, "instrument record")
            if len(fields) != 5 or fields[0] != "I":
                raise ValueError("instrument record differs")
            instrument_id = _unsigned(fields[1], "instrument_id", (1 << 32) - 1)
            if instrument_id <= prior_instrument:
                raise ValueError("catalog is not strictly ordered")
            prior_instrument = instrument_id
            catalog.append(
                InstrumentConfig(
                    instrument_id,
                    MatchingConfig(
                        max_order_quantity=_unsigned(fields[2], "max_order_quantity"),
                        tick_increment=_signed(fields[3], "tick_increment"),
                        max_active_orders=_unsigned(fields[4], "max_active_orders"),
                    ),
                )
            )
        commands: list[Command] = []
        for _ in range(command_count):
            fields = _workload_fields(source, digest, "command record")
            if fields[0] == "N" and len(fields) == 10:
                presence = _unsigned(fields[7], "price_presence", 1)
                price = _signed(fields[8], "price")
                if presence == 0 and price != 0:
                    raise ValueError("absent price placeholder is nonzero")
                commands.append(
                    NewOrder(
                        _unsigned(fields[1], "client_id", (1 << 32) - 1),
                        _unsigned(fields[2], "order_id"),
                        _unsigned(fields[3], "instrument_id", (1 << 32) - 1),
                        _unsigned(fields[4], "side", 255),
                        _unsigned(fields[5], "order_type", 255),
                        _unsigned(fields[6], "time_in_force", 255),
                        price if presence else None,
                        _unsigned(fields[9], "quantity"),
                    )
                )
            elif fields[0] == "C" and len(fields) == 4:
                commands.append(
                    CancelOrder(
                        _unsigned(fields[1], "client_id", (1 << 32) - 1),
                        _unsigned(fields[2], "order_id"),
                        _unsigned(fields[3], "instrument_id", (1 << 32) - 1),
                    )
                )
            elif fields[0] == "R" and len(fields) == 7:
                commands.append(
                    ReplaceOrder(
                        _unsigned(fields[1], "client_id", (1 << 32) - 1),
                        _unsigned(fields[2], "old_order_id"),
                        _unsigned(fields[3], "new_order_id"),
                        _unsigned(fields[4], "instrument_id", (1 << 32) - 1),
                        _signed(fields[5], "new_limit_price"),
                        _unsigned(fields[6], "new_quantity"),
                    )
                )
            else:
                raise ValueError("command record differs")
        if source.read(1):
            raise ValueError("workload contains trailing records")
    if digest.hexdigest() != expected_sha256:
        raise ValueError("workload digest differs")
    return (maximum_active, tuple(catalog)), tuple(commands)


def _workload_fields(
    source: BinaryIO,
    digest: _Digest,
    name: str,
) -> list[str]:
    raw = source.readline(1025)
    if (
        not isinstance(raw, bytes)
        or not raw
        or len(raw) > 1024
        or not raw.endswith(b"\n")
        or b"\r" in raw
    ):
        raise ValueError(f"{name} is missing, oversized, or not canonical LF text")
    digest.update(raw)
    try:
        text = raw[:-1].decode("ascii")
    except UnicodeDecodeError as exc:
        raise ValueError(f"{name} is not ASCII") from exc
    if not text or "  " in text or text.startswith(" ") or text.endswith(" "):
        raise ValueError(f"{name} is not canonically spaced")
    return text.split(" ")


def _rss_bytes() -> int:
    try:
        for line in Path("/proc/self/status").read_text(encoding="ascii").splitlines():
            if line.startswith("VmRSS:"):
                fields = line.split()
                if len(fields) == 3 and fields[2] == "kB":
                    return int(fields[1]) * 1024
    except OSError:
        pass
    return 0


def _peak_rss_bytes() -> int:
    try:
        import resource

        usage = resource.getrusage(resource.RUSAGE_SELF)  # type: ignore[attr-defined]
        return int(usage.ru_maxrss) * 1024
    except (ImportError, OSError, ValueError):
        return 0


def _chunks(values: Sequence[T], size: int) -> Iterable[Sequence[T]]:
    for begin in range(0, len(values), size):
        yield values[begin : begin + size]


def _submit_prefix(engine: Engine, commands: Sequence[Command], batch_size: int) -> None:
    for batch in _chunks(commands, batch_size):
        result = engine.submit_batch(batch, output="summary")
        if result.terminal_error is not None or result.processed_count != len(batch):
            raise ValueError("prefix execution stopped with an engine error")


def _validate_results(
    config: tuple[int, tuple[InstrumentConfig, ...]],
    commands: tuple[Command, ...],
    measured_begin: int,
    batch_size: int,
) -> tuple[int, int, int, int, int, str, str]:
    from atlaslob import Engine
    from atlaslob.canonical import event_digest
    from atlaslob.domain import EventBatch
    from atlaslob.engine import ObjectBatch

    maximum_active, catalog = config
    engine = Engine(catalog, max_total_active_orders=maximum_active)
    _submit_prefix(engine, commands[:measured_begin], batch_size)
    committed = rejected = engine_errors = events = processed = 0
    event_hash = hashlib.sha256()
    for batch in _chunks(commands[measured_begin:], batch_size):
        result = engine.submit_batch(batch, output="objects")
        if not isinstance(result.payload, ObjectBatch):
            raise RuntimeError("object batch returned the wrong payload type")
        committed += result.committed_count
        rejected += result.rejected_count
        processed += result.processed_count
        for command_result in result.payload.results:
            if command_result.events is None:
                continue
            events += len(command_result.events)
            event_hash.update(bytes.fromhex(event_digest(EventBatch(command_result.events))))
        if result.terminal_error is not None:
            engine_errors += 1
            break
    return (
        processed + engine_errors,
        events,
        committed,
        rejected,
        engine_errors,
        event_hash.hexdigest(),
        engine.state_digest(),
    )


def _measurement_parameters(options: argparse.Namespace) -> dict[str, str]:
    return {
        "batch_size": str(options.batch_size),
        "instrument_count": options.instrument_count,
        "measured_start_active_order_count": options.measured_start_active_order_count,
        "output_mode": options.output_mode,
        "sweep_depth": options.sweep_depth,
    }


def _observation(
    options: argparse.Namespace,
    *,
    elapsed_ns: int,
    rss_before: int,
    rss_after: int,
    peak_rss: int,
    evidence: tuple[int, int, int, int, int, str, str],
    valid: bool,
    failure_reason: str | None,
) -> dict[str, object]:
    commands, events, committed, rejected, engine_errors, event_digest, final_digest = evidence
    return {
        "schema": _OBSERVATION_SCHEMA,
        "boundary": f"python_{options.output_mode}",
        "timed_input_kind": "none",
        "timed_input_sha256": None,
        "measurement_parameters": _measurement_parameters(options),
        "workload_id": options.workload_id,
        "workload_sha256": options.workload_sha256,
        "workload_manifest_sha256": options.workload_manifest_sha256,
        "binary_sha256": options.binary_sha256,
        "environment_sha256": options.environment_sha256,
        "host_context_sha256": options.host_context_sha256,
        "suite_label": options.suite_label,
        "run_label": options.run_label,
        "variant": options.variant,
        "block_index": str(options.block_index),
        "block_position": str(options.block_position),
        "preload_commands": str(options.preload_count),
        "warmup_commands": str(options.warmup_count),
        "commands": str(commands),
        "events": str(events),
        "committed": str(committed),
        "rejected": str(rejected),
        "engine_errors": str(engine_errors),
        "elapsed_ns": str(elapsed_ns),
        "rss_before_bytes": str(rss_before),
        "rss_after_bytes": str(rss_after),
        "peak_rss_bytes": str(peak_rss),
        "latency_ns": None,
        "allocations": None,
        "event_digest": event_digest,
        "final_digest": final_digest,
        "valid": valid,
        "failure_reason": failure_reason,
    }


def _run_observation(options: argparse.Namespace) -> dict[str, object]:
    from atlaslob import Engine

    for name in (
        "preload_count",
        "warmup_count",
        "measured_count",
    ):
        value = getattr(options, name)
        if not 0 <= value <= _MAX_COMMANDS:
            raise ValueError("command region exceeds the V1 bound")
    if options.measured_count == 0:
        raise ValueError("measured region must not be empty")
    instrument_count = _unsigned(options.instrument_count, "instrument_count", _MAX_CATALOG_ENTRIES)
    if instrument_count == 0:
        raise ValueError("instrument_count must be nonzero")
    _unsigned(
        options.measured_start_active_order_count,
        "measured_start_active_order_count",
    )
    _unsigned(options.sweep_depth, "sweep_depth", 64)
    config, commands = _parse_workload(options.workload, options.workload_sha256)
    expected_total = options.preload_count + options.warmup_count + options.measured_count
    if expected_total > _MAX_COMMANDS or len(commands) != expected_total:
        raise ValueError("region counts do not cover the workload")
    maximum_active, catalog = config
    if instrument_count != len(catalog):
        raise ValueError("instrument_count differs from the workload catalog")
    engine = Engine(catalog, max_total_active_orders=maximum_active)
    measured_begin = options.preload_count + options.warmup_count
    _submit_prefix(engine, commands[:measured_begin], options.batch_size)
    measured_batches = tuple(
        tuple(batch) for batch in _chunks(commands[measured_begin:], options.batch_size)
    )
    rss_before = _rss_bytes()
    start = time.perf_counter_ns()
    timed_committed = timed_rejected = timed_errors = timed_processed = 0
    for batch in measured_batches:
        result = engine.submit_batch(batch, output=options.output_mode)
        timed_committed += result.committed_count
        timed_rejected += result.rejected_count
        timed_processed += result.processed_count
        terminal_error = result.terminal_error is not None
        del result
        if terminal_error:
            timed_errors += 1
            break
    elapsed = max(1, time.perf_counter_ns() - start)
    rss_after = _rss_bytes()
    peak_rss = max(_peak_rss_bytes(), rss_before, rss_after)
    evidence = _validate_results(
        config,
        commands,
        measured_begin,
        options.batch_size,
    )
    expected = (
        options.measured_count,
        options.expected_events,
        options.expected_committed,
        options.expected_rejected,
        options.expected_engine_errors,
        options.expected_event_digest,
        options.expected_final_digest,
    )
    timed_outcomes = (
        timed_processed + timed_errors,
        timed_committed,
        timed_rejected,
        timed_errors,
        engine.state_digest(),
    )
    expected_timed = (
        options.measured_count,
        options.expected_committed,
        options.expected_rejected,
        options.expected_engine_errors,
        options.expected_final_digest,
    )
    if evidence != expected or timed_outcomes != expected_timed:
        raise ValueError("Python batch execution differs from frozen workload evidence")
    return _observation(
        options,
        elapsed_ns=elapsed,
        rss_before=rss_before,
        rss_after=rss_after,
        peak_rss=peak_rss,
        evidence=evidence,
        valid=True,
        failure_reason=None,
    )


def _digest(value: str) -> str:
    if _DIGEST.fullmatch(value) is None:
        raise argparse.ArgumentTypeError("value must be lowercase SHA-256")
    return value


def _identifier(value: str) -> str:
    if _IDENTIFIER.fullmatch(value) is None:
        raise argparse.ArgumentTypeError("value must be a safe identifier")
    return value


def _argument_uint(value: str) -> int:
    try:
        return _unsigned(value, "argument")
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    identity = commands.add_parser("identity")
    identity.add_argument("--wheel", type=Path, required=True)
    identity.add_argument("--include-private-path", action="store_true")
    run = commands.add_parser("run")
    run.add_argument("--workload", type=Path, required=True)
    run.add_argument("--workload-id", type=_identifier, required=True)
    run.add_argument("--workload-sha256", type=_digest, required=True)
    run.add_argument("--workload-manifest-sha256", type=_digest, required=True)
    run.add_argument("--binary-sha256", type=_digest, required=True)
    run.add_argument("--environment-sha256", type=_digest, required=True)
    run.add_argument("--host-context-sha256", type=_digest, required=True)
    run.add_argument("--suite-label", type=_identifier, required=True)
    run.add_argument("--run-label", type=_identifier, required=True)
    run.add_argument("--variant", choices=("standalone", "baseline", "candidate"), required=True)
    run.add_argument("--block-index", type=_argument_uint, required=True)
    run.add_argument("--block-position", type=_argument_uint, required=True)
    run.add_argument("--preload-count", type=_argument_uint, required=True)
    run.add_argument("--warmup-count", type=_argument_uint, required=True)
    run.add_argument("--measured-count", type=_argument_uint, required=True)
    run.add_argument("--expected-events", type=_argument_uint, required=True)
    run.add_argument("--expected-committed", type=_argument_uint, required=True)
    run.add_argument("--expected-rejected", type=_argument_uint, required=True)
    run.add_argument("--expected-engine-errors", type=_argument_uint, required=True)
    run.add_argument("--expected-event-digest", type=_digest, required=True)
    run.add_argument("--expected-final-digest", type=_digest, required=True)
    run.add_argument("--instrument-count", required=True)
    run.add_argument("--measured-start-active-order-count", required=True)
    run.add_argument("--sweep-depth", required=True)
    run.add_argument("--batch-size", type=int, choices=(1, 64, 1024, 65_536), required=True)
    run.add_argument("--output-mode", choices=("objects", "columns", "summary"), required=True)
    return parser


def main(arguments: list[str] | None = None) -> int:
    options = _parser().parse_args(arguments)
    try:
        if options.command == "identity":
            value = _identity(options.wheel, options.include_private_path)
        else:
            value = _run_observation(options)
    except Exception:
        if options.command != "run":
            return 1
        value = _observation(
            options,
            elapsed_ns=0,
            rss_before=0,
            rss_after=0,
            peak_rss=0,
            evidence=(0, 0, 0, 0, 0, "0" * 64, "0" * 64),
            valid=False,
            failure_reason="Python target-wheel worker failed validation",
        )
        sys.stdout.buffer.write(_canonical_json(value))
        return 1
    sys.stdout.buffer.write(_canonical_json(value))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
