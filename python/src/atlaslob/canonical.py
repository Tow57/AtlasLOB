"""Independent implementation of the canonical ADR 0009 digest encodings."""

from __future__ import annotations

import hashlib
import struct

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
    BookSnapshot,
    CanceledEvent,
    DoneEvent,
    Event,
    EventBatch,
    OrderSnapshot,
    PriceLevelSnapshot,
    RejectedEvent,
    ReplacedEvent,
    RestedEvent,
    TopOfBookLevel,
    TradeEvent,
    event_type,
)

_STATE_PREFIX = b"ATLSST01"
_EVENT_PREFIX = b"ATLSEV01"


def _u8(value: int) -> bytes:
    if not 0 <= value <= U8_MAX:
        raise ValueError(f"value is not a u8: {value}")
    return struct.pack(">B", value)


def _u16(value: int) -> bytes:
    if not 0 <= value <= U16_MAX:
        raise ValueError(f"value is not a u16: {value}")
    return struct.pack(">H", value)


def _u32(value: int) -> bytes:
    if not 0 <= value <= U32_MAX:
        raise ValueError(f"value is not a u32: {value}")
    return struct.pack(">I", value)


def _u64(value: int) -> bytes:
    if not 0 <= value <= U64_MAX:
        raise ValueError(f"value is not a u64: {value}")
    return struct.pack(">Q", value)


def _i64(value: int) -> bytes:
    if not I64_MIN <= value <= I64_MAX:
        raise ValueError(f"value is not an i64: {value}")
    return _u64(value & U64_MAX)


def _encode_order(output: bytearray, order: OrderSnapshot) -> None:
    output += _u64(order.order_id)
    output += _u32(order.client_id)
    output += _u32(order.instrument_id)
    output += _u8(order.side)
    output += _i64(order.price)
    output += _u64(order.remaining_quantity)
    output += _u64(order.priority_sequence)


def _encode_level(output: bytearray, level: PriceLevelSnapshot) -> None:
    output += _i64(level.price)
    output += _u64(level.aggregate_quantity)
    output += _u64(len(level.orders))
    for order in level.orders:
        _encode_order(output, order)


def state_bytes(snapshot: BookSnapshot) -> bytes:
    """Return the exact canonical state stream defined by ADR 0009."""

    output = bytearray(_STATE_PREFIX)
    output += _u16(snapshot.semantics_version)
    output += _u32(snapshot.instrument_id)
    output += _u64(snapshot.last_sequence)
    output += _u8(int(snapshot.sequence_exhausted))
    output += _u64(snapshot.active_order_count)
    output += _u64(len(snapshot.bids))
    for level in snapshot.bids:
        _encode_level(output, level)
    output += _u64(len(snapshot.asks))
    for level in snapshot.asks:
        _encode_level(output, level)
    return bytes(output)


def state_digest(snapshot: BookSnapshot) -> str:
    """Return a lowercase SHA-256 digest for one canonical book snapshot."""

    return hashlib.sha256(state_bytes(snapshot)).hexdigest()


def _optional_order_id(output: bytearray, order_id: int | None) -> None:
    output += _u8(int(order_id is not None))
    output += _u64(order_id if order_id is not None else 0)


def _optional_top(output: bytearray, level: TopOfBookLevel | None) -> None:
    output += _u8(int(level is not None))
    output += _i64(level.price if level is not None else 0)
    output += _u64(level.aggregate_quantity if level is not None else 0)


def _encode_event_payload(output: bytearray, event: Event) -> None:
    if isinstance(event, AcceptedEvent):
        output += _u8(event.command_type)
    elif isinstance(event, RejectedEvent):
        output += _u8(event.command_type)
        output += _u16(event.reason)
        _optional_order_id(output, event.order_id)
    elif isinstance(event, TradeEvent):
        output += _u64(event.aggressor_order_id)
        output += _u64(event.resting_order_id)
        output += _u32(event.aggressor_client_id)
        output += _u32(event.resting_client_id)
        output += _u8(event.aggressor_side)
        output += _i64(event.execution_price)
        output += _u64(event.execution_quantity)
        output += _u64(event.aggressor_remaining)
        output += _u64(event.resting_remaining)
    elif isinstance(event, RestedEvent):
        output += _u64(event.order_id)
        output += _u32(event.client_id)
        output += _u8(event.side)
        output += _i64(event.price)
        output += _u64(event.remaining_quantity)
    elif isinstance(event, CanceledEvent):
        output += _u64(event.order_id)
        output += _u64(event.canceled_quantity)
    elif isinstance(event, ReplacedEvent):
        output += _u64(event.old_order_id)
        output += _u64(event.new_order_id)
    elif isinstance(event, DoneEvent):
        output += _u64(event.order_id)
        output += _u8(event.reason)
        output += _u64(event.remaining_quantity)
    elif isinstance(event, BookChangedEvent):
        _optional_top(output, event.best_bid)
        _optional_top(output, event.best_ask)
    else:
        raise TypeError(f"unsupported event type: {type(event)!r}")


def event_bytes(batch: EventBatch) -> bytes:
    """Return the exact canonical event-batch stream defined by ADR 0009."""

    output = bytearray(_EVENT_PREFIX)
    output += _u16(ATLASLOB_SEMANTICS_VERSION)
    output += _u64(batch.command_sequence)
    output += _u32(batch.instrument_id)
    output += _u64(len(batch.events))
    for event in batch.events:
        output += _u8(event_type(event))
        output += _u64(event.header.command_sequence)
        output += _u32(event.header.event_index)
        output += _u32(event.header.instrument_id)
        _encode_event_payload(output, event)
    return bytes(output)


def event_digest(batch: EventBatch) -> str:
    """Return a lowercase SHA-256 digest for one canonical event batch."""

    return hashlib.sha256(event_bytes(batch)).hexdigest()
