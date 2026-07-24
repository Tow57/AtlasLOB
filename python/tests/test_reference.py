from __future__ import annotations

from typing import cast

import pytest
from atlaslob.domain import (
    U64_MAX,
    AcceptedEvent,
    BookChangedEvent,
    CanceledEvent,
    CancelOrder,
    DoneEvent,
    DoneReason,
    EngineError,
    Event,
    EventHeader,
    MatchingConfig,
    NewOrder,
    OrderType,
    ReferenceResult,
    RejectedEvent,
    RejectReason,
    ReplacedEvent,
    ReplaceOrder,
    RestedEvent,
    Side,
    TimeInForce,
    TopOfBookLevel,
    TradeEvent,
)
from atlaslob.reference import BookTop, ReferenceEngine

INSTRUMENT = 7


def _limit(
    order_id: int,
    *,
    client_id: int = 1,
    instrument_id: int = INSTRUMENT,
    side: int = Side.BUY,
    price: int | None = 100,
    quantity: int = 5,
    time_in_force: int = TimeInForce.GTC,
) -> NewOrder:
    return NewOrder(
        client_id=client_id,
        order_id=order_id,
        instrument_id=instrument_id,
        side=side,
        order_type=OrderType.LIMIT,
        time_in_force=time_in_force,
        limit_price=price,
        quantity=quantity,
    )


def _market(
    order_id: int,
    *,
    client_id: int = 1,
    instrument_id: int = INSTRUMENT,
    side: int = Side.BUY,
    quantity: int = 5,
    time_in_force: int = TimeInForce.IOC,
) -> NewOrder:
    return NewOrder(
        client_id=client_id,
        order_id=order_id,
        instrument_id=instrument_id,
        side=side,
        order_type=OrderType.MARKET,
        time_in_force=time_in_force,
        limit_price=None,
        quantity=quantity,
    )


def _events(result: ReferenceResult) -> tuple[Event, ...]:
    batch = result.batch
    assert batch is not None
    return batch.events


def _rejection(result: ReferenceResult) -> RejectedEvent:
    events = _events(result)
    assert len(events) == 1
    event = events[0]
    assert isinstance(event, RejectedEvent)
    return event


@pytest.mark.parametrize(
    ("command", "reason", "relevant_order_id"),
    [
        (
            NewOrder(0, 0, 0, 0, 0, 0, None, 0),
            RejectReason.INVALID_CLIENT_ID,
            None,
        ),
        (
            NewOrder(1, 0, 0, 0, 0, 0, None, 0),
            RejectReason.INVALID_ORDER_ID,
            None,
        ),
        (
            NewOrder(1, 1, 0, 0, 0, 0, None, 0),
            RejectReason.INVALID_INSTRUMENT_ID,
            1,
        ),
        (
            NewOrder(1, 1, INSTRUMENT, 0, 0, 0, None, 0),
            RejectReason.INVALID_QUANTITY,
            1,
        ),
        (
            NewOrder(1, 1, INSTRUMENT, 0, 0, 0, None, 1),
            RejectReason.INVALID_SIDE,
            1,
        ),
        (
            NewOrder(1, 1, INSTRUMENT, Side.BUY, 0, 0, None, 1),
            RejectReason.INVALID_ORDER_TYPE,
            1,
        ),
        (
            NewOrder(1, 1, INSTRUMENT, Side.BUY, OrderType.LIMIT, 0, None, 1),
            RejectReason.INVALID_TIME_IN_FORCE,
            1,
        ),
        (
            NewOrder(
                1,
                1,
                INSTRUMENT,
                Side.BUY,
                OrderType.LIMIT,
                TimeInForce.FOK,
                None,
                1,
            ),
            RejectReason.UNSUPPORTED_TIME_IN_FORCE,
            1,
        ),
        (
            NewOrder(
                1,
                1,
                INSTRUMENT,
                Side.BUY,
                OrderType.LIMIT,
                TimeInForce.GTC,
                None,
                1,
            ),
            RejectReason.MISSING_LIMIT_PRICE,
            1,
        ),
        (
            NewOrder(
                1,
                1,
                INSTRUMENT,
                Side.BUY,
                OrderType.LIMIT,
                TimeInForce.GTC,
                0,
                1,
            ),
            RejectReason.INVALID_PRICE,
            1,
        ),
        (
            NewOrder(
                1,
                1,
                INSTRUMENT,
                Side.BUY,
                OrderType.MARKET,
                TimeInForce.GTC,
                100,
                1,
            ),
            RejectReason.UNEXPECTED_LIMIT_PRICE,
            1,
        ),
        (
            NewOrder(
                1,
                1,
                INSTRUMENT,
                Side.BUY,
                OrderType.MARKET,
                TimeInForce.GTC,
                None,
                1,
            ),
            RejectReason.INVALID_ORDER_TYPE_TIME_IN_FORCE,
            1,
        ),
    ],
)
def test_new_validation_precedence_and_relevant_identity(
    command: NewOrder,
    reason: RejectReason,
    relevant_order_id: int | None,
) -> None:
    engine = ReferenceEngine(INSTRUMENT)

    rejection = _rejection(engine.execute(command))

    assert rejection.reason == reason
    assert rejection.order_id == relevant_order_id
    assert rejection.header == EventHeader(1, 0, command.instrument_id)
    assert engine.next_sequence == 2
    assert engine.empty


