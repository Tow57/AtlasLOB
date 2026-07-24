"""Typed process boundary for the test-only ``atlas_diff_native`` adapter."""

from __future__ import annotations

import json
import re
import subprocess
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Literal, NoReturn, TypeVar, cast

from atlaslob.canonical import event_digest, state_digest
from atlaslob.domain import (
    ATLASLOB_SEMANTICS_VERSION,
    I64_MAX,
    I64_MIN,
    U32_MAX,
    U64_MAX,
    AcceptedEvent,
    BookChangedEvent,
    BookSnapshot,
    CanceledEvent,
    CancelOrder,
    Command,
    CommandType,
    DoneEvent,
    DoneReason,
    EngineError,
    Event,
    EventBatch,
    EventHeader,
    MatchingConfig,
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

_SCHEMA = "atlas_diff_v1"
_HEADER = "ATLAS_DIFF_V1"
_UNSIGNED_DECIMAL = re.compile(r"(?:0|[1-9][0-9]*)\Z")
_SIGNED_DECIMAL = re.compile(r"(?:0|-[1-9][0-9]*|[1-9][0-9]*)\Z")
_DIGEST = re.compile(r"[0-9a-f]{64}\Z")
_SUCCESS_EXIT_CODE = 0
_INPUT_ERROR_EXIT_CODE = 2
_ENGINE_ERROR_EXIT_CODE = 3
_REQUEST_BOUND_ERROR_CODES = frozenset(
    {
        "adapter_exception",
        "engine_exception",
        "input_read_failure",
        "resource_failure",
    }
)
_ERROR_CODES = frozenset(
    {
        "adapter_exception",
        "empty_command",
        "engine_construction_failure",
        "engine_exception",
        "input_read_failure",
        "invalid_cancel_client",
        "invalid_cancel_field_count",
        "invalid_cancel_instrument",
        "invalid_cancel_order",
        "invalid_engine_config",
        "invalid_header_field_count",
        "invalid_header_instrument",
        "invalid_header_max_active_orders",
        "invalid_header_max_quantity",
        "invalid_header_snapshot_interval",
        "invalid_header_tick_increment",
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
        "missing_header",
        "nonzero_absent_price_placeholder",
        "resource_failure",
        "unknown_command",
        "unsupported_header",
    }
)

OutputMode = Literal["exact", "compact"]
_EnumValue = TypeVar("_EnumValue")


class NativeProtocolError(ValueError):
    """Raised when native output does not satisfy the versioned evidence schema."""


@dataclass(frozen=True, slots=True)
class NativeInputConfig:
    instrument_id: int
    engine: MatchingConfig = MatchingConfig()
    snapshot_interval: int = 0

    def __post_init__(self) -> None:
        if (
            isinstance(self.instrument_id, bool)
            or not isinstance(self.instrument_id, int)
            or not 1 <= self.instrument_id <= U32_MAX
        ):
            raise ValueError("instrument_id must be a nonzero u32")
        if (
            isinstance(self.snapshot_interval, bool)
            or not isinstance(self.snapshot_interval, int)
            or not 0 <= self.snapshot_interval <= U64_MAX
        ):
            raise ValueError("snapshot_interval must be a u64")


@dataclass(frozen=True, slots=True)
class NativeConfigRecord:
    mode: OutputMode
    semantics_version: int
    input: NativeInputConfig


@dataclass(frozen=True, slots=True)
class NativeState:
    active_order_count: int
    empty: bool
    next_sequence: int
    sequence_exhausted: bool
    best_bid: TopOfBookLevel | None
    best_ask: TopOfBookLevel | None
    state_digest: str


@dataclass(frozen=True, slots=True)
class NativeResultRecord:
    command_index: int
    line: int
    command_type: CommandType
    outcome: Literal["committed", "rejected", "engine_error"]
    command_sequence: int | None
    engine_error: EngineError | None
    event_digest: str | None
    events: tuple[Event, ...] | None
    state: NativeState
    snapshot: BookSnapshot | None


@dataclass(frozen=True, slots=True)
class NativeFinalRecord:
    commands_processed: int
    state: NativeState
    snapshot: BookSnapshot


@dataclass(frozen=True, slots=True)
class NativeErrorRecord:
    line: int
    code: str


@dataclass(frozen=True, slots=True)
class NativeTranscript:
    config: NativeConfigRecord | None
    results: tuple[NativeResultRecord, ...]
    final: NativeFinalRecord | None
    error: NativeErrorRecord | None


@dataclass(frozen=True, slots=True)
class NativeRun:
    returncode: int
    transcript: NativeTranscript
    stdout: str
    stderr: str


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


def encode_stream(config: NativeInputConfig, commands: Sequence[Command]) -> str:
    """Encode already-typed commands into the lossless numeric V1 input grammar."""

    lines = [
        " ".join(
            (
                _HEADER,
                str(config.instrument_id),
                str(config.engine.max_order_quantity),
                str(config.engine.tick_increment),
                str(config.engine.max_active_orders),
                str(config.snapshot_interval),
            )
        )
    ]
    for command in commands:
        _require_representable(command)
        if isinstance(command, NewOrder):
            present = int(command.limit_price is not None)
            price = command.limit_price if command.limit_price is not None else 0
            lines.append(
                " ".join(
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
            )
        elif isinstance(command, CancelOrder):
            lines.append(f"C {command.client_id} {command.order_id} {command.instrument_id}")
        elif isinstance(command, ReplaceOrder):
            lines.append(
                " ".join(
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
            )
        else:
            raise TypeError(f"unsupported command type: {type(command)!r}")
    return "\n".join(lines) + "\n"


def run_native(
    executable: str | Path,
    config: NativeInputConfig,
    commands: Sequence[Command],
    *,
    mode: OutputMode = "exact",
    timeout: float = 30.0,
) -> NativeRun:
    """Execute the native adapter without a shell and decode its complete stdout."""

    if mode not in ("exact", "compact"):
        raise ValueError(f"unsupported output mode: {mode}")
    try:
        completed = subprocess.run(
            [str(Path(executable)), mode],
            input=encode_stream(config, commands),
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
    except UnicodeDecodeError as exc:
        raise NativeProtocolError("native process output is not valid text") from exc
    if completed.stderr:
        raise NativeProtocolError("native process wrote unexpected standard-error output")
    transcript = decode_jsonl(
        completed.stdout,
        expected_mode=mode,
        expected_input=config,
        expected_commands=commands,
        returncode=completed.returncode,
    )
    return NativeRun(
        returncode=completed.returncode,
        transcript=transcript,
        stdout=completed.stdout,
        stderr=completed.stderr,
    )


def decode_jsonl(
    text: str,
    *,
    expected_mode: OutputMode | None = None,
    expected_input: NativeInputConfig | None = None,
    expected_commands: Sequence[Command] | None = None,
    returncode: int | None = None,
) -> NativeTranscript:
    """Strictly decode and cross-check a complete native JSONL transcript."""

    raw_lines = text.splitlines()
    if not raw_lines:
        raise NativeProtocolError("native output is empty")

    config: NativeConfigRecord | None = None
    results: list[NativeResultRecord] = []
    final: NativeFinalRecord | None = None
    error: NativeErrorRecord | None = None

    for offset, line in enumerate(raw_lines, start=1):
        if not line:
            raise NativeProtocolError(f"output line {offset} is blank")
        try:
            value = cast(
                object,
                json.loads(
                    line,
                    object_pairs_hook=_unique_json_object,
                    parse_constant=_reject_json_constant,
                ),
            )
        except NativeProtocolError:
            raise
        except (ValueError, OverflowError, RecursionError) as exc:
            raise NativeProtocolError(f"output line {offset} is not valid JSON") from exc
        record = _object(value, f"output line {offset}")
        _require_schema(record)
        kind = _string(_field(record, "kind"), "kind")

        if final is not None or error is not None:
            raise NativeProtocolError("records appear after a terminal record")
        if kind == "config":
            if config is not None or results:
                raise NativeProtocolError("config record is not first")
            config = _parse_config(record)
        elif kind == "result":
            if config is None:
                raise NativeProtocolError("result appears before config")
            result = _parse_result(record)
            if result.command_index != len(results):
                raise NativeProtocolError("result command indices are not contiguous")
            if results and result.line <= results[-1].line:
                raise NativeProtocolError("result source lines are not increasing")
            _validate_result_mode(result, config.mode)
            results.append(result)
        elif kind == "final":
            if config is None:
                raise NativeProtocolError("final record appears before config")
            final = _parse_final(record)
            if final.commands_processed != len(results):
                raise NativeProtocolError("final command count does not match result records")
        elif kind == "error":
            error = _parse_error(record)
        else:
            raise NativeProtocolError(f"unknown record kind: {kind}")

    if final is None and error is None:
        raise NativeProtocolError("transcript has no terminal record")
    if final is not None and error is not None:
        raise NativeProtocolError("transcript has two terminal records")
    transcript = NativeTranscript(config, tuple(results), final, error)
    _validate_transcript(
        transcript,
        expected_mode=expected_mode,
        expected_input=expected_input,
        expected_commands=expected_commands,
        returncode=returncode,
    )
    return transcript


def _parse_config(record: dict[str, object]) -> NativeConfigRecord:
    _require_keys(
        record,
        {
            "schema",
            "kind",
            "mode",
            "semantics_version",
            "instrument_id",
            "max_order_quantity",
            "tick_increment",
            "max_active_orders",
            "snapshot_interval",
        },
    )
    mode_value = _string(_field(record, "mode"), "mode")
    if mode_value not in ("exact", "compact"):
        raise NativeProtocolError(f"invalid mode: {mode_value}")
    mode = cast(OutputMode, mode_value)
    semantics = _json_uint(_field(record, "semantics_version"), "semantics_version", (1 << 16) - 1)
    if semantics != ATLASLOB_SEMANTICS_VERSION:
        raise NativeProtocolError(f"unsupported semantics version: {semantics}")
    try:
        input_config = NativeInputConfig(
            instrument_id=_decimal_uint(_field(record, "instrument_id"), "instrument_id", U32_MAX),
            engine=MatchingConfig(
                max_order_quantity=_decimal_uint(
                    _field(record, "max_order_quantity"), "max_order_quantity", U64_MAX
                ),
                tick_increment=_decimal_int(
                    _field(record, "tick_increment"), "tick_increment", I64_MIN, I64_MAX
                ),
                max_active_orders=_decimal_uint(
                    _field(record, "max_active_orders"), "max_active_orders", U64_MAX
                ),
            ),
            snapshot_interval=_decimal_uint(
                _field(record, "snapshot_interval"), "snapshot_interval", U64_MAX
            ),
        )
    except ValueError as exc:
        raise NativeProtocolError("native config is outside the supported domain") from exc
    return NativeConfigRecord(mode, semantics, input_config)


def _parse_result(record: dict[str, object]) -> NativeResultRecord:
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
            "event_digest",
            "events",
            "state",
            "snapshot",
        },
    )
    command_type = _enum_name(_field(record, "command_type"), "command_type", _COMMAND_NAMES)
    outcome_value = _string(_field(record, "outcome"), "outcome")
    if outcome_value not in ("committed", "rejected", "engine_error"):
        raise NativeProtocolError(f"invalid result outcome: {outcome_value}")
    outcome = cast(Literal["committed", "rejected", "engine_error"], outcome_value)
    sequence = _optional_decimal_uint(
        _field(record, "command_sequence"), "command_sequence", U64_MAX
    )
    engine_error = _optional_enum_name(
        _field(record, "engine_error"), "engine_error", _ENGINE_ERRORS
    )
    digest = _optional_digest(_field(record, "event_digest"), "event_digest")
    raw_events = _field(record, "events")
    events = None if raw_events is None else _parse_events(raw_events)
    state = _parse_state(_field(record, "state"))
    raw_snapshot = _field(record, "snapshot")
    snapshot = None if raw_snapshot is None else _parse_snapshot(raw_snapshot)

    if outcome == "engine_error":
        if sequence is not None or engine_error is None or digest is not None or events is not None:
            raise NativeProtocolError("engine-error result has inconsistent nullable fields")
    else:
        if sequence is None or engine_error is not None or digest is None:
            raise NativeProtocolError("domain result has inconsistent nullable fields")
        if events is not None:
            try:
                batch = EventBatch(events)
            except ValueError as exc:
                raise NativeProtocolError("event batch is structurally invalid") from exc
            if batch.command_sequence != sequence:
                raise NativeProtocolError("event sequence differs from result sequence")
            _validate_event_envelope(events, outcome, command_type)
            if event_digest(batch) != digest:
                raise NativeProtocolError("event digest differs from canonical event values")
    if snapshot is not None:
        _validate_state_snapshot(state, snapshot)
    if events is not None and isinstance(events[-1], BookChangedEvent):
        if events[-1].best_bid != state.best_bid or events[-1].best_ask != state.best_ask:
            raise NativeProtocolError("book-changed payload differs from post-command state")

    return NativeResultRecord(
        command_index=_decimal_uint(_field(record, "command_index"), "command_index", U64_MAX),
        line=_decimal_uint(_field(record, "line"), "line", U64_MAX),
        command_type=command_type,
        outcome=outcome,
        command_sequence=sequence,
        engine_error=engine_error,
        event_digest=digest,
        events=events,
        state=state,
        snapshot=snapshot,
    )


def _parse_final(record: dict[str, object]) -> NativeFinalRecord:
    _require_keys(
        record,
        {"schema", "kind", "commands_processed", "state", "snapshot"},
    )
    state = _parse_state(_field(record, "state"))
    snapshot = _parse_snapshot(_field(record, "snapshot"))
    _validate_state_snapshot(state, snapshot)
    return NativeFinalRecord(
        commands_processed=_decimal_uint(
            _field(record, "commands_processed"), "commands_processed", U64_MAX
        ),
        state=state,
        snapshot=snapshot,
    )


def _parse_error(record: dict[str, object]) -> NativeErrorRecord:
    _require_keys(record, {"schema", "kind", "line", "code"})
    code = _string(_field(record, "code"), "code")
    if code not in _ERROR_CODES:
        raise NativeProtocolError("error code is outside the closed adapter vocabulary")
    return NativeErrorRecord(
        line=_decimal_uint(_field(record, "line"), "line", U64_MAX),
        code=code,
    )


def _parse_state(value: object) -> NativeState:
    record = _object(value, "state")
    _require_keys(
        record,
        {
            "active_order_count",
            "empty",
            "next_sequence",
            "sequence_exhausted",
            "best_bid",
            "best_ask",
            "state_digest",
        },
    )
    state = NativeState(
        active_order_count=_decimal_uint(
            _field(record, "active_order_count"), "active_order_count", U64_MAX
        ),
        empty=_boolean(_field(record, "empty"), "empty"),
        next_sequence=_decimal_uint(_field(record, "next_sequence"), "next_sequence", U64_MAX),
        sequence_exhausted=_boolean(_field(record, "sequence_exhausted"), "sequence_exhausted"),
        best_bid=_parse_optional_top(_field(record, "best_bid"), "best_bid"),
        best_ask=_parse_optional_top(_field(record, "best_ask"), "best_ask"),
        state_digest=_digest_value(_field(record, "state_digest"), "state_digest"),
    )
    _validate_state_structure(state)
    return state


def _parse_snapshot(value: object) -> BookSnapshot:
    record = _object(value, "snapshot")
    _require_keys(
        record,
        {
            "semantics_version",
            "instrument_id",
            "last_sequence",
            "sequence_exhausted",
            "active_order_count",
            "bids",
            "asks",
        },
    )
    semantics = _json_uint(_field(record, "semantics_version"), "semantics_version", (1 << 16) - 1)
    if semantics != ATLASLOB_SEMANTICS_VERSION:
        raise NativeProtocolError(f"unsupported snapshot semantics version: {semantics}")
    instrument_id = _decimal_uint(_field(record, "instrument_id"), "instrument_id", U32_MAX)
    if instrument_id == 0:
        raise NativeProtocolError("snapshot instrument is unassigned")
    snapshot = BookSnapshot(
        semantics_version=semantics,
        instrument_id=instrument_id,
        last_sequence=_decimal_uint(_field(record, "last_sequence"), "last_sequence", U64_MAX),
        sequence_exhausted=_boolean(_field(record, "sequence_exhausted"), "sequence_exhausted"),
        active_order_count=_decimal_uint(
            _field(record, "active_order_count"), "active_order_count", U64_MAX
        ),
        bids=_parse_levels(_field(record, "bids"), Side.BUY, instrument_id),
        asks=_parse_levels(_field(record, "asks"), Side.SELL, instrument_id),
    )
    _validate_snapshot_structure(snapshot)
    return snapshot


def _parse_levels(
    value: object,
    side: Side,
    snapshot_instrument_id: int,
) -> tuple[PriceLevelSnapshot, ...]:
    output: list[PriceLevelSnapshot] = []
    for level_index, raw_level in enumerate(_array(value, "levels")):
        level = _object(raw_level, f"level[{level_index}]")
        _require_keys(level, {"price", "aggregate_quantity", "orders"})
        price = _decimal_int(_field(level, "price"), "level.price", I64_MIN, I64_MAX)
        if price <= 0:
            raise NativeProtocolError("snapshot level price must be positive")
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
            order_side = _enum_name(_field(order, "side"), "order.side", _SIDE_NAMES)
            if order_side != side:
                raise NativeProtocolError("snapshot order side differs from its collection")
            order_price = _decimal_int(_field(order, "price"), "order.price", I64_MIN, I64_MAX)
            if order_price != price:
                raise NativeProtocolError("snapshot order price differs from its level")
            order_id = _decimal_uint(_field(order, "order_id"), "order.order_id", U64_MAX)
            client_id = _decimal_uint(_field(order, "client_id"), "order.client_id", U32_MAX)
            instrument_id = _decimal_uint(
                _field(order, "instrument_id"), "order.instrument_id", U32_MAX
            )
            remaining_quantity = _decimal_uint(
                _field(order, "remaining_quantity"),
                "order.remaining_quantity",
                U64_MAX,
            )
            priority_sequence = _decimal_uint(
                _field(order, "priority_sequence"),
                "order.priority_sequence",
                U64_MAX,
            )
            if order_id == 0 or client_id == 0:
                raise NativeProtocolError("snapshot order identity is unassigned")
            if instrument_id != snapshot_instrument_id:
                raise NativeProtocolError("snapshot order instrument differs from its book")
            if remaining_quantity == 0:
                raise NativeProtocolError("snapshot order has zero remaining quantity")
            if priority_sequence == 0:
                raise NativeProtocolError("snapshot order priority is unassigned")
            if orders and orders[-1].priority_sequence >= priority_sequence:
                raise NativeProtocolError("snapshot FIFO priority is not strictly increasing")
            orders.append(
                OrderSnapshot(
                    order_id=order_id,
                    client_id=client_id,
                    instrument_id=instrument_id,
                    side=order_side,
                    price=order_price,
                    remaining_quantity=remaining_quantity,
                    priority_sequence=priority_sequence,
                )
            )
        if not orders:
            raise NativeProtocolError("snapshot contains an empty price level")
        aggregate = _decimal_uint(
            _field(level, "aggregate_quantity"), "level.aggregate_quantity", U64_MAX
        )
        if sum(order.remaining_quantity for order in orders) != aggregate:
            raise NativeProtocolError("snapshot level aggregate differs from its orders")
        output.append(PriceLevelSnapshot(price, aggregate, tuple(orders)))

    prices = [level.price for level in output]
    expected = sorted(prices, reverse=side == Side.BUY)
    if prices != expected or len(prices) != len(set(prices)):
        raise NativeProtocolError("snapshot levels are not unique best-price-first values")
    return tuple(output)


def _validate_snapshot_structure(snapshot: BookSnapshot) -> None:
    if snapshot.sequence_exhausted:
        if snapshot.last_sequence != U64_MAX:
            raise NativeProtocolError("exhausted snapshot did not issue the maximum sequence")
    elif snapshot.last_sequence == U64_MAX:
        raise NativeProtocolError("maximum-sequence snapshot is not marked exhausted")

    orders = tuple(order for level in (*snapshot.bids, *snapshot.asks) for order in level.orders)
    if len(orders) != snapshot.active_order_count:
        raise NativeProtocolError("snapshot active count differs from its orders")

    order_ids = [order.order_id for order in orders]
    if len(order_ids) != len(set(order_ids)):
        raise NativeProtocolError("snapshot contains duplicate active order IDs")

    priorities = [order.priority_sequence for order in orders]
    if len(priorities) != len(set(priorities)):
        raise NativeProtocolError("snapshot contains duplicate active priorities")
    if any(priority > snapshot.last_sequence for priority in priorities):
        raise NativeProtocolError("snapshot order priority exceeds the last sequence")

    if snapshot.bids and snapshot.asks and snapshot.bids[0].price >= snapshot.asks[0].price:
        raise NativeProtocolError("snapshot book is crossed")


def _parse_events(value: object) -> tuple[Event, ...]:
    events: list[Event] = []
    for index, raw_event in enumerate(_array(value, "events")):
        record = _object(raw_event, f"event[{index}]")
        type_name = _string(_field(record, "type"), "event.type")
        header = EventHeader(
            command_sequence=_decimal_uint(
                _field(record, "command_sequence"), "event.command_sequence", U64_MAX
            ),
            event_index=_json_uint(_field(record, "event_index"), "event.event_index", U32_MAX),
            instrument_id=_decimal_uint(
                _field(record, "instrument_id"), "event.instrument_id", U32_MAX
            ),
        )
        base = {"type", "command_sequence", "event_index", "instrument_id"}
        if type_name == "accepted":
            _require_keys(record, base | {"command_type"})
            event: Event = AcceptedEvent(
                header,
                _enum_name(_field(record, "command_type"), "command_type", _COMMAND_NAMES),
            )
        elif type_name == "rejected":
            _require_keys(record, base | {"command_type", "reason", "order_id"})
            event = RejectedEvent(
                header,
                _enum_name(_field(record, "command_type"), "command_type", _COMMAND_NAMES),
                _enum_name(_field(record, "reason"), "reason", _REJECT_NAMES),
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
                _decimal_uint(_field(record, "aggressor_order_id"), "aggressor_order_id", U64_MAX),
                _decimal_uint(_field(record, "resting_order_id"), "resting_order_id", U64_MAX),
                _decimal_uint(
                    _field(record, "aggressor_client_id"), "aggressor_client_id", U32_MAX
                ),
                _decimal_uint(_field(record, "resting_client_id"), "resting_client_id", U32_MAX),
                _enum_name(_field(record, "aggressor_side"), "aggressor_side", _SIDE_NAMES),
                _decimal_int(
                    _field(record, "execution_price"), "execution_price", I64_MIN, I64_MAX
                ),
                _decimal_uint(_field(record, "execution_quantity"), "execution_quantity", U64_MAX),
                _decimal_uint(
                    _field(record, "aggressor_remaining"), "aggressor_remaining", U64_MAX
                ),
                _decimal_uint(_field(record, "resting_remaining"), "resting_remaining", U64_MAX),
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
                _enum_name(_field(record, "side"), "side", _SIDE_NAMES),
                _decimal_int(_field(record, "price"), "price", I64_MIN, I64_MAX),
                _decimal_uint(_field(record, "remaining_quantity"), "remaining_quantity", U64_MAX),
            )
        elif type_name == "canceled":
            _require_keys(record, base | {"order_id", "canceled_quantity"})
            event = CanceledEvent(
                header,
                _decimal_uint(_field(record, "order_id"), "order_id", U64_MAX),
                _decimal_uint(_field(record, "canceled_quantity"), "canceled_quantity", U64_MAX),
            )
        elif type_name == "replaced":
            _require_keys(record, base | {"old_order_id", "new_order_id"})
            event = ReplacedEvent(
                header,
                _decimal_uint(_field(record, "old_order_id"), "old_order_id", U64_MAX),
                _decimal_uint(_field(record, "new_order_id"), "new_order_id", U64_MAX),
            )
        elif type_name == "done":
            _require_keys(record, base | {"order_id", "reason", "remaining_quantity"})
            event = DoneEvent(
                header,
                _decimal_uint(_field(record, "order_id"), "order_id", U64_MAX),
                _enum_name(_field(record, "reason"), "reason", _DONE_NAMES),
                _decimal_uint(_field(record, "remaining_quantity"), "remaining_quantity", U64_MAX),
            )
        elif type_name == "book_changed":
            _require_keys(record, base | {"best_bid", "best_ask"})
            event = BookChangedEvent(
                header,
                _parse_optional_top(_field(record, "best_bid"), "best_bid"),
                _parse_optional_top(_field(record, "best_ask"), "best_ask"),
            )
        else:
            raise NativeProtocolError(f"unknown event type: {type_name}")
        _validate_event_payload(event)
        events.append(event)
    if not events:
        raise NativeProtocolError("exact event list is empty")
    return tuple(events)


def _parse_optional_top(value: object, name: str) -> TopOfBookLevel | None:
    if value is None:
        return None
    record = _object(value, name)
    _require_keys(record, {"price", "aggregate_quantity"})
    result = TopOfBookLevel(
        _decimal_int(_field(record, "price"), f"{name}.price", I64_MIN, I64_MAX),
        _decimal_uint(_field(record, "aggregate_quantity"), f"{name}.aggregate_quantity", U64_MAX),
    )
    if result.price <= 0 or result.aggregate_quantity == 0:
        raise NativeProtocolError(f"{name} must have positive price and quantity")
    return result


def _validate_event_payload(event: Event) -> None:
    if isinstance(event, RejectedEvent):
        if event.order_id is not None and event.order_id == 0:
            raise NativeProtocolError("rejected event carries an unassigned order ID")
        return
    if isinstance(event, TradeEvent):
        if (
            event.aggressor_order_id == 0
            or event.resting_order_id == 0
            or event.aggressor_client_id == 0
            or event.resting_client_id == 0
        ):
            raise NativeProtocolError("trade event carries unassigned identity")
        if event.execution_price <= 0 or event.execution_quantity == 0:
            raise NativeProtocolError("trade event price and execution quantity must be positive")
        return
    if isinstance(event, RestedEvent):
        if event.order_id == 0 or event.client_id == 0:
            raise NativeProtocolError("rested event carries unassigned identity")
        if event.price <= 0 or event.remaining_quantity == 0:
            raise NativeProtocolError("rested event price and quantity must be positive")
        return
    if isinstance(event, CanceledEvent):
        if event.order_id == 0 or event.canceled_quantity == 0:
            raise NativeProtocolError("canceled event identity and quantity must be positive")
        return
    if isinstance(event, ReplacedEvent):
        if (
            event.old_order_id == 0
            or event.new_order_id == 0
            or event.old_order_id == event.new_order_id
        ):
            raise NativeProtocolError("replaced event requires distinct assigned order IDs")
        return
    if isinstance(event, DoneEvent):
        if event.order_id == 0:
            raise NativeProtocolError("done event carries an unassigned order ID")
        if event.reason == DoneReason.FILLED:
            if event.remaining_quantity != 0:
                raise NativeProtocolError("filled done event has nonzero remaining quantity")
        elif event.remaining_quantity == 0:
            raise NativeProtocolError("non-filled done event has zero remaining quantity")
        return
    if isinstance(event, BookChangedEvent):
        _validate_top_pair(event.best_bid, event.best_ask, "book-changed event")


def _validate_event_envelope(
    events: tuple[Event, ...],
    outcome: Literal["committed", "rejected", "engine_error"],
    result_command_type: CommandType,
) -> None:
    if outcome == "rejected":
        if len(events) != 1 or not isinstance(events[0], RejectedEvent):
            raise NativeProtocolError("rejected result must contain exactly one rejection")
        if events[0].command_type != result_command_type:
            raise NativeProtocolError("rejection command type differs from result command type")
        return
    if outcome != "committed":
        raise NativeProtocolError("engine-error result unexpectedly contains events")
    if not isinstance(events[0], AcceptedEvent):
        raise NativeProtocolError("committed result must begin with an accepted event")
    if events[0].command_type != result_command_type:
        raise NativeProtocolError("acceptance command type differs from result command type")

    if result_command_type == CommandType.CANCEL:
        _validate_cancel_envelope(events)
        return
    if result_command_type == CommandType.REPLACE:
        _validate_replace_envelope(events)
        return
    _validate_new_envelope(events)


def _validate_new_envelope(events: tuple[Event, ...]) -> None:
    trades, terminal, book_changed = _trade_terminal_suffix(events, 1)
    _validate_trade_chain(trades, terminal)
    if isinstance(terminal, DoneEvent) and terminal.reason not in (
        DoneReason.FILLED,
        DoneReason.IOC_RESIDUAL_CANCELED,
        DoneReason.MARKET_EXHAUSTED,
    ):
        raise NativeProtocolError("new-order done event has an invalid terminal reason")
    if book_changed is not None:
        _validate_terminal_top(book_changed)


def _validate_cancel_envelope(events: tuple[Event, ...]) -> None:
    if len(events) not in (3, 4):
        raise NativeProtocolError("cancel event envelope has an invalid size")
    canceled = events[1]
    done = events[2]
    if not isinstance(canceled, CanceledEvent) or not isinstance(done, DoneEvent):
        raise NativeProtocolError("cancel event envelope has invalid alternatives")
    if (
        done.reason != DoneReason.CANCELED
        or canceled.order_id != done.order_id
        or canceled.canceled_quantity != done.remaining_quantity
    ):
        raise NativeProtocolError("cancel lifecycle payloads are inconsistent")
    if len(events) == 4:
        if not isinstance(events[3], BookChangedEvent):
            raise NativeProtocolError("cancel envelope must end with book-changed")
        _validate_terminal_top(events[3])


def _validate_replace_envelope(events: tuple[Event, ...]) -> None:
    if len(events) < 5:
        raise NativeProtocolError("replace event envelope is incomplete")
    replaced = events[1]
    canceled = events[2]
    old_done = events[3]
    if (
        not isinstance(replaced, ReplacedEvent)
        or not isinstance(canceled, CanceledEvent)
        or not isinstance(old_done, DoneEvent)
    ):
        raise NativeProtocolError("replace lifecycle alternatives are invalid")
    if (
        old_done.reason != DoneReason.REPLACED
        or canceled.order_id != replaced.old_order_id
        or old_done.order_id != replaced.old_order_id
        or canceled.canceled_quantity != old_done.remaining_quantity
    ):
        raise NativeProtocolError("replace old-order lifecycle payloads are inconsistent")

    trades, terminal, book_changed = _trade_terminal_suffix(events, 4)
    if terminal.order_id != replaced.new_order_id:
        raise NativeProtocolError("replacement terminal event has the wrong new order ID")
    if isinstance(terminal, DoneEvent) and terminal.reason != DoneReason.FILLED:
        raise NativeProtocolError("GTC replacement may terminate only as filled")
    _validate_trade_chain(trades, terminal)
    if trades and any(trade.aggressor_order_id != replaced.new_order_id for trade in trades):
        raise NativeProtocolError("replacement trade has the wrong aggressor order ID")
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
        raise NativeProtocolError("event envelope is missing its aggressor terminal event")
    terminal = cast(RestedEvent | DoneEvent, events[index])
    index += 1
    book_changed: BookChangedEvent | None = None
    if index < len(events) and isinstance(events[index], BookChangedEvent):
        book_changed = cast(BookChangedEvent, events[index])
        index += 1
    if index != len(events):
        raise NativeProtocolError("events appear after the terminal envelope")
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
            raise NativeProtocolError("trade aggressor identity changes within one command")
        if trade.resting_order_id in seen_resting_ids:
            raise NativeProtocolError("one passive order appears in multiple trade events")
        seen_resting_ids.add(trade.resting_order_id)
        if trade.execution_quantity > prior_remaining:
            raise NativeProtocolError("trade execution exceeds the aggressor remainder")
        if trade.aggressor_remaining != prior_remaining - trade.execution_quantity:
            raise NativeProtocolError("trade aggressor quantities do not form a chain")
        if index != 0:
            if first.aggressor_side == Side.BUY and trade.execution_price < prior_price:
                raise NativeProtocolError("buy trades are not in ascending passive-price order")
            if first.aggressor_side == Side.SELL and trade.execution_price > prior_price:
                raise NativeProtocolError("sell trades are not in descending passive-price order")
        if trade.resting_remaining != 0 and (
            trade.aggressor_remaining != 0 or index != len(trades) - 1
        ):
            raise NativeProtocolError("partially filled passive is not the final trade")
        prior_price = trade.execution_price
        prior_remaining = trade.aggressor_remaining

    if terminal.order_id != first.aggressor_order_id:
        raise NativeProtocolError("aggressor terminal event has the wrong order ID")
    if terminal.remaining_quantity != prior_remaining:
        raise NativeProtocolError("terminal quantity differs from the final trade remainder")
    if isinstance(terminal, RestedEvent):
        if terminal.client_id != first.aggressor_client_id or terminal.side != first.aggressor_side:
            raise NativeProtocolError("rested aggressor identity differs from its trades")


def _validate_terminal_top(event: BookChangedEvent) -> None:
    _validate_top_pair(event.best_bid, event.best_ask, "terminal book-changed event")


def _validate_top_pair(
    best_bid: TopOfBookLevel | None,
    best_ask: TopOfBookLevel | None,
    name: str,
) -> None:
    for side_name, level in (("bid", best_bid), ("ask", best_ask)):
        if level is not None and (level.price <= 0 or level.aggregate_quantity == 0):
            raise NativeProtocolError(f"{name} {side_name} must have positive price and quantity")
    if best_bid is not None and best_ask is not None and best_bid.price >= best_ask.price:
        raise NativeProtocolError(f"{name} is crossed")


def _validate_result_mode(result: NativeResultRecord, mode: OutputMode) -> None:
    if result.outcome != "engine_error":
        if mode == "exact" and result.events is None:
            raise NativeProtocolError("exact result omits events")
        if mode == "compact" and result.events is not None:
            raise NativeProtocolError("compact result contains events")


def _validate_state_snapshot(state: NativeState, snapshot: BookSnapshot) -> None:
    if state.active_order_count != snapshot.active_order_count:
        raise NativeProtocolError("observer count differs from snapshot")
    if state.empty != (snapshot.active_order_count == 0):
        raise NativeProtocolError("empty observer differs from snapshot")
    if state.sequence_exhausted != snapshot.sequence_exhausted:
        raise NativeProtocolError("sequence exhaustion differs from snapshot")
    expected_next = 0 if snapshot.sequence_exhausted else snapshot.last_sequence + 1
    if state.next_sequence != expected_next:
        raise NativeProtocolError("next sequence differs from snapshot")
    expected_bid = (
        TopOfBookLevel(snapshot.bids[0].price, snapshot.bids[0].aggregate_quantity)
        if snapshot.bids
        else None
    )
    expected_ask = (
        TopOfBookLevel(snapshot.asks[0].price, snapshot.asks[0].aggregate_quantity)
        if snapshot.asks
        else None
    )
    if state.best_bid != expected_bid or state.best_ask != expected_ask:
        raise NativeProtocolError("top observer differs from snapshot")
    if state_digest(snapshot) != state.state_digest:
        raise NativeProtocolError("state digest differs from canonical snapshot")


def _validate_state_structure(state: NativeState) -> None:
    if state.empty != (state.active_order_count == 0):
        raise NativeProtocolError("empty observer differs from active count")
    if state.sequence_exhausted:
        if state.next_sequence != 0:
            raise NativeProtocolError("exhausted observer has a nonzero next sequence")
    elif state.next_sequence == 0:
        raise NativeProtocolError("available observer has an unassigned next sequence")
    _validate_top_pair(state.best_bid, state.best_ask, "state observer")


def _validate_transcript(
    transcript: NativeTranscript,
    *,
    expected_mode: OutputMode | None,
    expected_input: NativeInputConfig | None,
    expected_commands: Sequence[Command] | None,
    returncode: int | None,
) -> None:
    config = transcript.config
    commands = tuple(expected_commands) if expected_commands is not None else None

    if config is None:
        if transcript.results or transcript.final is not None:
            raise NativeProtocolError("transcript has engine records without a config")
        if expected_mode is not None or expected_input is not None or commands is not None:
            raise NativeProtocolError("native output did not echo the requested config")
    else:
        if expected_mode is not None and config.mode != expected_mode:
            raise NativeProtocolError("native output mode differs from the requested mode")
        if expected_input is not None and config.input != expected_input:
            raise NativeProtocolError("native output config differs from the requested config")
        _validate_configured_records(config, transcript.results, transcript.final)

    _validate_result_timeline(transcript.results)
    _validate_terminal_continuity(transcript)

    if commands is not None:
        _validate_expected_commands(transcript, commands)
    if returncode is not None:
        _validate_process_terminal_state(
            transcript,
            returncode,
            len(commands) if commands is not None else None,
        )


def _validate_configured_records(
    config: NativeConfigRecord,
    results: tuple[NativeResultRecord, ...],
    final: NativeFinalRecord | None,
) -> None:
    for result in results:
        expected_checkpoint = config.input.snapshot_interval != 0 and (
            (result.command_index + 1) % config.input.snapshot_interval == 0
        )
        if (result.snapshot is not None) != expected_checkpoint:
            raise NativeProtocolError("result snapshot does not follow the configured cadence")
        _validate_state_against_config(result.state, config.input)
        if result.snapshot is not None:
            _validate_snapshot_against_config(result.snapshot, config.input)
        if result.events is not None:
            _validate_events_against_config(result.events, config.input)
            if isinstance(result.events[0], AcceptedEvent):
                if result.events[0].header.instrument_id != config.input.instrument_id:
                    raise NativeProtocolError("accepted event uses a different routed instrument")
    if final is not None:
        _validate_state_against_config(final.state, config.input)
        _validate_snapshot_against_config(final.snapshot, config.input)


def _validate_result_timeline(results: tuple[NativeResultRecord, ...]) -> None:
    previous: NativeState | None = None
    engine_error_seen = False
    for index, result in enumerate(results):
        if engine_error_seen:
            raise NativeProtocolError("result appears after an engine error")
        if result.outcome == "engine_error":
            engine_error_seen = True
        else:
            expected_sequence = index + 1
            if result.command_sequence != expected_sequence:
                raise NativeProtocolError("domain result sequences are not contiguous from one")
            expected_exhausted = expected_sequence == U64_MAX
            expected_next = 0 if expected_exhausted else expected_sequence + 1
            if (
                result.state.sequence_exhausted != expected_exhausted
                or result.state.next_sequence != expected_next
            ):
                raise NativeProtocolError("post-command sequence observers are inconsistent")

        if result.outcome == "rejected":
            if previous is None:
                if (
                    result.state.active_order_count != 0
                    or not result.state.empty
                    or result.state.best_bid is not None
                    or result.state.best_ask is not None
                ):
                    raise NativeProtocolError("first rejected command mutated the fresh book")
            elif (
                result.state.active_order_count != previous.active_order_count
                or result.state.empty != previous.empty
                or result.state.best_bid != previous.best_bid
                or result.state.best_ask != previous.best_ask
            ):
                raise NativeProtocolError("rejected command changed visible book observers")
        if result.events is not None and result.outcome == "committed":
            before_bid = previous.best_bid if previous is not None else None
            before_ask = previous.best_ask if previous is not None else None
            top_changed = before_bid != result.state.best_bid or before_ask != result.state.best_ask
            emitted_book_change = isinstance(result.events[-1], BookChangedEvent)
            if emitted_book_change != top_changed:
                raise NativeProtocolError(
                    "book-changed event presence differs from the visible top transition"
                )
        previous = result.state


def _validate_terminal_continuity(transcript: NativeTranscript) -> None:
    results = transcript.results
    final = transcript.final
    error = transcript.error
    if error is not None:
        if results and results[-1].outcome == "engine_error":
            raise NativeProtocolError("adapter error follows a terminal engine-error result")
        if results and error.line <= results[-1].line:
            raise NativeProtocolError("terminal adapter error does not follow the result prefix")
        return
    if final is None:
        return
    if results:
        if final.state != results[-1].state:
            raise NativeProtocolError("final state differs from the last result state")
        if results[-1].snapshot is not None and final.snapshot != results[-1].snapshot:
            raise NativeProtocolError("final snapshot differs from the last checkpoint")
    else:
        if (
            final.snapshot.last_sequence != 0
            or final.snapshot.sequence_exhausted
            or final.snapshot.active_order_count != 0
            or final.snapshot.bids
            or final.snapshot.asks
            or final.state.next_sequence != 1
        ):
            raise NativeProtocolError("zero-command final record is not a fresh engine")


def _validate_expected_commands(
    transcript: NativeTranscript,
    commands: tuple[Command, ...],
) -> None:
    if len(transcript.results) > len(commands):
        raise NativeProtocolError("native output contains more results than requested commands")
    for index, result in enumerate(transcript.results):
        command = commands[index]
        if result.command_type != command_type(command):
            raise NativeProtocolError("result command type differs from the submitted command")
        if result.line != index + 2:
            raise NativeProtocolError("result source line differs from the encoded command line")
        if result.events is not None:
            if result.events[0].header.instrument_id != command.instrument_id:
                raise NativeProtocolError("event instrument differs from the submitted command")
            _validate_events_against_command(result.events, command, result.outcome)

    error = transcript.error
    if error is None:
        return
    if error.line != len(transcript.results) + 2:
        raise NativeProtocolError("terminal adapter error is not at the next submitted line")
    if error.code not in _REQUEST_BOUND_ERROR_CODES:
        raise NativeProtocolError("canonical typed input cannot produce this adapter error")
    if len(transcript.results) == len(commands) and error.code != "input_read_failure":
        raise NativeProtocolError("terminal adapter error appears after every submitted command")


def _validate_events_against_command(
    events: tuple[Event, ...],
    command: Command,
    outcome: Literal["committed", "rejected", "engine_error"],
) -> None:
    if outcome == "rejected":
        return
    if isinstance(command, CancelOrder):
        canceled = cast(CanceledEvent, events[1])
        done = cast(DoneEvent, events[2])
        if canceled.order_id != command.order_id or done.order_id != command.order_id:
            raise NativeProtocolError("cancel events use the wrong submitted order ID")
        return
    if isinstance(command, ReplaceOrder):
        replaced = cast(ReplacedEvent, events[1])
        if (
            replaced.old_order_id != command.old_order_id
            or replaced.new_order_id != command.new_order_id
        ):
            raise NativeProtocolError("replace event lineage differs from the submitted IDs")
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
        raise NativeProtocolError("terminal event uses the wrong submitted order ID")
    remaining = initial_quantity
    for trade in trades:
        if trade.aggressor_order_id != order_id or trade.aggressor_client_id != client_id:
            raise NativeProtocolError("trade aggressor differs from the submitted command")
        if side is not None and trade.aggressor_side != side:
            raise NativeProtocolError("trade aggressor side differs from the submitted command")
        if limit_price is not None:
            effective_side = side if side is not None else trade.aggressor_side
            if effective_side == Side.BUY and trade.execution_price > limit_price:
                raise NativeProtocolError("buy trade executes beyond the submitted limit")
            if effective_side == Side.SELL and trade.execution_price < limit_price:
                raise NativeProtocolError("sell trade executes beyond the submitted limit")
        if trade.execution_quantity > remaining:
            raise NativeProtocolError("trade execution exceeds submitted quantity")
        remaining -= trade.execution_quantity
        if trade.aggressor_remaining != remaining:
            raise NativeProtocolError("trade remainder differs from submitted quantity chain")
    if terminal.remaining_quantity != remaining:
        raise NativeProtocolError("terminal remainder differs from submitted quantity chain")

    if isinstance(terminal, RestedEvent):
        if terminal.client_id != client_id:
            raise NativeProtocolError("rested client differs from the submitted command")
        if side is not None and terminal.side != side:
            raise NativeProtocolError("rested side differs from the submitted command")
        if limit_price is None or terminal.price != limit_price:
            raise NativeProtocolError("rested price differs from the submitted command")
        if not replacement and (order_type != OrderType.LIMIT or time_in_force != TimeInForce.GTC):
            raise NativeProtocolError("non-GTC-limit command emitted a rested event")
    elif remaining == 0:
        if terminal.reason != DoneReason.FILLED:
            raise NativeProtocolError("zero residual did not terminate as filled")
    elif replacement:
        raise NativeProtocolError("GTC replacement residual did not rest")
    elif order_type == OrderType.MARKET:
        if terminal.reason != DoneReason.MARKET_EXHAUSTED:
            raise NativeProtocolError("market residual has the wrong terminal reason")
    elif time_in_force == TimeInForce.IOC:
        if terminal.reason != DoneReason.IOC_RESIDUAL_CANCELED:
            raise NativeProtocolError("IOC residual has the wrong terminal reason")
    else:
        raise NativeProtocolError("GTC limit residual did not rest")


def _validate_state_against_config(state: NativeState, config: NativeInputConfig) -> None:
    if state.active_order_count > config.engine.max_active_orders:
        raise NativeProtocolError("state observer exceeds configured active capacity")
    for level in (state.best_bid, state.best_ask):
        if level is not None and level.price % config.engine.tick_increment != 0:
            raise NativeProtocolError("state top is not aligned to the configured tick")


def _validate_snapshot_against_config(
    snapshot: BookSnapshot,
    config: NativeInputConfig,
) -> None:
    if snapshot.instrument_id != config.instrument_id:
        raise NativeProtocolError("snapshot instrument differs from the requested route")
    if snapshot.active_order_count > config.engine.max_active_orders:
        raise NativeProtocolError("snapshot exceeds configured active capacity")
    for level in (*snapshot.bids, *snapshot.asks):
        if level.price % config.engine.tick_increment != 0:
            raise NativeProtocolError("snapshot level is not aligned to the configured tick")
        for order in level.orders:
            if order.remaining_quantity > config.engine.max_order_quantity:
                raise NativeProtocolError("snapshot order exceeds configured quantity")


def _validate_events_against_config(
    events: tuple[Event, ...],
    config: NativeInputConfig,
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
        if any(price % config.engine.tick_increment != 0 for price in prices):
            raise NativeProtocolError("event price is not aligned to the configured tick")
        if any(quantity > config.engine.max_order_quantity for quantity in quantities):
            raise NativeProtocolError("event order quantity exceeds the configured maximum")


def _validate_process_terminal_state(
    transcript: NativeTranscript,
    returncode: int,
    expected_command_count: int | None,
) -> None:
    if isinstance(returncode, bool) or not isinstance(returncode, int):
        raise NativeProtocolError("native process return code is not an integer")
    has_engine_error = bool(transcript.results and transcript.results[-1].outcome == "engine_error")
    if returncode == _SUCCESS_EXIT_CODE:
        if transcript.error is not None or transcript.final is None or has_engine_error:
            raise NativeProtocolError("successful process has a failure terminal record")
        if expected_command_count is not None and len(transcript.results) != expected_command_count:
            raise NativeProtocolError("successful process did not execute every requested command")
        return
    if returncode == _INPUT_ERROR_EXIT_CODE:
        if transcript.error is None or transcript.final is not None:
            raise NativeProtocolError("input-error process lacks its adapter error terminal")
        if transcript.error.code in {
            "adapter_exception",
            "engine_construction_failure",
            "engine_exception",
            "resource_failure",
        }:
            raise NativeProtocolError("input-error exit carries an engine-failure code")
        return
    if returncode == _ENGINE_ERROR_EXIT_CODE:
        if transcript.error is None and not (transcript.final is not None and has_engine_error):
            raise NativeProtocolError("engine-error process lacks a failure terminal")
        if transcript.error is not None and transcript.error.code not in {
            "adapter_exception",
            "engine_construction_failure",
            "engine_exception",
            "resource_failure",
        }:
            raise NativeProtocolError("engine-error exit carries an input-failure code")
        return
    raise NativeProtocolError(f"native process returned an unsupported exit code: {returncode}")


def _unique_json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise NativeProtocolError(f"JSON object contains duplicate field: {key}")
        result[key] = value
    return result


def _reject_json_constant(value: str) -> NoReturn:
    raise NativeProtocolError(f"JSON contains a non-finite numeric constant: {value}")


def _require_schema(record: dict[str, object]) -> None:
    if _string(_field(record, "schema"), "schema") != _SCHEMA:
        raise NativeProtocolError("unsupported native output schema")


def _require_keys(record: dict[str, object], expected: set[str]) -> None:
    actual = set(record)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise NativeProtocolError(f"record fields differ: missing={missing}, extra={extra}")


def _field(record: dict[str, object], name: str) -> object:
    try:
        return record[name]
    except KeyError as exc:
        raise NativeProtocolError(f"missing field: {name}") from exc


def _object(value: object, name: str) -> dict[str, object]:
    if not isinstance(value, dict) or not all(isinstance(key, str) for key in value):
        raise NativeProtocolError(f"{name} is not a JSON object")
    return cast(dict[str, object], value)


def _array(value: object, name: str) -> list[object]:
    if not isinstance(value, list):
        raise NativeProtocolError(f"{name} is not a JSON array")
    return cast(list[object], value)


def _string(value: object, name: str) -> str:
    if not isinstance(value, str):
        raise NativeProtocolError(f"{name} is not a string")
    return value


def _boolean(value: object, name: str) -> bool:
    if not isinstance(value, bool):
        raise NativeProtocolError(f"{name} is not a boolean")
    return value


def _json_uint(value: object, name: str, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
        raise NativeProtocolError(f"{name} is not an unsigned JSON integer")
    return value


def _decimal_uint(value: object, name: str, maximum: int) -> int:
    text = _string(value, name)
    if _UNSIGNED_DECIMAL.fullmatch(text) is None:
        raise NativeProtocolError(f"{name} is not a canonical unsigned decimal string")
    try:
        parsed = int(text)
    except ValueError as exc:
        raise NativeProtocolError(f"{name} cannot be represented as a Python integer") from exc
    if parsed > maximum:
        raise NativeProtocolError(f"{name} exceeds its fixed-width representation")
    return parsed


def _optional_decimal_uint(value: object, name: str, maximum: int) -> int | None:
    return None if value is None else _decimal_uint(value, name, maximum)


def _decimal_int(value: object, name: str, minimum: int, maximum: int) -> int:
    text = _string(value, name)
    if _SIGNED_DECIMAL.fullmatch(text) is None:
        raise NativeProtocolError(f"{name} is not a canonical signed decimal string")
    try:
        parsed = int(text)
    except ValueError as exc:
        raise NativeProtocolError(f"{name} cannot be represented as a Python integer") from exc
    if not minimum <= parsed <= maximum:
        raise NativeProtocolError(f"{name} exceeds its fixed-width representation")
    return parsed


def _digest_value(value: object, name: str) -> str:
    digest = _string(value, name)
    if _DIGEST.fullmatch(digest) is None:
        raise NativeProtocolError(f"{name} is not a lowercase SHA-256 value")
    return digest


def _optional_digest(value: object, name: str) -> str | None:
    return None if value is None else _digest_value(value, name)


def _enum_name(
    value: object,
    name: str,
    values: dict[str, _EnumValue],
) -> _EnumValue:
    text = _string(value, name)
    try:
        return values[text]
    except KeyError as exc:
        raise NativeProtocolError(f"{name} has an unknown value: {text}") from exc


def _optional_enum_name(
    value: object,
    name: str,
    values: dict[str, _EnumValue],
) -> _EnumValue | None:
    return None if value is None else _enum_name(value, name, values)


def _require_representable(command: Command) -> None:
    def uint(name: str, value: int, maximum: int) -> None:
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
            raise ValueError(f"{name} is outside its unsigned domain representation")

    def i64(name: str, value: int) -> None:
        if isinstance(value, bool) or not isinstance(value, int) or not I64_MIN <= value <= I64_MAX:
            raise ValueError(f"{name} is outside its i64 domain representation")

    if isinstance(command, NewOrder):
        uint("client_id", command.client_id, U32_MAX)
        uint("order_id", command.order_id, U64_MAX)
        uint("instrument_id", command.instrument_id, U32_MAX)
        uint("side", command.side, (1 << 8) - 1)
        uint("order_type", command.order_type, (1 << 8) - 1)
        uint("time_in_force", command.time_in_force, (1 << 8) - 1)
        if command.limit_price is not None:
            i64("limit_price", command.limit_price)
        uint("quantity", command.quantity, U64_MAX)
    elif isinstance(command, CancelOrder):
        uint("client_id", command.client_id, U32_MAX)
        uint("order_id", command.order_id, U64_MAX)
        uint("instrument_id", command.instrument_id, U32_MAX)
    elif isinstance(command, ReplaceOrder):
        uint("client_id", command.client_id, U32_MAX)
        uint("old_order_id", command.old_order_id, U64_MAX)
        uint("new_order_id", command.new_order_id, U64_MAX)
        uint("instrument_id", command.instrument_id, U32_MAX)
        i64("new_limit_price", command.new_limit_price)
        uint("new_quantity", command.new_quantity, U64_MAX)
    else:
        raise TypeError(f"unsupported command type: {type(command)!r}")
