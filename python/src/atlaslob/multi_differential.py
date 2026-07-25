"""Reference-side capture and metamorphic evidence for differential schema V2."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Final

from atlaslob.canonical import engine_state_digest, event_digest
from atlaslob.domain import (
    AcceptedEvent,
    BookChangedEvent,
    CanceledEvent,
    CancelOrder,
    Command,
    DoneEvent,
    EngineSnapshot,
    Event,
    InstrumentConfig,
    MultiInstrumentEngineConfig,
    NewOrder,
    PriceLevelSnapshot,
    ReferenceResult,
    RejectedEvent,
    ReplacedEvent,
    ReplaceOrder,
    RestedEvent,
    TopOfBookLevel,
    TradeEvent,
    event_type,
)
from atlaslob.generation import SplitMix64
from atlaslob.multi_workload import command_to_dict
from atlaslob.router import ReferenceRouter

MULTI_DIFFERENTIAL_CAPTURE_SCHEMA: Final = "atlas_differential_capture_v2"


@dataclass(frozen=True, slots=True)
class MultiCaptureRecord:
    command_index: int
    command: Command
    result: ReferenceResult
    post_state_digest: str
    post_snapshot: EngineSnapshot


@dataclass(frozen=True, slots=True)
class MultiDifferentialCapture:
    catalog: tuple[InstrumentConfig, ...]
    engine_config: MultiInstrumentEngineConfig
    records: tuple[MultiCaptureRecord, ...]
    final_snapshot: EngineSnapshot
    final_state_digest: str


def capture_reference_router(
    catalog: tuple[InstrumentConfig, ...],
    commands: tuple[Command, ...],
    *,
    engine_config: MultiInstrumentEngineConfig | None = None,
) -> MultiDifferentialCapture:
    config = engine_config if engine_config is not None else MultiInstrumentEngineConfig()
    router = ReferenceRouter(catalog, config)
    records: list[MultiCaptureRecord] = []
    for index, command in enumerate(commands):
        result = router.execute(command)
        post_snapshot = router.snapshot()
        records.append(
            MultiCaptureRecord(
                command_index=index,
                command=command,
                result=result,
                post_state_digest=engine_state_digest(post_snapshot),
                post_snapshot=post_snapshot,
            )
        )
        if result.error is not None:
            break
    final_snapshot = router.snapshot()
    return MultiDifferentialCapture(
        catalog=router.catalog,
        engine_config=config,
        records=tuple(records),
        final_snapshot=final_snapshot,
        final_state_digest=engine_state_digest(final_snapshot),
    )


def multi_capture_to_dict(capture: MultiDifferentialCapture) -> dict[str, object]:
    return {
        "schema": MULTI_DIFFERENTIAL_CAPTURE_SCHEMA,
        "semantics_version": capture.final_snapshot.semantics_version,
        "engine": {
            "max_total_active_orders": str(capture.engine_config.max_total_active_orders),
            "catalog": [
                {
                    "instrument_id": str(entry.instrument_id),
                    "max_order_quantity": str(entry.matching.max_order_quantity),
                    "tick_increment": str(entry.matching.tick_increment),
                    "max_active_orders": str(entry.matching.max_active_orders),
                }
                for entry in capture.catalog
            ],
        },
        "records": [
            {
                "command_index": str(record.command_index),
                "command": command_to_dict(record.command),
                "result": _result_to_dict(record.result),
                "post_state_digest": record.post_state_digest,
            }
            for record in capture.records
        ],
        "final": {
            "state_digest": capture.final_state_digest,
            "state": _engine_snapshot_to_dict(capture.final_snapshot),
        },
    }


def reinterleave_independent(
    catalog: tuple[InstrumentConfig, ...],
    commands: tuple[Command, ...],
    seed: int,
    *,
    engine_config: MultiInstrumentEngineConfig | None = None,
) -> tuple[Command, ...]:
    """Reorder instruments while retaining command order within each instrument."""

    _require_independent_reinterleaving(catalog, commands, engine_config)
    groups: dict[int, list[Command]] = {}
    for command in commands:
        groups.setdefault(command.instrument_id, []).append(command)
    positions = {instrument_id: 0 for instrument_id in groups}
    rng = SplitMix64(seed)
    output: list[Command] = []
    while len(output) < len(commands):
        available = tuple(
            sorted(
                instrument_id
                for instrument_id, group in groups.items()
                if positions[instrument_id] < len(group)
            )
        )
        chosen = available[rng.randbelow(len(available))]
        output.append(groups[chosen][positions[chosen]])
        positions[chosen] += 1
    return tuple(output)


def normalized_economic_state(snapshot: EngineSnapshot) -> tuple[object, ...]:
    """Remove absolute global priorities while retaining exact FIFO order."""

    return tuple(
        (
            book.instrument_id,
            tuple(
                (
                    level.price,
                    level.aggregate_quantity,
                    tuple(
                        (
                            order.order_id,
                            order.client_id,
                            int(order.side),
                            order.price,
                            order.remaining_quantity,
                        )
                        for order in level.orders
                    ),
                )
                for level in book.bids
            ),
            tuple(
                (
                    level.price,
                    level.aggregate_quantity,
                    tuple(
                        (
                            order.order_id,
                            order.client_id,
                            int(order.side),
                            order.price,
                            order.remaining_quantity,
                        )
                        for order in level.orders
                    ),
                )
                for level in book.asks
            ),
        )
        for book in snapshot.instruments
    )


def normalized_instrument_evidence(
    capture: MultiDifferentialCapture,
) -> tuple[tuple[int, tuple[object, ...]], ...]:
    """Group outcomes by instrument and replace global sequence with local ordinal."""

    grouped: dict[int, list[object]] = {}
    for record in capture.records:
        batch = record.result.batch
        instrument_id = record.command.instrument_id
        local_records = grouped.setdefault(instrument_id, [])
        local_ordinal = len(local_records)
        if batch is None:
            local_records.append((local_ordinal, "engine_error", int(record.result.error or 0)))
            continue
        local_records.append(
            (
                local_ordinal,
                "committed" if batch.committed else "rejected",
                tuple(_normalized_event(event) for event in batch.events),
            )
        )
    return tuple(
        (instrument_id, tuple(records)) for instrument_id, records in sorted(grouped.items())
    )


def reinterleaving_is_equivalent(
    catalog: tuple[InstrumentConfig, ...],
    commands: tuple[Command, ...],
    seed: int,
    *,
    engine_config: MultiInstrumentEngineConfig | None = None,
) -> bool:
    """Check the independent-instrument reinterleaving property."""

    first = capture_reference_router(
        catalog,
        commands,
        engine_config=engine_config,
    )
    second = capture_reference_router(
        catalog,
        reinterleave_independent(
            catalog,
            commands,
            seed,
            engine_config=engine_config,
        ),
        engine_config=engine_config,
    )
    return normalized_economic_state(first.final_snapshot) == normalized_economic_state(
        second.final_snapshot
    ) and normalized_instrument_evidence(first) == normalized_instrument_evidence(second)


def _require_independent_reinterleaving(
    catalog: tuple[InstrumentConfig, ...],
    commands: tuple[Command, ...],
    engine_config: MultiInstrumentEngineConfig | None,
) -> None:
    configured = {entry.instrument_id: entry for entry in catalog}
    if not configured or len(configured) != len(catalog):
        raise ValueError("reinterleaving catalog must be nonempty with unique IDs")
    unknown = sorted({command.instrument_id for command in commands} - set(configured))
    if unknown:
        raise ValueError("reinterleaving commands must route only to configured instruments")
    config = engine_config if engine_config is not None else MultiInstrumentEngineConfig()
    if config.max_total_active_orders < sum(entry.matching.max_active_orders for entry in catalog):
        raise ValueError("reinterleaving requires nonbinding engine-wide capacity")

    identity_instrument: dict[int, int] = {}
    for command in commands:
        order_ids: tuple[int, ...]
        if isinstance(command, ReplaceOrder):
            order_ids = (command.old_order_id, command.new_order_id)
        elif isinstance(command, (NewOrder, CancelOrder)):
            order_ids = (command.order_id,)
        else:
            raise TypeError(f"unsupported command type: {type(command)!r}")
        for order_id in order_ids:
            if order_id == 0:
                continue
            previous = identity_instrument.setdefault(order_id, command.instrument_id)
            if previous != command.instrument_id:
                raise ValueError("reinterleaving requires disjoint per-instrument order IDs")


def _result_to_dict(result: ReferenceResult) -> dict[str, object]:
    if result.error is not None:
        return {"classification": "engine_error", "error": int(result.error)}
    batch = result.batch
    if batch is None:
        raise RuntimeError("reference result has neither batch nor engine error")
    return {
        "classification": "committed" if batch.committed else "rejected",
        "event_digest": event_digest(batch),
        "events": [_event_to_dict(event) for event in batch.events],
    }


def _header_to_dict(event: Event) -> dict[str, object]:
    return {
        "command_sequence": str(event.header.command_sequence),
        "event_index": event.header.event_index,
        "instrument_id": str(event.header.instrument_id),
    }


def _event_to_dict(event: Event) -> dict[str, object]:
    output: dict[str, object] = {
        "type": int(event_type(event)),
        "header": _header_to_dict(event),
    }
    if isinstance(event, AcceptedEvent):
        output["command_type"] = int(event.command_type)
    elif isinstance(event, RejectedEvent):
        output["command_type"] = int(event.command_type)
        output["reason"] = int(event.reason)
        output["order_id"] = None if event.order_id is None else str(event.order_id)
    elif isinstance(event, TradeEvent):
        output.update(
            {
                "aggressor_order_id": str(event.aggressor_order_id),
                "resting_order_id": str(event.resting_order_id),
                "aggressor_client_id": str(event.aggressor_client_id),
                "resting_client_id": str(event.resting_client_id),
                "aggressor_side": int(event.aggressor_side),
                "execution_price": str(event.execution_price),
                "execution_quantity": str(event.execution_quantity),
                "aggressor_remaining": str(event.aggressor_remaining),
                "resting_remaining": str(event.resting_remaining),
            }
        )
    elif isinstance(event, RestedEvent):
        output.update(
            {
                "order_id": str(event.order_id),
                "client_id": str(event.client_id),
                "side": int(event.side),
                "price": str(event.price),
                "remaining_quantity": str(event.remaining_quantity),
            }
        )
    elif isinstance(event, CanceledEvent):
        output["order_id"] = str(event.order_id)
        output["canceled_quantity"] = str(event.canceled_quantity)
    elif isinstance(event, ReplacedEvent):
        output["old_order_id"] = str(event.old_order_id)
        output["new_order_id"] = str(event.new_order_id)
    elif isinstance(event, DoneEvent):
        output["order_id"] = str(event.order_id)
        output["reason"] = int(event.reason)
        output["remaining_quantity"] = str(event.remaining_quantity)
    elif isinstance(event, BookChangedEvent):
        output["best_bid"] = _top_to_dict(event.best_bid)
        output["best_ask"] = _top_to_dict(event.best_ask)
    else:
        raise TypeError(f"unsupported event type: {type(event)!r}")
    return output


def _top_to_dict(value: TopOfBookLevel | None) -> dict[str, object] | None:
    if value is None:
        return None
    return {
        "price": str(value.price),
        "aggregate_quantity": str(value.aggregate_quantity),
    }


def _engine_snapshot_to_dict(snapshot: EngineSnapshot) -> dict[str, object]:
    return {
        "last_sequence": str(snapshot.last_sequence),
        "sequence_exhausted": snapshot.sequence_exhausted,
        "active_order_count": str(snapshot.active_order_count),
        "instruments": [
            {
                "instrument_id": str(book.instrument_id),
                "active_order_count": str(book.active_order_count),
                "bids": [_level_to_dict(level) for level in book.bids],
                "asks": [_level_to_dict(level) for level in book.asks],
            }
            for book in snapshot.instruments
        ],
    }


def _level_to_dict(value: PriceLevelSnapshot) -> dict[str, object]:
    return {
        "price": str(value.price),
        "aggregate_quantity": str(value.aggregate_quantity),
        "orders": [
            {
                "order_id": str(order.order_id),
                "client_id": str(order.client_id),
                "instrument_id": str(order.instrument_id),
                "side": int(order.side),
                "price": str(order.price),
                "remaining_quantity": str(order.remaining_quantity),
                "priority_sequence": str(order.priority_sequence),
            }
            for order in value.orders
        ],
    }


def _normalized_event(event: Event) -> tuple[object, ...]:
    payload = _event_to_dict(event)
    header = payload.pop("header")
    if not isinstance(header, dict):
        raise RuntimeError("event header mapping is not an object")
    header.pop("command_sequence")
    return (
        tuple(sorted(header.items())),
        tuple(sorted(payload.items(), key=lambda item: item[0])),
    )
