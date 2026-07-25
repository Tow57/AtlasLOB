"""Strict process boundary for the test-only ``ATLAS_DIFF_V2`` adapter."""

from __future__ import annotations

import json
import re
import subprocess
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Literal, NoReturn, TypeAlias, TypeVar, cast

from atlaslob.canonical import engine_state_digest, event_digest
from atlaslob.domain import (
    ATLASLOB_SEMANTICS_VERSION,
    I64_MAX,
    I64_MIN,
    U8_MAX,
    U16_MAX,
    U32_MAX,
    U64_MAX,
    AcceptedEvent,
    BookChangedEvent,
    CanceledEvent,
    CancelOrder,
    Command,
    CommandType,
    DoneEvent,
    DoneReason,
    EngineError,
    EngineSnapshot,
    Event,
    EventBatch,
    EventHeader,
    InstrumentConfig,
    InstrumentSnapshot,
    MatchingConfig,
    MultiInstrumentEngineConfig,
    NewOrder,
    OrderSnapshot,
    OrderType,
    PriceLevelSnapshot,
    RejectedEvent,
    RejectReason,
    ReplacedEvent,
    ReplaceOrder,
    RestedEvent,
    Side,
    TimeInForce,
    TopOfBookLevel,
    TradeEvent,
    command_type,
)
from atlaslob.multi_differential import MultiDifferentialCapture
from atlaslob.multi_generation import MultiWorkloadSpec

_SCHEMA = "atlas_diff_v2"
_HEADER = "ATLAS_DIFF_V2"
_UNSIGNED_DECIMAL = re.compile(r"(?:0|[1-9][0-9]*)\Z")
_SIGNED_DECIMAL = re.compile(r"(?:0|-[1-9][0-9]*|[1-9][0-9]*)\Z")
_DIGEST = re.compile(r"[0-9a-f]{64}\Z")
_SUCCESS_EXIT_CODE = 0
_INPUT_ERROR_EXIT_CODE = 2
_ENGINE_ERROR_EXIT_CODE = 3

MultiOutputMode = Literal["exact", "compact"]
MultiOutcome = Literal["committed", "rejected", "engine_error"]
ValueT = TypeVar("ValueT")

_COMMAND_NAMES = {
    "new": CommandType.NEW,
    "cancel": CommandType.CANCEL,
    "replace": CommandType.REPLACE,
}
_SIDE_NAMES = {"buy": Side.BUY, "sell": Side.SELL}
_DONE_NAMES = {
    "filled": DoneReason.FILLED,
    "ioc_residual_canceled": DoneReason.IOC_RESIDUAL_CANCELED,
    "market_exhausted": DoneReason.MARKET_EXHAUSTED,
    "canceled": DoneReason.CANCELED,
    "replaced": DoneReason.REPLACED,
    "fok_unavailable": DoneReason.FOK_UNAVAILABLE,
}
_REJECT_NAMES = {
    "invalid_order_id": RejectReason.INVALID_ORDER_ID,
    "invalid_instrument_id": RejectReason.INVALID_INSTRUMENT_ID,
    "invalid_quantity": RejectReason.INVALID_QUANTITY,
    "invalid_side": RejectReason.INVALID_SIDE,
    "invalid_order_type": RejectReason.INVALID_ORDER_TYPE,
    "invalid_time_in_force": RejectReason.INVALID_TIME_IN_FORCE,
    "missing_limit_price": RejectReason.MISSING_LIMIT_PRICE,
    "unexpected_limit_price": RejectReason.UNEXPECTED_LIMIT_PRICE,
    "invalid_price": RejectReason.INVALID_PRICE,
    "invalid_order_type_time_in_force": RejectReason.INVALID_ORDER_TYPE_TIME_IN_FORCE,
    "unsupported_time_in_force": RejectReason.UNSUPPORTED_TIME_IN_FORCE,
    "invalid_client_id": RejectReason.INVALID_CLIENT_ID,
    "unknown_instrument": RejectReason.UNKNOWN_INSTRUMENT,
    "quantity_out_of_range": RejectReason.QUANTITY_OUT_OF_RANGE,
    "invalid_tick": RejectReason.INVALID_TICK,
    "duplicate_order_id": RejectReason.DUPLICATE_ORDER_ID,
    "unknown_order_id": RejectReason.UNKNOWN_ORDER_ID,
    "invalid_replacement_id": RejectReason.INVALID_REPLACEMENT_ID,
    "ownership_mismatch": RejectReason.OWNERSHIP_MISMATCH,
    "instrument_mismatch": RejectReason.INSTRUMENT_MISMATCH,
    "capacity_exceeded": RejectReason.CAPACITY_EXCEEDED,
}
_ENGINE_ERRORS = {
    "sequence_exhausted": EngineError.SEQUENCE_EXHAUSTED,
    "internal_failure": EngineError.INTERNAL_FAILURE,
}
_PROCESS_FAILURE_CODES = frozenset(
    {
        "adapter_exception",
        "engine_construction_failure",
        "engine_exception",
        "resource_failure",
    }
)
_REQUEST_BOUND_ERROR_CODES = frozenset(
    {
        "engine_exception",
        "resource_failure",
    }
)
_ERROR_CODES = frozenset(
    {
        "adapter_exception",
        "duplicate_instrument_id",
        "engine_construction_failure",
        "engine_exception",
        "input_read_failure",
        "invalid_cancel_client",
        "invalid_cancel_field_count",
        "invalid_cancel_instrument",
        "invalid_cancel_order",
        "invalid_engine_config",
        "invalid_header_catalog_count",
        "invalid_header_checkpoint_interval",
        "invalid_header_command_count",
        "invalid_header_field_count",
        "invalid_header_max_total_active_orders",
        "invalid_instrument_id",
        "invalid_instrument_max_active_orders",
        "invalid_instrument_max_quantity",
        "invalid_instrument_record",
        "invalid_instrument_tick_increment",
        "invalid_line_ending",
        "invalid_new_client",
        "invalid_new_field_count",
        "invalid_new_instrument",
        "invalid_new_order",
        "invalid_new_order_type_code",
        "invalid_new_price",
        "invalid_new_price_presence",
        "invalid_new_quantity",
        "invalid_new_side_code",
        "invalid_new_time_in_force_code",
        "invalid_replace_client",
        "invalid_replace_field_count",
        "invalid_replace_instrument",
        "invalid_replace_new_order",
        "invalid_replace_old_order",
        "invalid_replace_price",
        "invalid_replace_quantity",
        "missing_command",
        "missing_header",
        "missing_instrument_record",
        "noncanonical_command",
        "noncanonical_header",
        "noncanonical_instrument",
        "nonzero_absent_price_placeholder",
        "resource_failure",
        "unexpected_trailing_input",
        "unknown_command",
        "unsupported_header",
    }
)


class MultiNativeProtocolError(ValueError):
    """Raised when V2 native evidence violates its closed schema."""


@dataclass(frozen=True, slots=True)
class MultiNativeInput:
    catalog: tuple[InstrumentConfig, ...]
    engine: MultiInstrumentEngineConfig = MultiInstrumentEngineConfig()
    checkpoint_interval: int = 0

    def __post_init__(self) -> None:
        if not self.catalog:
            raise ValueError("catalog must contain at least one instrument")
        if any(not isinstance(entry, InstrumentConfig) for entry in self.catalog):
            raise TypeError("catalog must contain InstrumentConfig values")
        identifiers = tuple(entry.instrument_id for entry in self.catalog)
        if identifiers != tuple(sorted(identifiers)) or len(identifiers) != len(set(identifiers)):
            raise ValueError("catalog must have unique, sorted instrument IDs")
        if not isinstance(self.engine, MultiInstrumentEngineConfig):
            raise TypeError("engine must be a MultiInstrumentEngineConfig")
        _require_uint("checkpoint_interval", self.checkpoint_interval, U64_MAX)


@dataclass(frozen=True, slots=True)
class MultiNativeConfigRecord:
    mode: MultiOutputMode
    semantics_version: int
    input: MultiNativeInput
    command_count: int


@dataclass(frozen=True, slots=True)
class MultiNativeResultRecord:
    command_index: int
    line: int
    command_type: CommandType
    outcome: MultiOutcome
    command_sequence: int | None
    engine_error: EngineError | None
    reject_reason: RejectReason | None
    event_digest: str | None
    events: tuple[Event, ...] | None
    post_state_digest: str
    checkpoint_snapshot: EngineSnapshot | None


@dataclass(frozen=True, slots=True)
class MultiNativeFinalRecord:
    commands_declared: int
    commands_processed: int
    committed: int
    rejected: int
    engine_errors: int
    final_state_digest: str
    snapshot: EngineSnapshot