@pytest.mark.parametrize(
    ("command", "seed", "reason", "relevant_order_id", "sequence"),
    [
        (CancelOrder(0, 0, 0), False, RejectReason.INVALID_CLIENT_ID, None, 1),
        (CancelOrder(1, 0, 0), False, RejectReason.INVALID_ORDER_ID, None, 1),
        (CancelOrder(1, 1, 0), False, RejectReason.INVALID_INSTRUMENT_ID, 1, 1),
        (CancelOrder(1, 1, INSTRUMENT + 1), True, RejectReason.UNKNOWN_INSTRUMENT, 1, 2),
        (CancelOrder(1, 99, INSTRUMENT), True, RejectReason.UNKNOWN_ORDER_ID, 99, 2),
        (CancelOrder(2, 1, INSTRUMENT), True, RejectReason.OWNERSHIP_MISMATCH, 1, 2),
    ],
)
def test_cancel_validation_precedence_and_submitted_identity(
    command: CancelOrder,
    seed: bool,
    reason: RejectReason,
    relevant_order_id: int | None,
    sequence: int,
) -> None:
    engine = ReferenceEngine(INSTRUMENT)
    if seed:
        assert engine.execute(_limit(1, client_id=1)).committed

    rejection = _rejection(engine.execute(command))

    assert (rejection.reason, rejection.order_id) == (reason, relevant_order_id)
    assert rejection.header == EventHeader(sequence, 0, command.instrument_id)


def test_state_validation_precedence_is_route_then_limits_tick_and_identity() -> None:
    engine = ReferenceEngine(
        INSTRUMENT,
        MatchingConfig(max_order_quantity=10, tick_increment=5, max_active_orders=5),
    )
    first = engine.execute(_limit(1, quantity=2))
    assert first.committed

    wrong_route = _rejection(
        engine.execute(
            _limit(
                1,
                instrument_id=INSTRUMENT + 1,
                price=102,
                quantity=11,
            )
        )
    )
    too_large = _rejection(engine.execute(_limit(2, price=102, quantity=11)))
    bad_tick = _rejection(engine.execute(_limit(2, price=102, quantity=2)))
    duplicate = _rejection(engine.execute(_limit(1, price=105, quantity=2)))

    assert wrong_route.reason == RejectReason.UNKNOWN_INSTRUMENT
    assert too_large.reason == RejectReason.QUANTITY_OUT_OF_RANGE
    assert bad_tick.reason == RejectReason.INVALID_TICK
    assert duplicate.reason == RejectReason.DUPLICATE_ORDER_ID
    assert engine.active_order_count == 1


