from __future__ import annotations

import os
from dataclasses import replace
from pathlib import Path

from atlaslob.domain import (
    BookSnapshot,
    CancelOrder,
    Command,
    Event,
    NewOrder,
    OrderSnapshot,
    OrderType,
    PriceLevelSnapshot,
    ReferenceResult,
    ReplaceOrder,
    Side,
    TimeInForce,
    TradeEvent,
)
from atlaslob.native import NativeInputConfig, run_native
from atlaslob.reference import ReferenceEngine

INSTRUMENT = 7
MIRROR_ANCHOR = 200


def _executable() -> Path:
    configured = os.environ.get("ATLAS_DIFF_NATIVE")
    if configured is not None:
        candidate = Path(configured)
        if not candidate.is_file():
            raise FileNotFoundError(
                f"ATLAS_DIFF_NATIVE does not name a native evidence executable: {configured}"
            )
        return candidate.resolve()
    candidates = (
        Path("build/dev-gcc/atlas_diff_native.exe"),
        Path("build/dev-gcc/atlas_diff_native"),
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError("build atlas_diff_native or set ATLAS_DIFF_NATIVE")


def _limit(
    order_id: int,
    side: Side,
    price: int,
    quantity: int,
    *,
    client_id: int,
    tif: TimeInForce = TimeInForce.GTC,
) -> NewOrder:
    return NewOrder(
        client_id=client_id,
        order_id=order_id,
        instrument_id=INSTRUMENT,
        side=side,
        order_type=OrderType.LIMIT,
        time_in_force=tif,
        limit_price=price,
        quantity=quantity,
    )


def _market(order_id: int, side: Side, quantity: int, *, client_id: int) -> NewOrder:
    return NewOrder(
        client_id=client_id,
        order_id=order_id,
        instrument_id=INSTRUMENT,
        side=side,
        order_type=OrderType.MARKET,
        time_in_force=TimeInForce.IOC,
        limit_price=None,
        quantity=quantity,
    )


def _execute(
    commands: tuple[Command, ...],
) -> tuple[tuple[ReferenceResult, ...], BookSnapshot]:
    reference = ReferenceEngine(INSTRUMENT)
    expected = tuple(reference.execute(command) for command in commands)
    expected_snapshot = reference.snapshot()

    native = run_native(
        _executable(),
        NativeInputConfig(INSTRUMENT, snapshot_interval=1),
        commands,
    )
    assert native.returncode == 0
    assert native.transcript.error is None
    assert native.transcript.final is not None
    assert native.transcript.final.snapshot == expected_snapshot
    assert len(native.transcript.results) == len(expected)
    for reference_result, native_result in zip(expected, native.transcript.results, strict=True):
        if reference_result.batch is None:
            assert native_result.engine_error == reference_result.error
        else:
            assert native_result.events == reference_result.batch.events
    return expected, expected_snapshot


def _events(results: tuple[ReferenceResult, ...]) -> tuple[Event, ...]:
    return tuple(
        event for result in results if result.batch is not None for event in result.batch.events
    )


def _trades(results: tuple[ReferenceResult, ...]) -> tuple[TradeEvent, ...]:
    return tuple(event for event in _events(results) if isinstance(event, TradeEvent))


def _book_without_sequence(snapshot: BookSnapshot) -> tuple[object, ...]:
    return (
        snapshot.instrument_id,
        snapshot.active_order_count,
        tuple(
            (
                level.price,
                level.aggregate_quantity,
                tuple(
                    (
                        order.order_id,
                        order.client_id,
                        order.instrument_id,
                        order.side,
                        order.price,
                        order.remaining_quantity,
                    )
                    for order in level.orders
                ),
            )
            for level in snapshot.bids
        ),
        tuple(
            (
                level.price,
                level.aggregate_quantity,
                tuple(
                    (
                        order.order_id,
                        order.client_id,
                        order.instrument_id,
                        order.side,
                        order.price,
                        order.remaining_quantity,
                    )
                    for order in level.orders
                ),
            )
            for level in snapshot.asks
        ),
    )


def _mirror_command(command: Command) -> Command:
    if isinstance(command, NewOrder):
        return replace(
            command,
            side=Side.SELL if command.side == Side.BUY else Side.BUY,
            limit_price=(
                None if command.limit_price is None else MIRROR_ANCHOR - command.limit_price
            ),
        )
    if isinstance(command, ReplaceOrder):
        return replace(
            command,
            new_limit_price=MIRROR_ANCHOR - command.new_limit_price,
        )
    return command


def _mirror_order(order: OrderSnapshot) -> OrderSnapshot:
    return replace(
        order,
        side=Side.SELL if order.side == Side.BUY else Side.BUY,
        price=MIRROR_ANCHOR - order.price,
    )


def _mirror_level(level: PriceLevelSnapshot) -> PriceLevelSnapshot:
    return PriceLevelSnapshot(
        price=MIRROR_ANCHOR - level.price,
        aggregate_quantity=level.aggregate_quantity,
        orders=tuple(_mirror_order(order) for order in level.orders),
    )


def _mirror_snapshot(snapshot: BookSnapshot) -> BookSnapshot:
    return BookSnapshot(
        semantics_version=snapshot.semantics_version,
        instrument_id=snapshot.instrument_id,
        last_sequence=snapshot.last_sequence,
        sequence_exhausted=snapshot.sequence_exhausted,
        active_order_count=snapshot.active_order_count,
        bids=tuple(_mirror_level(level) for level in snapshot.asks),
        asks=tuple(_mirror_level(level) for level in snapshot.bids),
    )


def _collapsed_consumption(
    trades: tuple[TradeEvent, ...],
) -> tuple[tuple[int, int], ...]:
    output: list[tuple[int, int]] = []
    for trade in trades:
        if output and output[-1][0] == trade.resting_order_id:
            order_id, quantity = output[-1]
            output[-1] = (order_id, quantity + trade.execution_quantity)
        else:
            output.append((trade.resting_order_id, trade.execution_quantity))
    return tuple(output)


def test_replay_is_deterministic_for_fresh_reference_and_native_engines() -> None:
    commands: tuple[Command, ...] = (
        _limit(1, Side.SELL, 100, 5, client_id=1),
        _limit(2, Side.SELL, 101, 7, client_id=2),
        _market(3, Side.BUY, 8, client_id=3),
        ReplaceOrder(2, 2, 4, INSTRUMENT, 99, 2),
        CancelOrder(2, 4, INSTRUMENT),
    )

    assert _execute(commands) == _execute(commands)


def test_side_and_price_mirror_preserves_quantities_identity_and_priority() -> None:
    original: tuple[Command, ...] = (
        _limit(1, Side.SELL, 101, 3, client_id=1),
        _limit(2, Side.SELL, 102, 5, client_id=2),
        _limit(3, Side.BUY, 98, 4, client_id=3),
        _market(4, Side.BUY, 6, client_id=4),
    )
    mirrored = tuple(_mirror_command(command) for command in original)

    original_results, original_snapshot = _execute(original)
    mirrored_results, mirrored_snapshot = _execute(mirrored)

    assert mirrored_snapshot == _mirror_snapshot(original_snapshot)
    original_trades = _trades(original_results)
    mirrored_trades = _trades(mirrored_results)
    assert len(original_trades) == len(mirrored_trades)
    for original_trade, mirrored_trade in zip(original_trades, mirrored_trades, strict=True):
        assert mirrored_trade.execution_price == (MIRROR_ANCHOR - original_trade.execution_price)
        assert mirrored_trade.execution_quantity == original_trade.execution_quantity
        assert mirrored_trade.resting_order_id == original_trade.resting_order_id
        assert mirrored_trade.aggressor_order_id == original_trade.aggressor_order_id


def test_split_market_order_consumes_same_resting_sequence_and_final_book() -> None:
    prefix: tuple[Command, ...] = (
        _limit(1, Side.SELL, 100, 3, client_id=1),
        _limit(2, Side.SELL, 101, 5, client_id=2),
    )
    unsplit = prefix + (_market(10, Side.BUY, 6, client_id=10),)
    split = prefix + (
        _market(10, Side.BUY, 2, client_id=10),
        _market(11, Side.BUY, 4, client_id=10),
    )

    unsplit_results, unsplit_snapshot = _execute(unsplit)
    split_results, split_snapshot = _execute(split)

    assert _book_without_sequence(unsplit_snapshot) == _book_without_sequence(split_snapshot)
    assert _collapsed_consumption(_trades(unsplit_results)) == _collapsed_consumption(
        _trades(split_results)
    )


def test_far_nonmarketable_level_does_not_change_bounded_incoming_fills() -> None:
    baseline: tuple[Command, ...] = (
        _limit(1, Side.SELL, 100, 3, client_id=1),
        _limit(2, Side.BUY, 100, 2, client_id=2, tif=TimeInForce.IOC),
    )
    with_far_level: tuple[Command, ...] = (
        _limit(1, Side.SELL, 100, 3, client_id=1),
        _limit(9, Side.SELL, 1_000, 50, client_id=9),
        _limit(2, Side.BUY, 100, 2, client_id=2, tif=TimeInForce.IOC),
    )

    baseline_results, _ = _execute(baseline)
    far_results, _ = _execute(with_far_level)

    baseline_trades = _trades(baseline_results)
    far_trades = _trades(far_results)
    assert [
        (
            trade.resting_order_id,
            trade.execution_price,
            trade.execution_quantity,
            trade.resting_remaining,
        )
        for trade in baseline_trades
    ] == [
        (
            trade.resting_order_id,
            trade.execution_price,
            trade.execution_quantity,
            trade.resting_remaining,
        )
        for trade in far_trades
    ]


def test_rejected_prefix_changes_audit_sequence_but_not_valid_book_outcome() -> None:
    valid = _limit(1, Side.BUY, 100, 5, client_id=1)
    rejected = replace(valid, client_id=0, order_id=99)

    with_reject_results, with_reject_snapshot = _execute((rejected, valid))
    valid_results, valid_snapshot = _execute((valid,))

    assert with_reject_results[0].rejected
    assert _book_without_sequence(with_reject_snapshot) == _book_without_sequence(valid_snapshot)
    assert with_reject_snapshot.last_sequence == valid_snapshot.last_sequence + 1
    with_reject_order = with_reject_snapshot.bids[0].orders[0]
    valid_order = valid_snapshot.bids[0].orders[0]
    assert with_reject_order.priority_sequence == valid_order.priority_sequence + 1