@dataclass(frozen=True, slots=True)
class MultiNativeErrorRecord:
    line: int
    code: str


@dataclass(frozen=True, slots=True)
class MultiNativeTranscript:
    config: MultiNativeConfigRecord | None
    results: tuple[MultiNativeResultRecord, ...]
    final: MultiNativeFinalRecord | None
    error: MultiNativeErrorRecord | None


@dataclass(frozen=True, slots=True)
class MultiNativeRun:
    returncode: int
    transcript: MultiNativeTranscript
    stdout: str
    stderr: str


MultiNativeRecord: TypeAlias = (
    MultiNativeConfigRecord
    | MultiNativeResultRecord
    | MultiNativeFinalRecord
    | MultiNativeErrorRecord
)


def multi_native_input_from_spec(
    spec: MultiWorkloadSpec,
    *,
    checkpoint_interval: int = 0,
) -> MultiNativeInput:
    if not isinstance(spec, MultiWorkloadSpec):
        raise TypeError("spec must be a MultiWorkloadSpec")
    return MultiNativeInput(spec.catalog, spec.engine, checkpoint_interval)


def encode_multi_header(config: MultiNativeInput, command_count: int) -> str:
    _require_uint("command_count", command_count, U64_MAX)
    return " ".join(
        (
            _HEADER,
            str(config.engine.max_total_active_orders),
            str(len(config.catalog)),
            str(command_count),
            str(config.checkpoint_interval),
        )
    )


def encode_multi_instrument(config: InstrumentConfig) -> str:
    return " ".join(
        (
            "I",
            str(config.instrument_id),
            str(config.matching.max_order_quantity),
            str(config.matching.tick_increment),
            str(config.matching.max_active_orders),
        )
    )


def encode_multi_command(command: Command) -> str:
    _require_representable(command)
    if isinstance(command, NewOrder):
        present = int(command.limit_price is not None)
        price = command.limit_price if command.limit_price is not None else 0
        return " ".join(
            str(value)
            for value in (
                "N",
                command.client_id,
                command.order_id,
                command.instrument_id,
                command.side,
                command.order_type,
                command.time_in_force,
                present,
                price,
                command.quantity,
            )
        )
    if isinstance(command, CancelOrder):
        return f"C {command.client_id} {command.order_id} {command.instrument_id}"
    if isinstance(command, ReplaceOrder):
        return " ".join(
            str(value)
            for value in (
                "R",
                command.client_id,
                command.old_order_id,
                command.new_order_id,
                command.instrument_id,
                command.new_limit_price,
                command.new_quantity,
            )
        )
    raise TypeError(f"unsupported command type: {type(command)!r}")


def encode_multi_stream(
    config: MultiNativeInput,
    commands: Sequence[Command],
) -> str:
    lines = [encode_multi_header(config, len(commands))]
    lines.extend(encode_multi_instrument(entry) for entry in config.catalog)
    lines.extend(encode_multi_command(command) for command in commands)
    return "\n".join(lines) + "\n"


def encode_multi_workload(
    spec: MultiWorkloadSpec,
    commands: Sequence[Command],
    *,
    checkpoint_interval: int = 0,
) -> str:
    if len(commands) != spec.command_count:
        raise ValueError("command count differs from its MultiWorkloadSpec")
    return encode_multi_stream(
        multi_native_input_from_spec(
            spec,
            checkpoint_interval=checkpoint_interval,
        ),
        commands,
    )