def test_best_price_then_fifo_matching_and_exact_event_order() -> None:
    engine = ReferenceEngine(INSTRUMENT)
    assert engine.execute(_limit(1, price=100, quantity=5)).committed
    assert engine.execute(_limit(2, price=101, quantity=7)).committed
    assert engine.execute(_limit(3, price=101, quantity=11)).committed

    result = engine.execute(_limit(4, client_id=9, side=Side.SELL, price=100, quantity=10))
    events = _events(result)

    assert tuple(type(event) for event in events) == (
        AcceptedEvent,
        TradeEvent,
        TradeEvent,
        DoneEvent,
        BookChangedEvent,
    )
    first_trade = events[1]
    second_trade = events[2]
    terminal = events[3]
    assert isinstance(first_trade, TradeEvent)
    assert isinstance(second_trade, TradeEvent)
    assert isinstance(terminal, DoneEvent)
    assert (
        first_trade.resting_order_id,
        first_trade.execution_price,
        first_trade.execution_quantity,
        first_trade.aggressor_remaining,
        first_trade.resting_remaining,
    ) == (2, 101, 7, 3, 0)
    assert (
        second_trade.resting_order_id,
        second_trade.execution_price,
        second_trade.execution_quantity,
        second_trade.aggressor_remaining,
        second_trade.resting_remaining,
    ) == (3, 101, 3, 0, 8)
    assert terminal == DoneEvent(terminal.header, 4, DoneReason.FILLED, 0)
    assert tuple(event.header.event_index for event in events) == tuple(range(5))

    snapshot = engine.snapshot()
    assert tuple(level.price for level in snapshot.bids) == (101, 100)
    assert tuple(order.order_id for order in snapshot.bids[0].orders) == (3,)
    assert snapshot.bids[0].aggregate_quantity == 8
    assert snapshot.bids[0].orders[0].priority_sequence == 3
    assert tuple(order.order_id for order in snapshot.bids[1].orders) == (1,)
    assert engine.top() == BookTop(TopOfBookLevel(101, 8), None)
    assert engine.validate_invariants()


def test_ioc_and_market_terminal_reasons_do_not_rest_residuals() -> None:
    engine = ReferenceEngine(INSTRUMENT)

    ioc = engine.execute(
        _limit(
            1,
            side=Side.BUY,
            price=100,
            quantity=5,
            time_in_force=TimeInForce.IOC,
        )
    )
    market = engine.execute(_market(2, side=Side.SELL, quantity=7))

    ioc_events = _events(ioc)
    market_events = _events(market)
    assert tuple(type(event) for event in ioc_events) == (AcceptedEvent, DoneEvent)
    assert tuple(type(event) for event in market_events) == (AcceptedEvent, DoneEvent)
    assert ioc_events[1] == DoneEvent(
        ioc_events[1].header,
        1,
        DoneReason.IOC_RESIDUAL_CANCELED,
        5,
    )
    assert market_events[1] == DoneEvent(
        market_events[1].header,
        2,
        DoneReason.MARKET_EXHAUSTED,
        7,
    )
    assert engine.empty
    assert engine.top() == BookTop(None, None)


def test_cancel_ownership_unknown_and_top_change_semantics() -> None:
    engine = ReferenceEngine(INSTRUMENT)
    engine.execute(_limit(1, client_id=11, price=100, quantity=3))
    engine.execute(_limit(2, client_id=12, price=101, quantity=4))

    wrong_owner = _rejection(engine.execute(CancelOrder(99, 1, INSTRUMENT)))
    unknown = _rejection(engine.execute(CancelOrder(11, 99, INSTRUMENT)))
    non_top = engine.execute(CancelOrder(11, 1, INSTRUMENT))
    top = engine.execute(CancelOrder(12, 2, INSTRUMENT))

    assert wrong_owner.reason == RejectReason.OWNERSHIP_MISMATCH
    assert unknown.reason == RejectReason.UNKNOWN_ORDER_ID
    assert tuple(type(event) for event in _events(non_top)) == (
        AcceptedEvent,
        CanceledEvent,
        DoneEvent,
    )
    assert tuple(type(event) for event in _events(top)) == (
        AcceptedEvent,
        CanceledEvent,
        DoneEvent,
        BookChangedEvent,
    )
    assert engine.empty