def run_multi_native(
    executable: str | Path,
    config: MultiNativeInput,
    commands: Sequence[Command],
    *,
    mode: MultiOutputMode = "exact",
    timeout: float = 30.0,
) -> MultiNativeRun:
    if mode not in ("exact", "compact"):
        raise ValueError(f"unsupported output mode: {mode}")
    try:
        completed = subprocess.run(
            [str(Path(executable)), mode],
            input=encode_multi_stream(config, commands).encode("ascii"),
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        stdout = completed.stdout.decode("utf-8", errors="strict")
        stderr = completed.stderr.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise MultiNativeProtocolError("native process output is not valid text") from exc
    if stderr:
        raise MultiNativeProtocolError("native process wrote unexpected standard-error output")
    transcript = decode_multi_jsonl(
        stdout,
        expected_mode=mode,
        expected_input=config,
        expected_commands=commands,
        returncode=completed.returncode,
    )
    return MultiNativeRun(
        returncode=completed.returncode,
        transcript=transcript,
        stdout=stdout,
        stderr=stderr,
    )


def run_multi_workload(
    executable: str | Path,
    spec: MultiWorkloadSpec,
    commands: Sequence[Command],
    *,
    mode: MultiOutputMode = "exact",
    checkpoint_interval: int = 0,
    timeout: float = 30.0,
) -> MultiNativeRun:
    if len(commands) != spec.command_count:
        raise ValueError("command count differs from its MultiWorkloadSpec")
    return run_multi_native(
        executable,
        multi_native_input_from_spec(
            spec,
            checkpoint_interval=checkpoint_interval,
        ),
        commands,
        mode=mode,
        timeout=timeout,
    )


def decode_multi_jsonl(
    text: str,
    *,
    expected_mode: MultiOutputMode | None = None,
    expected_input: MultiNativeInput | None = None,
    expected_commands: Sequence[Command] | None = None,
    returncode: int | None = None,
) -> MultiNativeTranscript:
    if not text or not text.endswith("\n") or "\r" in text:
        raise MultiNativeProtocolError("native output must be nonempty LF-terminated JSONL")
    raw_lines = text[:-1].split("\n")
    if any(not line for line in raw_lines):
        raise MultiNativeProtocolError("native output contains an empty JSONL record")

    config: MultiNativeConfigRecord | None = None
    results: list[MultiNativeResultRecord] = []
    final: MultiNativeFinalRecord | None = None
    error: MultiNativeErrorRecord | None = None
    for output_line, line in enumerate(raw_lines, start=1):
        record = _parse_json_line(line, output_line)
        kind = _string(_field(record, "kind"), "kind")
        if final is not None or error is not None:
            raise MultiNativeProtocolError("records appear after a terminal record")
        if kind == "config":
            if config is not None or results:
                raise MultiNativeProtocolError("config record is not first")
            config = _parse_config(record)
            if expected_mode is not None and config.mode != expected_mode:
                raise MultiNativeProtocolError("native mode differs from requested mode")
            if expected_input is not None and config.input != expected_input:
                raise MultiNativeProtocolError("native config differs from requested input")
        elif kind == "result":
            if config is None:
                raise MultiNativeProtocolError("result appears before config")
            parsed_result = _parse_result(record, config)
            _validate_result_position(
                parsed_result,
                config,
                len(results),
                expected_commands,
            )
            results.append(parsed_result)
        elif kind == "final":
            if config is None:
                raise MultiNativeProtocolError("final appears before config")
            final = _parse_final(record, config)
        elif kind == "error":
            error = _parse_error(record)
        else:
            raise MultiNativeProtocolError(f"unknown record kind: {kind}")

    transcript = MultiNativeTranscript(config, tuple(results), final, error)
    _validate_transcript(
        transcript,
        expected_request=(
            expected_mode is not None or expected_input is not None or expected_commands is not None
        ),
        expected_commands=expected_commands,
        returncode=returncode,
    )
    return transcript


def assert_multi_native_parity(
    run: MultiNativeRun,
    capture: MultiDifferentialCapture,
) -> None:
    transcript = run.transcript
    if transcript.error is not None or transcript.config is None or transcript.final is None:
        raise AssertionError("native run did not produce a successful transcript")
    if transcript.config.input.catalog != capture.catalog:
        raise AssertionError("native and reference catalogs differ")
    if transcript.config.input.engine != capture.engine_config:
        raise AssertionError("native and reference engine configs differ")
    if len(transcript.results) != len(capture.records):
        raise AssertionError("native and reference result counts differ")

    expected_committed = sum(record.result.committed for record in capture.records)
    expected_rejected = sum(record.result.rejected for record in capture.records)
    expected_engine_errors = sum(record.result.error is not None for record in capture.records)
    final = transcript.final
    if (
        final.commands_processed != len(capture.records)
        or final.committed != expected_committed
        or final.rejected != expected_rejected
        or final.engine_errors != expected_engine_errors
    ):
        raise AssertionError("native and reference final classification counts differ")

    mode = transcript.config.mode
    interval = transcript.config.input.checkpoint_interval
    for native, reference in zip(
        transcript.results,
        capture.records,
        strict=True,
    ):
        expected = reference.result
        expected_outcome: MultiOutcome
        if expected.error is not None:
            expected_outcome = "engine_error"
        elif expected.committed:
            expected_outcome = "committed"
        else:
            expected_outcome = "rejected"
        if native.outcome != expected_outcome:
            raise AssertionError(f"outcome differs at command {native.command_index}")
        if native.command_type != command_type(reference.command):
            raise AssertionError(f"command type differs at command {native.command_index}")
        if expected.error is not None:
            if native.engine_error != expected.error:
                raise AssertionError(f"engine error differs at command {native.command_index}")
        else:
            batch = expected.batch
            if batch is None:
                raise AssertionError("reference domain result has no event batch")
            if native.command_sequence != batch.command_sequence:
                raise AssertionError(f"sequence differs at command {native.command_index}")
            expected_reason = (
                cast(RejectedEvent, batch.events[0]).reason if batch.rejected else None
            )
            if native.reject_reason != expected_reason:
                raise AssertionError(f"rejection differs at command {native.command_index}")
            if native.event_digest != event_digest(batch):
                raise AssertionError(f"event digest differs at command {native.command_index}")
            if mode == "exact" and native.events != batch.events:
                raise AssertionError(f"events differ at command {native.command_index}")
            if mode == "compact" and native.events is not None:
                raise AssertionError("compact native result unexpectedly contains events")
        if native.post_state_digest != reference.post_state_digest:
            raise AssertionError(f"state digest differs at command {native.command_index}")
        checkpoint = interval != 0 and (native.command_index + 1) % interval == 0
        expected_snapshot = reference.post_snapshot if checkpoint else None
        if native.checkpoint_snapshot != expected_snapshot:
            raise AssertionError(f"checkpoint differs at command {native.command_index}")

    if final.snapshot != capture.final_snapshot:
        raise AssertionError("native and reference final snapshots differ")
    if final.final_state_digest != capture.final_state_digest:
        raise AssertionError("native and reference final digests differ")


def _parse_config(record: dict[str, object]) -> MultiNativeConfigRecord:
    _require_keys(
        record,
        {
            "schema",
            "kind",
            "mode",
            "semantics_version",
            "max_total_active_orders",
            "catalog_count",
            "catalog",
            "command_count",
            "checkpoint_interval",
        },
    )
    mode_value = _string(_field(record, "mode"), "mode")
    if mode_value not in ("exact", "compact"):
        raise MultiNativeProtocolError("config mode is outside the V2 vocabulary")
    semantics = _decimal_uint(
        _field(record, "semantics_version"),
        "semantics_version",
        U16_MAX,
    )
    if semantics != ATLASLOB_SEMANTICS_VERSION:
        raise MultiNativeProtocolError("config semantics version is unsupported")
    catalog_count = _decimal_uint(
        _field(record, "catalog_count"),
        "catalog_count",
        U32_MAX,
    )
    catalog = _parse_catalog(_field(record, "catalog"))
    if len(catalog) != catalog_count:
        raise MultiNativeProtocolError("catalog count differs from catalog records")
    try:
        input_config = MultiNativeInput(
            catalog=catalog,
            engine=MultiInstrumentEngineConfig(
                _decimal_uint(
                    _field(record, "max_total_active_orders"),
                    "max_total_active_orders",
                    U64_MAX,
                )
            ),
            checkpoint_interval=_decimal_uint(
                _field(record, "checkpoint_interval"),
                "checkpoint_interval",
                U64_MAX,
            ),
        )
    except (TypeError, ValueError) as exc:
        raise MultiNativeProtocolError("native config is outside the domain") from exc
    return MultiNativeConfigRecord(
        mode=cast(MultiOutputMode, mode_value),
        semantics_version=semantics,
        input=input_config,
        command_count=_decimal_uint(
            _field(record, "command_count"),
            "command_count",
            U64_MAX,
        ),
    )


def _parse_result(
    record: dict[str, object],
    config: MultiNativeConfigRecord,
) -> MultiNativeResultRecord:
    _require_keys(
        record,
        {
            "schema",
            "kind",
            "command_index",
            "line",
            "command_type",
            "outcome",
            "command_sequence",
            "engine_error",
            "reject_reason",
            "event_digest",
            "events",
            "post_state_digest",
            "checkpoint_snapshot",
        },
    )
    command_name = _string(_field(record, "command_type"), "command_type")
    if command_name not in _COMMAND_NAMES:
        raise MultiNativeProtocolError("command type is outside the V2 vocabulary")
    outcome_value = _string(_field(record, "outcome"), "outcome")
    if outcome_value not in ("committed", "rejected", "engine_error"):
        raise MultiNativeProtocolError("outcome is outside the V2 vocabulary")
    outcome = cast(MultiOutcome, outcome_value)
    sequence = _optional_decimal_uint(
        _field(record, "command_sequence"),
        "command_sequence",
        U64_MAX,
    )
    engine_error = _optional_name(
        _field(record, "engine_error"),
        "engine_error",
        _ENGINE_ERRORS,
    )
    reject_reason = _optional_name(
        _field(record, "reject_reason"),
        "reject_reason",
        _REJECT_NAMES,
    )
    digest = _optional_digest(_field(record, "event_digest"), "event_digest")
    raw_events = _field(record, "events")
    events = None if raw_events is None else _parse_events(raw_events)
    raw_snapshot = _field(record, "checkpoint_snapshot")
    snapshot = None if raw_snapshot is None else _parse_engine_snapshot(raw_snapshot, config.input)
    if outcome == "engine_error":
        if (
            any(value is not None for value in (sequence, reject_reason, digest, events))
            or engine_error is None
        ):
            raise MultiNativeProtocolError("engine-error result has inconsistent fields")
    else:
        if sequence is None or engine_error is not None or digest is None:
            raise MultiNativeProtocolError("domain result has inconsistent fields")
        if outcome == "rejected" and reject_reason is None:
            raise MultiNativeProtocolError("rejected result lacks a reason")
        if outcome == "committed" and reject_reason is not None:
            raise MultiNativeProtocolError("committed result carries a reason")
        if config.mode == "exact":
            if events is None:
                raise MultiNativeProtocolError("exact result omits events")
            batch = _validate_event_batch(
                events,
                sequence,
                _COMMAND_NAMES[command_name],
                outcome,
                reject_reason,
            )
            if event_digest(batch) != digest:
                raise MultiNativeProtocolError("event digest differs from event values")
        elif events is not None:
            raise MultiNativeProtocolError("compact result contains events")

    command_index = _decimal_uint(
        _field(record, "command_index"),
        "command_index",
        U64_MAX,
    )
    checkpoint = (
        config.input.checkpoint_interval != 0
        and (command_index + 1) % config.input.checkpoint_interval == 0
    )
    if (snapshot is not None) != checkpoint:
        raise MultiNativeProtocolError("checkpoint presence differs from requested interval")
    if snapshot is not None:
        expected_last_sequence = sequence if sequence is not None else command_index
        if snapshot.last_sequence != expected_last_sequence:
            raise MultiNativeProtocolError("checkpoint sequence differs from the result position")
    post_digest = _digest_value(_field(record, "post_state_digest"), "post_state_digest")
    if snapshot is not None and engine_state_digest(snapshot) != post_digest:
        raise MultiNativeProtocolError("checkpoint digest differs from checkpoint snapshot")
    return MultiNativeResultRecord(
        command_index=command_index,
        line=_decimal_uint(_field(record, "line"), "line", U64_MAX),
        command_type=_COMMAND_NAMES[command_name],
        outcome=outcome,
        command_sequence=sequence,
        engine_error=engine_error,
        reject_reason=reject_reason,
        event_digest=digest,
        events=events,
        post_state_digest=post_digest,
        checkpoint_snapshot=snapshot,
    )


def _parse_final(
    record: dict[str, object],
    config: MultiNativeConfigRecord,
) -> MultiNativeFinalRecord:
    _require_keys(
        record,
        {
            "schema",
            "kind",
            "commands_declared",
            "commands_processed",
            "committed",
            "rejected",
            "engine_errors",
            "final_state_digest",
            "snapshot",
        },
    )
    snapshot = _parse_engine_snapshot(_field(record, "snapshot"), config.input)
    digest = _digest_value(_field(record, "final_state_digest"), "final_state_digest")
    if engine_state_digest(snapshot) != digest:
        raise MultiNativeProtocolError("final digest differs from final snapshot")
    return MultiNativeFinalRecord(
        commands_declared=_decimal_uint(
            _field(record, "commands_declared"),
            "commands_declared",
            U64_MAX,
        ),
        commands_processed=_decimal_uint(
            _field(record, "commands_processed"),
            "commands_processed",
            U64_MAX,
        ),
        committed=_decimal_uint(_field(record, "committed"), "committed", U64_MAX),
        rejected=_decimal_uint(_field(record, "rejected"), "rejected", U64_MAX),
        engine_errors=_decimal_uint(
            _field(record, "engine_errors"),
            "engine_errors",
            U64_MAX,
        ),
        final_state_digest=digest,
        snapshot=snapshot,
    )


def _parse_error(record: dict[str, object]) -> MultiNativeErrorRecord:
    _require_keys(record, {"schema", "kind", "line", "code"})
    code = _string(_field(record, "code"), "code")
    if code not in _ERROR_CODES:
        raise MultiNativeProtocolError("error code is outside the V2 vocabulary")
    return MultiNativeErrorRecord(
        line=_decimal_uint(_field(record, "line"), "line", U64_MAX),
        code=code,
    )


def _parse_catalog(value: object) -> tuple[InstrumentConfig, ...]:
    output: list[InstrumentConfig] = []
    for index, raw in enumerate(_array(value, "catalog")):
        record = _object(raw, f"catalog[{index}]")
        _require_keys(
            record,
            {
                "instrument_id",
                "max_order_quantity",
                "tick_increment",
                "max_active_orders",
            },
        )
        try:
            output.append(
                InstrumentConfig(
                    instrument_id=_decimal_uint(
                        _field(record, "instrument_id"),
                        "instrument_id",
                        U32_MAX,
                    ),
                    matching=MatchingConfig(
                        max_order_quantity=_decimal_uint(
                            _field(record, "max_order_quantity"),
                            "max_order_quantity",
                            U64_MAX,
                        ),
                        tick_increment=_decimal_int(
                            _field(record, "tick_increment"),
                            "tick_increment",
                            I64_MIN,
                            I64_MAX,
                        ),
                        max_active_orders=_decimal_uint(
                            _field(record, "max_active_orders"),
                            "max_active_orders",
                            U64_MAX,
                        ),
                    ),
                )
            )
        except (TypeError, ValueError) as exc:
            raise MultiNativeProtocolError("catalog entry is outside the domain") from exc
    identifiers = tuple(entry.instrument_id for entry in output)
    if identifiers != tuple(sorted(identifiers)) or len(identifiers) != len(set(identifiers)):
        raise MultiNativeProtocolError("catalog is not unique and sorted")
    if not output:
        raise MultiNativeProtocolError("catalog is empty")
    return tuple(output)


def _parse_engine_snapshot(
    value: object,
    expected_input: MultiNativeInput,
) -> EngineSnapshot:
    record = _object(value, "snapshot")
    _require_keys(
        record,
        {
            "semantics_version",
            "engine_config",
            "catalog",
            "last_sequence",
            "sequence_exhausted",
            "active_order_count",
            "instruments",
        },
    )
    semantics = _decimal_uint(
        _field(record, "semantics_version"),
        "snapshot.semantics_version",
        U16_MAX,
    )
    if semantics != ATLASLOB_SEMANTICS_VERSION:
        raise MultiNativeProtocolError("snapshot semantics version is unsupported")
    raw_engine = _object(_field(record, "engine_config"), "engine_config")
    _require_keys(raw_engine, {"max_total_active_orders"})
    try:
        engine_config = MultiInstrumentEngineConfig(
            _decimal_uint(
                _field(raw_engine, "max_total_active_orders"),
                "snapshot.max_total_active_orders",
                U64_MAX,
            )
        )
    except ValueError as exc:
        raise MultiNativeProtocolError("snapshot engine config is invalid") from exc
    catalog = _parse_catalog(_field(record, "catalog"))
    if engine_config != expected_input.engine or catalog != expected_input.catalog:
        raise MultiNativeProtocolError("snapshot config differs from echoed config")

    raw_instruments = _array(_field(record, "instruments"), "instruments")
    if len(raw_instruments) != len(catalog):
        raise MultiNativeProtocolError("snapshot instrument count differs from catalog")
    instruments: list[InstrumentSnapshot] = []
    global_ids: set[int] = set()
    global_priorities: set[int] = set()
    for index, (raw_instrument, instrument_config) in enumerate(
        zip(raw_instruments, catalog, strict=True)
    ):
        instrument = _object(raw_instrument, f"instruments[{index}]")
        _require_keys(
            instrument,
            {"instrument_id", "active_order_count", "bids", "asks"},
        )
        instrument_id = _decimal_uint(
            _field(instrument, "instrument_id"),
            "snapshot.instrument_id",
            U32_MAX,
        )
        if instrument_id != instrument_config.instrument_id:
            raise MultiNativeProtocolError("snapshot instruments do not match catalog order")
        bids = _parse_levels(
            _field(instrument, "bids"),
            Side.BUY,
            instrument_config,
        )
        asks = _parse_levels(
            _field(instrument, "asks"),
            Side.SELL,
            instrument_config,
        )
        orders = tuple(order for level in (*bids, *asks) for order in level.orders)
        active_count = _decimal_uint(
            _field(instrument, "active_order_count"),
            "instrument.active_order_count",
            U64_MAX,
        )
        if len(orders) != active_count:
            raise MultiNativeProtocolError("instrument active count differs from orders")
        if active_count > instrument_config.matching.max_active_orders:
            raise MultiNativeProtocolError("instrument exceeds configured capacity")
        for order in orders:
            if order.order_id in global_ids:
                raise MultiNativeProtocolError("snapshot contains a global duplicate order ID")
            if order.priority_sequence in global_priorities:
                raise MultiNativeProtocolError("snapshot contains a duplicate global priority")
            global_ids.add(order.order_id)
            global_priorities.add(order.priority_sequence)
        if bids and asks and bids[0].price >= asks[0].price:
            raise MultiNativeProtocolError("snapshot instrument is crossed")
        instruments.append(
            InstrumentSnapshot(
                instrument_id=instrument_id,
                active_order_count=active_count,
                bids=bids,
                asks=asks,
            )
        )

    last_sequence = _decimal_uint(
        _field(record, "last_sequence"),
        "snapshot.last_sequence",
        U64_MAX,
    )
    exhausted = _boolean(
        _field(record, "sequence_exhausted"),
        "snapshot.sequence_exhausted",
    )
    if exhausted != (last_sequence == U64_MAX):
        raise MultiNativeProtocolError("snapshot sequence exhaustion is inconsistent")
    if any(priority > last_sequence for priority in global_priorities):
        raise MultiNativeProtocolError("snapshot priority exceeds global sequence")
    active_count = _decimal_uint(
        _field(record, "active_order_count"),
        "snapshot.active_order_count",
        U64_MAX,
    )
    if active_count != len(global_ids):
        raise MultiNativeProtocolError("snapshot total active count differs from books")
    if active_count > engine_config.max_total_active_orders:
        raise MultiNativeProtocolError("snapshot exceeds global capacity")
    return EngineSnapshot(
        semantics_version=semantics,
        engine_config=engine_config,
        catalog=catalog,
        last_sequence=last_sequence,
        sequence_exhausted=exhausted,
        active_order_count=active_count,
        instruments=tuple(instruments),
    )


def _parse_levels(
    value: object,
    side: Side,
    instrument_config: InstrumentConfig,
) -> tuple[PriceLevelSnapshot, ...]:
    output: list[PriceLevelSnapshot] = []
    for level_index, raw_level in enumerate(_array(value, "levels")):
        level = _object(raw_level, f"level[{level_index}]")
        _require_keys(level, {"price", "aggregate_quantity", "orders"})
        price = _decimal_int(_field(level, "price"), "level.price", I64_MIN, I64_MAX)
        if price <= 0 or price % instrument_config.matching.tick_increment != 0:
            raise MultiNativeProtocolError("snapshot level price is invalid")
        orders: list[OrderSnapshot] = []
        for order_index, raw_order in enumerate(_array(_field(level, "orders"), "orders")):
            order = _object(raw_order, f"order[{order_index}]")
            _require_keys(
                order,
                {
                    "order_id",
                    "client_id",
                    "instrument_id",
                    "side",
                    "price",
                    "remaining_quantity",
                    "priority_sequence",
                },
            )
            order_side = _name(_field(order, "side"), "order.side", _SIDE_NAMES)
            order_price = _decimal_int(
                _field(order, "price"),
                "order.price",
                I64_MIN,
                I64_MAX,
            )
            instrument_id = _decimal_uint(
                _field(order, "instrument_id"),
                "order.instrument_id",
                U32_MAX,
            )
            remaining = _decimal_uint(
                _field(order, "remaining_quantity"),
                "order.remaining_quantity",
                U64_MAX,
            )
            snapshot_order = OrderSnapshot(
                order_id=_decimal_uint(
                    _field(order, "order_id"),
                    "order.order_id",
                    U64_MAX,
                ),
                client_id=_decimal_uint(
                    _field(order, "client_id"),
                    "order.client_id",
                    U32_MAX,
                ),
                instrument_id=instrument_id,
                side=order_side,
                price=order_price,
                remaining_quantity=remaining,
                priority_sequence=_decimal_uint(
                    _field(order, "priority_sequence"),
                    "order.priority_sequence",
                    U64_MAX,
                ),
            )
            if (
                snapshot_order.order_id == 0
                or snapshot_order.client_id == 0
                or snapshot_order.instrument_id != instrument_config.instrument_id
                or snapshot_order.side != side
                or snapshot_order.price != price
                or snapshot_order.remaining_quantity == 0
                or snapshot_order.remaining_quantity > instrument_config.matching.max_order_quantity
                or snapshot_order.priority_sequence == 0
            ):
                raise MultiNativeProtocolError("snapshot order is structurally invalid")
            if orders and orders[-1].priority_sequence >= snapshot_order.priority_sequence:
                raise MultiNativeProtocolError("snapshot FIFO priorities are not increasing")
            orders.append(snapshot_order)
        if not orders:
            raise MultiNativeProtocolError("snapshot contains an empty level")
        aggregate = _decimal_uint(
            _field(level, "aggregate_quantity"),
            "level.aggregate_quantity",
            U64_MAX,
        )
        if aggregate != sum(order.remaining_quantity for order in orders):
            raise MultiNativeProtocolError("level aggregate differs from orders")
        output.append(PriceLevelSnapshot(price, aggregate, tuple(orders)))
    prices = [level.price for level in output]
    expected = sorted(prices, reverse=side == Side.BUY)
    if prices != expected or len(prices) != len(set(prices)):
        raise MultiNativeProtocolError("levels are not unique best-price-first values")
    return tuple(output)


def _parse_events(value: object) -> tuple[Event, ...]:
    events: list[Event] = []
    for index, raw_event in enumerate(_array(value, "events")):
        record = _object(raw_event, f"event[{index}]")
        type_name = _string(_field(record, "type"), "event.type")
        header = EventHeader(
            command_sequence=_decimal_uint(
                _field(record, "command_sequence"),
                "event.command_sequence",
                U64_MAX,
            ),
            event_index=_decimal_uint(
                _field(record, "event_index"),
                "event.event_index",
                U32_MAX,
            ),
            instrument_id=_decimal_uint(
                _field(record, "instrument_id"),
                "event.instrument_id",
                U32_MAX,
            ),
        )
        base = {"type", "command_sequence", "event_index", "instrument_id"}
        if type_name == "accepted":
            _require_keys(record, base | {"command_type"})
            event: Event = AcceptedEvent(
                header,
                _name(_field(record, "command_type"), "command_type", _COMMAND_NAMES),
            )
        elif type_name == "rejected":
            _require_keys(record, base | {"command_type", "reason", "order_id"})
            event = RejectedEvent(
                header,
                _name(_field(record, "command_type"), "command_type", _COMMAND_NAMES),
                _name(_field(record, "reason"), "reason", _REJECT_NAMES),
                _optional_decimal_uint(_field(record, "order_id"), "order_id", U64_MAX),
            )
        elif type_name == "trade":
            _require_keys(
                record,
                base
                | {
                    "aggressor_order_id",
                    "resting_order_id",
                    "aggressor_client_id",
                    "resting_client_id",
                    "aggressor_side",
                    "execution_price",
                    "execution_quantity",
                    "aggressor_remaining",
                    "resting_remaining",
                },
            )
            event = TradeEvent(
                header,
                _decimal_uint(
                    _field(record, "aggressor_order_id"),
                    "aggressor_order_id",
                    U64_MAX,
                ),
                _decimal_uint(
                    _field(record, "resting_order_id"),
                    "resting_order_id",
                    U64_MAX,
                ),
                _decimal_uint(
                    _field(record, "aggressor_client_id"),
                    "aggressor_client_id",
                    U32_MAX,
                ),
                _decimal_uint(
                    _field(record, "resting_client_id"),
                    "resting_client_id",
                    U32_MAX,
                ),
                _name(
                    _field(record, "aggressor_side"),
                    "aggressor_side",
                    _SIDE_NAMES,
                ),
                _decimal_int(
                    _field(record, "execution_price"),
                    "execution_price",
                    I64_MIN,
                    I64_MAX,
                ),
                _decimal_uint(
                    _field(record, "execution_quantity"),
                    "execution_quantity",
                    U64_MAX,
                ),
                _decimal_uint(
                    _field(record, "aggressor_remaining"),
                    "aggressor_remaining",
                    U64_MAX,
                ),
                _decimal_uint(
                    _field(record, "resting_remaining"),
                    "resting_remaining",
                    U64_MAX,
                ),
            )
        elif type_name == "rested":
            _require_keys(
                record,
                base | {"order_id", "client_id", "side", "price", "remaining_quantity"},
            )
            event = RestedEvent(
                header,
                _decimal_uint(_field(record, "order_id"), "order_id", U64_MAX),
                _decimal_uint(_field(record, "client_id"), "client_id", U32_MAX),
                _name(_field(record, "side"), "side", _SIDE_NAMES),
                _decimal_int(_field(record, "price"), "price", I64_MIN, I64_MAX),
                _decimal_uint(
                    _field(record, "remaining_quantity"),
                    "remaining_quantity",
                    U64_MAX,
                ),
            )
        elif type_name == "canceled":
            _require_keys(record, base | {"order_id", "canceled_quantity"})
            event = CanceledEvent(
                header,
                _decimal_uint(_field(record, "order_id"), "order_id", U64_MAX),
                _decimal_uint(
                    _field(record, "canceled_quantity"),
                    "canceled_quantity",
                    U64_MAX,
                ),
            )
        elif type_name == "replaced":
            _require_keys(record, base | {"old_order_id", "new_order_id"})
            event = ReplacedEvent(
                header,
                _decimal_uint(
                    _field(record, "old_order_id"),
                    "old_order_id",
                    U64_MAX,
                ),
                _decimal_uint(
                    _field(record, "new_order_id"),
                    "new_order_id",
                    U64_MAX,
                ),
            )
        elif type_name == "done":
            _require_keys(record, base | {"order_id", "reason", "remaining_quantity"})
            event = DoneEvent(
                header,
                _decimal_uint(_field(record, "order_id"), "order_id", U64_MAX),
                _name(_field(record, "reason"), "reason", _DONE_NAMES),
                _decimal_uint(
                    _field(record, "remaining_quantity"),
                    "remaining_quantity",
                    U64_MAX,
                ),
            )
        elif type_name == "book_changed":
            _require_keys(record, base | {"best_bid", "best_ask"})
            event = BookChangedEvent(
                header,
                _parse_optional_top(_field(record, "best_bid"), "best_bid"),
                _parse_optional_top(_field(record, "best_ask"), "best_ask"),
            )
        else:
            raise MultiNativeProtocolError(f"unknown event type: {type_name}")
        _validate_event_payload(event)
        events.append(event)
    if not events:
        raise MultiNativeProtocolError("exact event list is empty")
    return tuple(events)


def _validate_event_batch(
    events: tuple[Event, ...],
    sequence: int,
    expected_command_type: CommandType,
    outcome: MultiOutcome,
    reject_reason: RejectReason | None,
) -> EventBatch:
    try:
        batch = EventBatch(events)
    except ValueError as exc:
        raise MultiNativeProtocolError("event batch is structurally invalid") from exc
    if batch.command_sequence != sequence:
        raise MultiNativeProtocolError("event sequence differs from result sequence")
    if outcome == "rejected":
        if len(events) != 1 or not isinstance(events[0], RejectedEvent):
            raise MultiNativeProtocolError("rejected result is not one rejection")
        if events[0].command_type != expected_command_type or events[0].reason != reject_reason:
            raise MultiNativeProtocolError("rejected event differs from result envelope")
    elif (
        outcome != "committed"
        or not isinstance(events[0], AcceptedEvent)
        or events[0].command_type != expected_command_type
    ):
        raise MultiNativeProtocolError("committed result lacks matching acceptance")
    elif expected_command_type == CommandType.CANCEL:
        _validate_cancel_envelope(events)
    elif expected_command_type == CommandType.REPLACE:
        _validate_replace_envelope(events)
    else:
        _validate_new_envelope(events)
    return batch


def _validate_new_envelope(events: tuple[Event, ...]) -> None:
    trades, terminal, book_changed = _trade_terminal_suffix(events, 1)
    _validate_trade_chain(trades, terminal)
    if isinstance(terminal, DoneEvent) and terminal.reason not in (
        DoneReason.FILLED,
        DoneReason.IOC_RESIDUAL_CANCELED,
        DoneReason.MARKET_EXHAUSTED,
    ):
        raise MultiNativeProtocolError("new-order done event has an invalid terminal reason")
    if book_changed is not None:
        _validate_terminal_top(book_changed)


def _validate_cancel_envelope(events: tuple[Event, ...]) -> None:
    if len(events) not in (3, 4):
        raise MultiNativeProtocolError("cancel event envelope has an invalid size")
    canceled = events[1]
    done = events[2]
    if not isinstance(canceled, CanceledEvent) or not isinstance(done, DoneEvent):
        raise MultiNativeProtocolError("cancel event envelope has invalid alternatives")
    if (
        done.reason != DoneReason.CANCELED
        or canceled.order_id != done.order_id
        or canceled.canceled_quantity != done.remaining_quantity
    ):
        raise MultiNativeProtocolError("cancel lifecycle payloads are inconsistent")
    if len(events) == 4:
        if not isinstance(events[3], BookChangedEvent):
            raise MultiNativeProtocolError("cancel envelope must end with book-changed")
        _validate_terminal_top(events[3])


def _validate_replace_envelope(events: tuple[Event, ...]) -> None:
    if len(events) < 5:
        raise MultiNativeProtocolError("replace event envelope is incomplete")
    replaced = events[1]
    canceled = events[2]
    old_done = events[3]
    if (
        not isinstance(replaced, ReplacedEvent)
        or not isinstance(canceled, CanceledEvent)
        or not isinstance(old_done, DoneEvent)
    ):
        raise MultiNativeProtocolError("replace lifecycle alternatives are invalid")
    if (
        old_done.reason != DoneReason.REPLACED
        or canceled.order_id != replaced.old_order_id
        or old_done.order_id != replaced.old_order_id
        or canceled.canceled_quantity != old_done.remaining_quantity
    ):
        raise MultiNativeProtocolError("replace old-order lifecycle payloads are inconsistent")

    trades, terminal, book_changed = _trade_terminal_suffix(events, 4)
    if terminal.order_id != replaced.new_order_id:
        raise MultiNativeProtocolError("replacement terminal event has the wrong new order ID")
    if isinstance(terminal, DoneEvent) and terminal.reason != DoneReason.FILLED:
        raise MultiNativeProtocolError("GTC replacement may terminate only as filled")
    _validate_trade_chain(trades, terminal)
    if trades and any(trade.aggressor_order_id != replaced.new_order_id for trade in trades):
        raise MultiNativeProtocolError("replacement trade has the wrong aggressor order ID")
    if book_changed is not None:
        _validate_terminal_top(book_changed)


def _trade_terminal_suffix(
    events: tuple[Event, ...],
    start: int,
) -> tuple[tuple[TradeEvent, ...], RestedEvent | DoneEvent, BookChangedEvent | None]:
    index = start
    trades: list[TradeEvent] = []
    while index < len(events) and isinstance(events[index], TradeEvent):
        trades.append(cast(TradeEvent, events[index]))
        index += 1
    if index == len(events) or not isinstance(events[index], (RestedEvent, DoneEvent)):
        raise MultiNativeProtocolError("event envelope is missing its aggressor terminal event")
    terminal = cast(RestedEvent | DoneEvent, events[index])
    index += 1
    book_changed: BookChangedEvent | None = None
    if index < len(events) and isinstance(events[index], BookChangedEvent):
        book_changed = cast(BookChangedEvent, events[index])
        index += 1
    if index != len(events):
        raise MultiNativeProtocolError("events appear after the terminal envelope")
    return tuple(trades), terminal, book_changed


def _validate_trade_chain(
    trades: tuple[TradeEvent, ...],
    terminal: RestedEvent | DoneEvent,
) -> None:
    if not trades:
        return
    first = trades[0]
    prior_remaining = first.execution_quantity + first.aggressor_remaining
    seen_resting_ids: set[int] = set()
    prior_price = first.execution_price
    for index, trade in enumerate(trades):
        if (
            trade.aggressor_order_id != first.aggressor_order_id
            or trade.aggressor_client_id != first.aggressor_client_id
            or trade.aggressor_side != first.aggressor_side
        ):
            raise MultiNativeProtocolError("trade aggressor identity changes within one command")
        if trade.resting_order_id in seen_resting_ids:
            raise MultiNativeProtocolError("one passive order appears in multiple trade events")
        seen_resting_ids.add(trade.resting_order_id)
        if trade.execution_quantity > prior_remaining:
            raise MultiNativeProtocolError("trade execution exceeds the aggressor remainder")
        if trade.aggressor_remaining != prior_remaining - trade.execution_quantity:
            raise MultiNativeProtocolError("trade aggressor quantities do not form a chain")
        if index != 0:
            if first.aggressor_side == Side.BUY and trade.execution_price < prior_price:
                raise MultiNativeProtocolError(
                    "buy trades are not in ascending passive-price order"
                )
            if first.aggressor_side == Side.SELL and trade.execution_price > prior_price:
                raise MultiNativeProtocolError(
                    "sell trades are not in descending passive-price order"
                )
        if trade.resting_remaining != 0 and (
            trade.aggressor_remaining != 0 or index != len(trades) - 1
        ):
            raise MultiNativeProtocolError("partially filled passive is not the final trade")
        prior_price = trade.execution_price
        prior_remaining = trade.aggressor_remaining

    if terminal.order_id != first.aggressor_order_id:
        raise MultiNativeProtocolError("aggressor terminal event has the wrong order ID")
    if terminal.remaining_quantity != prior_remaining:
        raise MultiNativeProtocolError("terminal quantity differs from the final trade remainder")
    if isinstance(terminal, RestedEvent) and (
        terminal.client_id != first.aggressor_client_id or terminal.side != first.aggressor_side
    ):
        raise MultiNativeProtocolError("rested aggressor identity differs from its trades")


def _validate_terminal_top(event: BookChangedEvent) -> None:
    if (
        event.best_bid is not None
        and event.best_ask is not None
        and event.best_bid.price >= event.best_ask.price
    ):
        raise MultiNativeProtocolError("terminal book-changed event is crossed")


def _validate_events_against_command(
    events: tuple[Event, ...],
    command: Command,
    outcome: MultiOutcome,
) -> None:
    if outcome == "rejected":
        return
    if isinstance(command, CancelOrder):
        canceled = cast(CanceledEvent, events[1])
        done = cast(DoneEvent, events[2])
        if canceled.order_id != command.order_id or done.order_id != command.order_id:
            raise MultiNativeProtocolError("cancel events use the wrong submitted order ID")
        return
    if isinstance(command, ReplaceOrder):
        replaced = cast(ReplacedEvent, events[1])
        if (
            replaced.old_order_id != command.old_order_id
            or replaced.new_order_id != command.new_order_id
        ):
            raise MultiNativeProtocolError("replace event lineage differs from the submitted IDs")
        trades, terminal, _ = _trade_terminal_suffix(events, 4)
        _validate_expected_aggressor(
            trades,
            terminal,
            command.new_order_id,
            command.client_id,
            command.new_quantity,
            command.new_limit_price,
            replacement=True,
        )
        return
    trades, terminal, _ = _trade_terminal_suffix(events, 1)
    _validate_expected_aggressor(
        trades,
        terminal,
        command.order_id,
        command.client_id,
        command.quantity,
        command.limit_price,
        side=command.side,
        order_type=command.order_type,
        time_in_force=command.time_in_force,
    )


def _validate_expected_aggressor(
    trades: tuple[TradeEvent, ...],
    terminal: RestedEvent | DoneEvent,
    order_id: int,
    client_id: int,
    initial_quantity: int,
    limit_price: int | None,
    *,
    side: int | None = None,
    order_type: int | None = None,
    time_in_force: int | None = None,
    replacement: bool = False,
) -> None:
    if terminal.order_id != order_id:
        raise MultiNativeProtocolError("terminal event uses the wrong submitted order ID")
    remaining = initial_quantity
    for trade in trades:
        if trade.aggressor_order_id != order_id or trade.aggressor_client_id != client_id:
            raise MultiNativeProtocolError("trade aggressor differs from the submitted command")
        if side is not None and trade.aggressor_side != side:
            raise MultiNativeProtocolError(
                "trade aggressor side differs from the submitted command"
            )
        if limit_price is not None:
            effective_side = side if side is not None else trade.aggressor_side
            if effective_side == Side.BUY and trade.execution_price > limit_price:
                raise MultiNativeProtocolError("buy trade executes beyond the submitted limit")
            if effective_side == Side.SELL and trade.execution_price < limit_price:
                raise MultiNativeProtocolError("sell trade executes beyond the submitted limit")
        if trade.execution_quantity > remaining:
            raise MultiNativeProtocolError("trade execution exceeds submitted quantity")
        remaining -= trade.execution_quantity
        if trade.aggressor_remaining != remaining:
            raise MultiNativeProtocolError("trade remainder differs from submitted quantity chain")
    if terminal.remaining_quantity != remaining:
        raise MultiNativeProtocolError("terminal remainder differs from submitted quantity chain")

    if isinstance(terminal, RestedEvent):
        if terminal.client_id != client_id:
            raise MultiNativeProtocolError("rested client differs from the submitted command")
        if side is not None and terminal.side != side:
            raise MultiNativeProtocolError("rested side differs from the submitted command")
        if limit_price is None or terminal.price != limit_price:
            raise MultiNativeProtocolError("rested price differs from the submitted command")
        if not replacement and (order_type != OrderType.LIMIT or time_in_force != TimeInForce.GTC):
            raise MultiNativeProtocolError("non-GTC-limit command emitted a rested event")
    elif remaining == 0:
        if terminal.reason != DoneReason.FILLED:
            raise MultiNativeProtocolError("zero residual did not terminate as filled")
    elif replacement:
        raise MultiNativeProtocolError("GTC replacement residual did not rest")
    elif order_type == OrderType.MARKET:
        if terminal.reason != DoneReason.MARKET_EXHAUSTED:
            raise MultiNativeProtocolError("market residual has the wrong terminal reason")
    elif time_in_force == TimeInForce.IOC:
        if terminal.reason != DoneReason.IOC_RESIDUAL_CANCELED:
            raise MultiNativeProtocolError("IOC residual has the wrong terminal reason")
    else:
        raise MultiNativeProtocolError("GTC limit residual did not rest")


def _validate_events_against_config(
    events: tuple[Event, ...],
    config: MatchingConfig,
) -> None:
    for event in events:
        prices: tuple[int, ...]
        quantities: tuple[int, ...]
        if isinstance(event, TradeEvent):
            prices = (event.execution_price,)
            quantities = (
                event.execution_quantity,
                event.aggressor_remaining,
                event.resting_remaining,
            )
        elif isinstance(event, RestedEvent):
            prices = (event.price,)
            quantities = (event.remaining_quantity,)
        elif isinstance(event, CanceledEvent):
            prices = ()
            quantities = (event.canceled_quantity,)
        elif isinstance(event, DoneEvent):
            prices = ()
            quantities = (event.remaining_quantity,)
        elif isinstance(event, BookChangedEvent):
            prices = tuple(
                level.price for level in (event.best_bid, event.best_ask) if level is not None
            )
            quantities = ()
        else:
            prices = ()
            quantities = ()
        if any(price % config.tick_increment != 0 for price in prices):
            raise MultiNativeProtocolError("event price is not aligned to the configured tick")
        if any(quantity > config.max_order_quantity for quantity in quantities):
            raise MultiNativeProtocolError("event order quantity exceeds the configured maximum")


def _validate_event_payload(event: Event) -> None:
    if isinstance(event, RejectedEvent):
        if event.order_id == 0:
            raise MultiNativeProtocolError("rejection carries an unassigned order ID")
    elif isinstance(event, TradeEvent):
        if (
            event.aggressor_order_id == 0
            or event.resting_order_id == 0
            or event.aggressor_client_id == 0
            or event.resting_client_id == 0
            or event.execution_price <= 0
            or event.execution_quantity == 0
        ):
            raise MultiNativeProtocolError("trade event is structurally invalid")
    elif isinstance(event, RestedEvent):
        if (
            event.order_id == 0
            or event.client_id == 0
            or event.price <= 0
            or event.remaining_quantity == 0
        ):
            raise MultiNativeProtocolError("rested event is structurally invalid")
    elif isinstance(event, CanceledEvent):
        if event.order_id == 0 or event.canceled_quantity == 0:
            raise MultiNativeProtocolError("canceled event is structurally invalid")
    elif isinstance(event, ReplacedEvent):
        if (
            event.old_order_id == 0
            or event.new_order_id == 0
            or event.old_order_id == event.new_order_id
        ):
            raise MultiNativeProtocolError("replaced event is structurally invalid")
    elif isinstance(event, DoneEvent):
        if event.order_id == 0:
            raise MultiNativeProtocolError("done event has an unassigned order ID")
        if (event.reason == DoneReason.FILLED) != (event.remaining_quantity == 0):
            raise MultiNativeProtocolError("done event remaining quantity is inconsistent")
    elif isinstance(event, BookChangedEvent):
        if (
            event.best_bid is not None
            and event.best_ask is not None
            and event.best_bid.price >= event.best_ask.price
        ):
            raise MultiNativeProtocolError("book-changed event is crossed")


def _parse_optional_top(value: object, name: str) -> TopOfBookLevel | None:
    if value is None:
        return None
    record = _object(value, name)
    _require_keys(record, {"price", "aggregate_quantity"})
    result = TopOfBookLevel(
        _decimal_int(_field(record, "price"), f"{name}.price", I64_MIN, I64_MAX),
        _decimal_uint(
            _field(record, "aggregate_quantity"),
            f"{name}.aggregate_quantity",
            U64_MAX,
        ),
    )
    if result.price <= 0 or result.aggregate_quantity == 0:
        raise MultiNativeProtocolError(f"{name} is not a positive top level")
    return result


def _validate_result_position(
    result: MultiNativeResultRecord,
    config: MultiNativeConfigRecord,
    expected_index: int,
    expected_commands: Sequence[Command] | None,
) -> None:
    if result.command_index != expected_index:
        raise MultiNativeProtocolError("result command indices are not contiguous")
    if result.line != len(config.input.catalog) + expected_index + 2:
        raise MultiNativeProtocolError("result source line is inconsistent")
    if expected_index >= config.command_count:
        raise MultiNativeProtocolError("native emitted too many results")
    if expected_commands is not None:
        if expected_index >= len(expected_commands):
            raise MultiNativeProtocolError("native emitted more results than commands")
        expected_command = expected_commands[expected_index]
        if result.command_type != command_type(expected_command):
            raise MultiNativeProtocolError("result command type differs from input command")
        if result.events is not None and any(
            event.header.instrument_id != expected_command.instrument_id for event in result.events
        ):
            raise MultiNativeProtocolError("event instrument differs from the submitted command")
        if result.events is not None:
            _validate_events_against_command(
                result.events,
                expected_command,
                result.outcome,
            )
            if result.outcome == "committed":
                instrument_config = next(
                    (
                        entry.matching
                        for entry in config.input.catalog
                        if entry.instrument_id == expected_command.instrument_id
                    ),
                    None,
                )
                if instrument_config is None:
                    raise MultiNativeProtocolError(
                        "committed event batch uses an unconfigured instrument"
                    )
                _validate_events_against_config(result.events, instrument_config)
    if result.outcome != "engine_error":
        expected_sequence = expected_index + 1
        if result.command_sequence != expected_sequence:
            raise MultiNativeProtocolError("domain result sequence is not contiguous")


def _validate_transcript(
    transcript: MultiNativeTranscript,
    *,
    expected_request: bool,
    expected_commands: Sequence[Command] | None,
    returncode: int | None,
) -> None:
    if returncode is not None and (isinstance(returncode, bool) or not isinstance(returncode, int)):
        raise MultiNativeProtocolError("native process return code is not an integer")
    if transcript.error is not None:
        if transcript.final is not None:
            raise MultiNativeProtocolError("error and final records both appear")
        _validate_error_terminal(
            transcript,
            expected_request=expected_request,
            expected_commands=expected_commands,
            returncode=returncode,
        )
        return
    if transcript.config is None or transcript.final is None:
        raise MultiNativeProtocolError("successful transcript lacks config or final record")
    config = transcript.config
    final = transcript.final
    if expected_commands is not None and config.command_count != len(expected_commands):
        raise MultiNativeProtocolError("echoed command count differs from requested commands")
    if final.commands_declared != config.command_count:
        raise MultiNativeProtocolError("final declared count differs from config")
    if final.commands_processed != len(transcript.results):
        raise MultiNativeProtocolError("final processed count differs from results")
    counts = {
        "committed": sum(result.outcome == "committed" for result in transcript.results),
        "rejected": sum(result.outcome == "rejected" for result in transcript.results),
        "engine_errors": sum(result.outcome == "engine_error" for result in transcript.results),
    }
    if (
        final.committed != counts["committed"]
        or final.rejected != counts["rejected"]
        or final.engine_errors != counts["engine_errors"]
    ):
        raise MultiNativeProtocolError("final classification counts differ from results")
    if transcript.results:
        if final.final_state_digest != transcript.results[-1].post_state_digest:
            raise MultiNativeProtocolError("final digest differs from last result")
    elif final.snapshot.last_sequence != 0:
        raise MultiNativeProtocolError("empty command stream advanced the sequence")
    expected_last_sequence = sum(result.outcome != "engine_error" for result in transcript.results)
    if final.snapshot.last_sequence != expected_last_sequence:
        raise MultiNativeProtocolError(
            "final snapshot sequence differs from processed domain commands"
        )
    if final.engine_errors == 0 and final.commands_processed != config.command_count:
        raise MultiNativeProtocolError("successful engine stopped before all commands")
    if final.engine_errors > 1:
        raise MultiNativeProtocolError("transcript contains multiple engine errors")
    if final.engine_errors and transcript.results[-1].outcome != "engine_error":
        raise MultiNativeProtocolError("engine error is not the terminal result")
    if returncode is not None:
        expected_returncode = _ENGINE_ERROR_EXIT_CODE if final.engine_errors else _SUCCESS_EXIT_CODE
        if returncode != expected_returncode:
            raise MultiNativeProtocolError("process status differs from terminal evidence")


def _validate_error_terminal(
    transcript: MultiNativeTranscript,
    *,
    expected_request: bool,
    expected_commands: Sequence[Command] | None,
    returncode: int | None,
) -> None:
    error = transcript.error
    if error is None:
        raise AssertionError("error-terminal validation requires an error")
    if transcript.results and transcript.results[-1].outcome == "engine_error":
        raise MultiNativeProtocolError("adapter error follows a terminal engine-error result")

    config = transcript.config
    if config is None:
        if transcript.results:
            raise MultiNativeProtocolError("result prefix appears without a config")
        if expected_request:
            if error.code == "invalid_engine_config":
                if error.line != 1:
                    raise MultiNativeProtocolError(
                        "engine-config error is not bound to the header line"
                    )
            elif error.code == "adapter_exception":
                if error.line != 0:
                    raise MultiNativeProtocolError("adapter exception has an invalid source line")
            elif error.code == "engine_construction_failure":
                if error.line != 1:
                    raise MultiNativeProtocolError(
                        "engine construction failure is not bound to the header"
                    )
            elif error.code == "resource_failure":
                if error.line not in (0, 1):
                    raise MultiNativeProtocolError(
                        "resource failure has an invalid pre-config source line"
                    )
            else:
                raise MultiNativeProtocolError(
                    "canonical typed input cannot produce this adapter error"
                )
    else:
        if error.code not in _REQUEST_BOUND_ERROR_CODES:
            raise MultiNativeProtocolError("accepted typed input cannot produce this adapter error")
        next_line = len(config.input.catalog) + len(transcript.results) + 2
        if error.line != next_line:
            raise MultiNativeProtocolError(
                "terminal adapter error is not at the next submitted line"
            )
        if len(transcript.results) >= config.command_count:
            raise MultiNativeProtocolError(
                "terminal adapter error appears after every submitted command"
            )

    if returncode is None:
        return
    if returncode == _INPUT_ERROR_EXIT_CODE:
        if error.code in _PROCESS_FAILURE_CODES:
            raise MultiNativeProtocolError("input-error exit carries an engine-failure code")
        return
    if returncode == _ENGINE_ERROR_EXIT_CODE:
        if error.code not in _PROCESS_FAILURE_CODES:
            raise MultiNativeProtocolError("engine-error exit carries an input-failure code")
        return
    raise MultiNativeProtocolError(
        f"native process returned an unsupported exit code: {returncode}"
    )


def _parse_json_line(line: str, output_line: int) -> dict[str, object]:
    try:
        value = json.loads(
            line,
            object_pairs_hook=_unique_object,
            parse_constant=_reject_json_constant,
        )
    except (TypeError, ValueError, json.JSONDecodeError) as exc:
        raise MultiNativeProtocolError(
            f"native output line {output_line} is not strict JSON"
        ) from exc
    record = _object(value, f"output line {output_line}")
    if _field(record, "schema") != _SCHEMA:
        raise MultiNativeProtocolError("native record has an unsupported schema")
    return record


def _unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    output: dict[str, object] = {}
    for key, value in pairs:
        if key in output:
            raise MultiNativeProtocolError(f"duplicate JSON key: {key}")
        output[key] = value
    return output


def _reject_json_constant(value: str) -> NoReturn:
    raise MultiNativeProtocolError(f"non-finite JSON constant: {value}")


def _require_keys(record: Mapping[str, object], expected: set[str]) -> None:
    if set(record) != expected:
        raise MultiNativeProtocolError("native record has unexpected fields")


def _field(record: Mapping[str, object], name: str) -> object:
    try:
        return record[name]
    except KeyError as exc:
        raise MultiNativeProtocolError(f"native record is missing {name}") from exc


def _object(value: object, name: str) -> dict[str, object]:
    if not isinstance(value, dict) or any(not isinstance(key, str) for key in value):
        raise MultiNativeProtocolError(f"{name} is not an object")
    return cast(dict[str, object], value)


def _array(value: object, name: str) -> list[object]:
    if not isinstance(value, list):
        raise MultiNativeProtocolError(f"{name} is not an array")
    return cast(list[object], value)


def _string(value: object, name: str) -> str:
    if not isinstance(value, str):
        raise MultiNativeProtocolError(f"{name} is not a string")
    return value


def _boolean(value: object, name: str) -> bool:
    if not isinstance(value, bool):
        raise MultiNativeProtocolError(f"{name} is not a boolean")
    return value


def _decimal_uint(value: object, name: str, maximum: int) -> int:
    text = _string(value, name)
    if _UNSIGNED_DECIMAL.fullmatch(text) is None:
        raise MultiNativeProtocolError(f"{name} is not canonical unsigned decimal")
    parsed = int(text)
    if parsed > maximum:
        raise MultiNativeProtocolError(f"{name} exceeds its representation")
    return parsed


def _optional_decimal_uint(
    value: object,
    name: str,
    maximum: int,
) -> int | None:
    return None if value is None else _decimal_uint(value, name, maximum)


def _decimal_int(value: object, name: str, minimum: int, maximum: int) -> int:
    text = _string(value, name)
    if _SIGNED_DECIMAL.fullmatch(text) is None:
        raise MultiNativeProtocolError(f"{name} is not canonical signed decimal")
    parsed = int(text)
    if not minimum <= parsed <= maximum:
        raise MultiNativeProtocolError(f"{name} exceeds its representation")
    return parsed


def _digest_value(value: object, name: str) -> str:
    text = _string(value, name)
    if _DIGEST.fullmatch(text) is None:
        raise MultiNativeProtocolError(f"{name} is not lowercase SHA-256 hex")
    return text


def _optional_digest(value: object, name: str) -> str | None:
    return None if value is None else _digest_value(value, name)


def _name(
    value: object,
    name: str,
    vocabulary: Mapping[str, ValueT],
) -> ValueT:
    text = _string(value, name)
    try:
        return vocabulary[text]
    except KeyError as exc:
        raise MultiNativeProtocolError(f"{name} is outside its vocabulary") from exc


def _optional_name(
    value: object,
    name: str,
    vocabulary: Mapping[str, ValueT],
) -> ValueT | None:
    return None if value is None else _name(value, name, vocabulary)


def _require_representable(command: Command) -> None:
    if isinstance(command, NewOrder):
        _require_uint("client_id", command.client_id, U32_MAX)
        _require_uint("order_id", command.order_id, U64_MAX)
        _require_uint("instrument_id", command.instrument_id, U32_MAX)
        _require_uint("side", command.side, U8_MAX)
        _require_uint("order_type", command.order_type, U8_MAX)
        _require_uint("time_in_force", command.time_in_force, U8_MAX)
        if command.limit_price is not None:
            _require_int("limit_price", command.limit_price, I64_MIN, I64_MAX)
        _require_uint("quantity", command.quantity, U64_MAX)
    elif isinstance(command, CancelOrder):
        _require_uint("client_id", command.client_id, U32_MAX)
        _require_uint("order_id", command.order_id, U64_MAX)
        _require_uint("instrument_id", command.instrument_id, U32_MAX)
    elif isinstance(command, ReplaceOrder):
        _require_uint("client_id", command.client_id, U32_MAX)
        _require_uint("old_order_id", command.old_order_id, U64_MAX)
        _require_uint("new_order_id", command.new_order_id, U64_MAX)
        _require_uint("instrument_id", command.instrument_id, U32_MAX)
        _require_int("new_limit_price", command.new_limit_price, I64_MIN, I64_MAX)
        _require_uint("new_quantity", command.new_quantity, U64_MAX)
    else:
        raise TypeError(f"unsupported command type: {type(command)!r}")


def _require_uint(name: str, value: int, maximum: int) -> None:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
        raise ValueError(f"{name} is outside its unsigned representation")


def _require_int(name: str, value: int, minimum: int, maximum: int) -> None:
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise ValueError(f"{name} is outside its signed representation")