def test_replace_is_cancel_then_new_with_new_fifo_priority() -> None:
    engine = ReferenceEngine(INSTRUMENT)
    engine.execute(_limit(1, client_id=11, price=100, quantity=5))
    engine.execute(_limit(2, client_id=12, price=100, quantity=7))

    result = engine.execute(
        ReplaceOrder(
            client_id=11,
            old_order_id=1,
            new_order_id=3,
            instrument_id=INSTRUMENT,
            new_limit_price=100,
            new_quantity=5,
        )
    )
    events = _events(result)

    assert tuple(type(event) for event in events) == (
        AcceptedEvent,
        ReplacedEvent,
        CanceledEvent,
        DoneEvent,
        RestedEvent,
    )
    assert events[1] == ReplacedEvent(events[1].header, 1, 3)
    assert events[2] == CanceledEvent(events[2].header, 1, 5)
    assert events[3] == DoneEvent(events[3].header, 1, DoneReason.REPLACED, 5)
    snapshot = engine.snapshot()
    level = snapshot.bids[0]
    assert level.aggregate_quantity == 12
    assert tuple(order.order_id for order in level.orders) == (2, 3)
    assert tuple(order.priority_sequence for order in level.orders) == (2, 3)
    assert 1 not in {order.order_id for order in level.orders}


def test_aggressive_replace_orders_old_lifecycle_before_trades() -> None:
    engine = ReferenceEngine(INSTRUMENT)
    engine.execute(_limit(1, client_id=11, side=Side.BUY, price=90, quantity=4))
    engine.execute(_limit(2, client_id=12, side=Side.SELL, price=100, quantity=3))

    result = engine.execute(ReplaceOrder(11, 1, 3, INSTRUMENT, new_limit_price=105, new_quantity=4))
    events = _events(result)

    assert tuple(type(event) for event in events) == (
        AcceptedEvent,
        ReplacedEvent,
        CanceledEvent,
        DoneEvent,
        TradeEvent,
        RestedEvent,
        BookChangedEvent,
    )
    trade = events[4]
    rested = events[5]
    assert isinstance(trade, TradeEvent)
    assert isinstance(rested, RestedEvent)
    assert (trade.aggressor_order_id, trade.resting_order_id) == (3, 2)
    assert (trade.execution_price, trade.execution_quantity) == (100, 3)
    assert (rested.order_id, rested.remaining_quantity, rested.price) == (3, 1, 105)
    assert engine.top() == BookTop(TopOfBookLevel(105, 1), None)


def test_replace_validation_uses_documented_relevant_id_and_precedence() -> None:
    engine = ReferenceEngine(INSTRUMENT)
    engine.execute(_limit(10, client_id=4))

    zero_new = _rejection(engine.execute(ReplaceOrder(4, 10, 0, INSTRUMENT, 100, 1)))
    same_id = _rejection(engine.execute(ReplaceOrder(4, 10, 10, INSTRUMENT, 100, 1)))
    wrong_route = _rejection(engine.execute(ReplaceOrder(4, 10, 11, INSTRUMENT + 1, 100, 1)))
    unknown_old = _rejection(engine.execute(ReplaceOrder(4, 99, 11, INSTRUMENT, 100, 1)))

    assert (zero_new.reason, zero_new.order_id) == (RejectReason.INVALID_ORDER_ID, None)
    assert (same_id.reason, same_id.order_id) == (
        RejectReason.INVALID_REPLACEMENT_ID,
        10,
    )
    assert (wrong_route.reason, wrong_route.order_id) == (
        RejectReason.UNKNOWN_INSTRUMENT,
        10,
    )
    assert (unknown_old.reason, unknown_old.order_id) == (
        RejectReason.UNKNOWN_ORDER_ID,
        99,
    )


@pytest.mark.parametrize(
    ("command", "reason", "relevant_order_id"),
    [
        (
            ReplaceOrder(0, 10, 11, INSTRUMENT, 100, 1),
            RejectReason.INVALID_CLIENT_ID,
            10,
        ),
        (
            ReplaceOrder(4, 0, 11, INSTRUMENT, 100, 1),
            RejectReason.INVALID_ORDER_ID,
            None,
        ),
        (
            ReplaceOrder(4, 10, 0, INSTRUMENT, 100, 1),
            RejectReason.INVALID_ORDER_ID,
            None,
        ),
        (
            ReplaceOrder(4, 10, 10, INSTRUMENT, 100, 1),
            RejectReason.INVALID_REPLACEMENT_ID,
            10,
        ),
        (
            ReplaceOrder(4, 10, 11, 0, 100, 1),
            RejectReason.INVALID_INSTRUMENT_ID,
            10,
        ),
        (
            ReplaceOrder(4, 10, 11, INSTRUMENT, 100, 0),
            RejectReason.INVALID_QUANTITY,
            11,
        ),
        (
            ReplaceOrder(4, 10, 11, INSTRUMENT, 0, 1),
            RejectReason.INVALID_PRICE,
            11,
        ),
        (
            ReplaceOrder(4, 10, 11, INSTRUMENT + 1, 100, 1),
            RejectReason.UNKNOWN_INSTRUMENT,
            10,
        ),
        (
            ReplaceOrder(4, 10, 11, INSTRUMENT, 100, 11),
            RejectReason.QUANTITY_OUT_OF_RANGE,
            11,
        ),
        (
            ReplaceOrder(4, 10, 11, INSTRUMENT, 102, 1),
            RejectReason.INVALID_TICK,
            11,
        ),
        (
            ReplaceOrder(4, 99, 11, INSTRUMENT, 100, 1),
            RejectReason.UNKNOWN_ORDER_ID,
            99,
        ),
        (
            ReplaceOrder(9, 10, 11, INSTRUMENT, 100, 1),
            RejectReason.OWNERSHIP_MISMATCH,
            10,
        ),
        (
            ReplaceOrder(4, 10, 20, INSTRUMENT, 100, 1),
            RejectReason.INVALID_REPLACEMENT_ID,
            20,
        ),
    ],
)
def test_replace_complete_precedence_matrix_is_atomic(
    command: ReplaceOrder,
    reason: RejectReason,
    relevant_order_id: int | None,
) -> None:
    engine = ReferenceEngine(
        INSTRUMENT,
        MatchingConfig(max_order_quantity=10, tick_increment=5, max_active_orders=4),
    )
    assert engine.execute(_limit(10, client_id=4, price=100, quantity=2)).committed
    assert engine.execute(_limit(20, client_id=5, side=Side.SELL, price=105, quantity=2)).committed
    before = engine.snapshot()

    rejection = _rejection(engine.execute(command))
    after = engine.snapshot()

    assert (rejection.reason, rejection.order_id) == (reason, relevant_order_id)
    assert rejection.header == EventHeader(3, 0, command.instrument_id)
    assert after.bids == before.bids
    assert after.asks == before.asks
    assert after.active_order_count == before.active_order_count


def test_projected_active_capacity_allows_terminal_command_at_limit() -> None:
    engine = ReferenceEngine(
        INSTRUMENT,
        MatchingConfig(max_order_quantity=10, tick_increment=1, max_active_orders=1),
    )
    engine.execute(_limit(1, side=Side.BUY, quantity=2))
    before_orders = engine.snapshot().bids

    rejected = _rejection(engine.execute(_limit(2, price=99, quantity=1)))
    terminal = engine.execute(_market(3, side=Side.SELL, quantity=1))

    assert rejected.reason == RejectReason.CAPACITY_EXCEEDED
    assert engine.active_order_count == 1
    assert before_orders[0].orders[0].order_id == engine.snapshot().bids[0].orders[0].order_id
    terminal_events = _events(terminal)
    assert tuple(type(event) for event in terminal_events) == (
        AcceptedEvent,
        TradeEvent,
        DoneEvent,
        BookChangedEvent,
    )
    assert engine.top() == BookTop(TopOfBookLevel(100, 1), None)


def test_u64_level_aggregate_overflow_rejects_without_book_mutation() -> None:
    engine = ReferenceEngine(
        INSTRUMENT,
        MatchingConfig(
            max_order_quantity=U64_MAX,
            tick_increment=1,
            max_active_orders=2,
        ),
    )
    engine.execute(_limit(1, price=100, quantity=U64_MAX))
    before = engine.snapshot().bids

    rejection = _rejection(engine.execute(_limit(2, price=100, quantity=1)))

    assert (rejection.reason, rejection.order_id) == (
        RejectReason.CAPACITY_EXCEEDED,
        2,
    )
    assert engine.snapshot().bids == before
    assert engine.active_order_count == 1
    assert engine.validate_invariants()


def test_replace_projection_subtracts_old_before_u64_aggregate_check() -> None:
    engine = ReferenceEngine(
        INSTRUMENT,
        MatchingConfig(
            max_order_quantity=U64_MAX,
            tick_increment=1,
            max_active_orders=2,
        ),
    )
    engine.execute(_limit(1, client_id=1, quantity=U64_MAX - 5))
    engine.execute(_limit(2, client_id=2, quantity=5))
    before = engine.snapshot().bids

    rejected = _rejection(
        engine.execute(
            ReplaceOrder(
                client_id=1,
                old_order_id=1,
                new_order_id=3,
                instrument_id=INSTRUMENT,
                new_limit_price=100,
                new_quantity=U64_MAX - 4,
            )
        )
    )
    after_rejection = engine.snapshot().bids
    allowed = engine.execute(
        ReplaceOrder(
            client_id=1,
            old_order_id=1,
            new_order_id=3,
            instrument_id=INSTRUMENT,
            new_limit_price=100,
            new_quantity=U64_MAX - 5,
        )
    )

    assert (rejected.reason, rejected.order_id) == (
        RejectReason.CAPACITY_EXCEEDED,
        3,
    )
    assert after_rejection == before
    assert allowed.committed
    assert engine.snapshot().bids[0].aggregate_quantity == U64_MAX
    assert tuple(order.order_id for order in engine.snapshot().bids[0].orders) == (2, 3)
    assert tuple(order.priority_sequence for order in engine.snapshot().bids[0].orders) == (2, 4)


def test_sequence_exhaustion_is_sticky_and_does_not_create_a_batch() -> None:
    engine = ReferenceEngine(INSTRUMENT, first_sequence=U64_MAX)
    invalid = _limit(0)

    maximum = engine.execute(invalid)
    exhausted = engine.execute(_limit(1))
    sticky = engine.execute(CancelOrder(1, 1, INSTRUMENT))

    assert _rejection(maximum).reason == RejectReason.INVALID_ORDER_ID
    assert maximum.batch is not None
    assert maximum.batch.command_sequence == U64_MAX
    assert exhausted.batch is None
    assert exhausted.error == EngineError.SEQUENCE_EXHAUSTED
    assert sticky.batch is None
    assert sticky.error == EngineError.SEQUENCE_EXHAUSTED
    assert engine.sequence_exhausted
    assert engine.next_sequence == 0
    assert engine.snapshot().last_sequence == U64_MAX
    assert engine.empty


def test_adapter_range_failure_precedes_sequence_issue() -> None:
    engine = ReferenceEngine(INSTRUMENT)
    outside_u64 = _limit(U64_MAX + 1)

    with pytest.raises(ValueError, match="order_id"):
        engine.execute(outside_u64)

    assert engine.next_sequence == 1
    assert engine.snapshot().last_sequence == 0


def test_boolean_adapter_scalar_is_not_treated_as_an_integer() -> None:
    engine = ReferenceEngine(INSTRUMENT)
    boolean_id = _limit(cast(int, True))

    with pytest.raises(ValueError, match="order_id"):
        engine.execute(boolean_id)

    assert engine.next_sequence == 1


def test_internal_exception_poisoning_prevents_continuation(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    engine = ReferenceEngine(INSTRUMENT)

    def fail_match(
        _engine: ReferenceEngine,
        _order: NewOrder,
        _sequence: int,
        _events: list[Event],
    ) -> int:
        raise MemoryError("injected reference allocation failure")

    monkeypatch.setattr(ReferenceEngine, "_match", fail_match)
    with pytest.raises(MemoryError, match="injected"):
        engine.execute(_limit(1))

    with pytest.raises(RuntimeError, match="poisoned"):
        engine.execute(_limit(2))
    assert not engine.validate_invariants()


def test_fresh_snapshot_and_digest_match_canonical_empty_golden() -> None:
    engine = ReferenceEngine(INSTRUMENT)

    snapshot = engine.snapshot()

    assert snapshot.last_sequence == 0
    assert not snapshot.sequence_exhausted
    assert snapshot.active_order_count == 0
    assert snapshot.bids == ()
    assert snapshot.asks == ()
    assert engine.state_digest() == (
        "19a8ffaeb1bee1b8aa87123c3508af1bfa87e3d634a09ba491e1b85fe597b219"
    )
    assert engine.invariant_errors() == ()
