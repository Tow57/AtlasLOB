from __future__ import annotations

import pytest
from atlaslob.domain import (
    U32_MAX,
    U64_MAX,
    CancelOrder,
    EngineError,
    InstrumentConfig,
    MatchingConfig,
    MultiInstrumentEngineConfig,
    NewOrder,
    OrderType,
    ReferenceResult,
    RejectedEvent,
    RejectReason,
    ReplaceOrder,
    Side,
    TimeInForce,
)
from atlaslob.router import ReferenceRouter


def _catalog() -> tuple[InstrumentConfig, ...]:
    return (
        InstrumentConfig(
            1,
            MatchingConfig(
                max_order_quantity=100,
                tick_increment=1,
                max_active_orders=8,
            ),
        ),
        InstrumentConfig(
            2,
            MatchingConfig(
                max_order_quantity=50,
                tick_increment=5,
                max_active_orders=8,
            ),
        ),
    )


def _limit(
    order_id: int,
    instrument_id: int,
    side: Side,
    price: int,
    quantity: int = 5,
    client_id: int = 1,
) -> NewOrder:
    return NewOrder(
        client_id=client_id,
        order_id=order_id,
        instrument_id=instrument_id,
        side=side,
        order_type=OrderType.LIMIT,
        time_in_force=TimeInForce.GTC,
        limit_price=price,
        quantity=quantity,
    )


def _reason(result: ReferenceResult) -> RejectReason:
    batch = result.batch
    assert batch is not None
    event = batch.events[0]
    assert isinstance(event, RejectedEvent)
    return event.reason


def test_catalog_is_validated_eagerly_and_stored_canonically() -> None:
    router = ReferenceRouter(tuple(reversed(_catalog())))

    assert tuple(entry.instrument_id for entry in router.catalog) == (1, 2)
    assert router.top(3) is None
    assert router.snapshot(3) is None
    assert router.snapshot().active_order_count == 0
    assert tuple(book.instrument_id for book in router.snapshot().instruments) == (1, 2)

    with pytest.raises(ValueError, match="at least one"):
        ReferenceRouter(())
    with pytest.raises(ValueError, match="unique"):
        ReferenceRouter((InstrumentConfig(1), InstrumentConfig(1)))
    with pytest.raises(TypeError, match="tuple"):
        ReferenceRouter(list(_catalog()))  # type: ignore[arg-type]


def test_global_sequence_spans_books_and_domain_rejections() -> None:
    router = ReferenceRouter(_catalog())
    results = (
        router.execute(_limit(1, 1, Side.BUY, 100)),
        router.execute(_limit(2, 2, Side.SELL, 105)),
        router.execute(_limit(3, 99, Side.BUY, 100)),
        router.execute(_limit(4, 1, Side.BUY, 100, quantity=0)),
    )

    assert tuple(result.batch.command_sequence for result in results if result.batch) == (
        1,
        2,
        3,
        4,
    )
    assert _reason(results[2]) == RejectReason.UNKNOWN_INSTRUMENT
    assert _reason(results[3]) == RejectReason.INVALID_QUANTITY
    assert router.next_sequence == 5
    assert router.snapshot().last_sequence == 4


def test_books_do_not_cross_match_and_ids_are_globally_unique() -> None:
    router = ReferenceRouter(_catalog())

    assert router.execute(_limit(10, 1, Side.BUY, 100)).committed
    assert router.execute(_limit(11, 2, Side.SELL, 95)).committed
    duplicate = router.execute(_limit(10, 2, Side.BUY, 90))

    assert _reason(duplicate) == RejectReason.DUPLICATE_ORDER_ID
    assert router.active_order_count == 2
    first_top = router.top(1)
    second_top = router.top(2)
    assert first_top is not None
    assert first_top.best_bid is not None
    assert second_top is not None
    assert second_top.best_ask is not None
    assert router.validate_invariants()


def test_cancel_precedence_uses_global_identity_after_routing() -> None:
    router = ReferenceRouter(_catalog())
    router.execute(_limit(20, 1, Side.BUY, 100, client_id=7))

    unknown_route = router.execute(CancelOrder(7, 20, 99))
    wrong_owner = router.execute(CancelOrder(8, 20, 2))
    wrong_instrument = router.execute(CancelOrder(7, 20, 2))
    missing = router.execute(CancelOrder(7, 999, 1))

    assert _reason(unknown_route) == RejectReason.UNKNOWN_INSTRUMENT
    assert _reason(wrong_owner) == RejectReason.OWNERSHIP_MISMATCH
    assert _reason(wrong_instrument) == RejectReason.INSTRUMENT_MISMATCH
    assert _reason(missing) == RejectReason.UNKNOWN_ORDER_ID


def test_replace_enforces_routed_policy_then_global_identity() -> None:
    router = ReferenceRouter(_catalog())
    router.execute(_limit(30, 1, Side.BUY, 100, client_id=7))
    router.execute(_limit(31, 2, Side.BUY, 90, client_id=9))

    bad_tick_wins = router.execute(ReplaceOrder(7, 30, 32, 2, 101, 5))
    wrong_instrument = router.execute(ReplaceOrder(7, 30, 32, 2, 100, 5))
    globally_active_new_id = router.execute(ReplaceOrder(7, 30, 31, 1, 101, 5))
    replaced = router.execute(ReplaceOrder(7, 30, 32, 1, 101, 5))

    assert _reason(bad_tick_wins) == RejectReason.INVALID_TICK
    assert _reason(wrong_instrument) == RejectReason.INSTRUMENT_MISMATCH
    assert _reason(globally_active_new_id) == RejectReason.INVALID_REPLACEMENT_ID
    assert replaced.committed
    assert router.execute(CancelOrder(7, 30, 1)).rejected
    assert router.execute(CancelOrder(7, 32, 1)).committed


def test_post_command_global_capacity_allows_terminal_aggressor() -> None:
    router = ReferenceRouter(
        _catalog(),
        MultiInstrumentEngineConfig(max_total_active_orders=1),
    )
    assert router.execute(_limit(40, 1, Side.BUY, 100)).committed

    fully_crossing = router.execute(_limit(41, 1, Side.SELL, 100))
    assert fully_crossing.committed
    assert router.active_order_count == 0

    assert router.execute(_limit(42, 1, Side.BUY, 90)).committed
    over_capacity = router.execute(_limit(43, 2, Side.BUY, 90))
    assert _reason(over_capacity) == RejectReason.CAPACITY_EXCEEDED
    assert router.active_order_count == 1


def test_terminal_id_can_be_reused_on_another_instrument() -> None:
    router = ReferenceRouter(_catalog())
    router.execute(_limit(50, 1, Side.BUY, 100, client_id=5))
    assert router.execute(CancelOrder(5, 50, 1)).committed
    assert router.execute(_limit(50, 2, Side.BUY, 100, client_id=6)).committed
    assert router.snapshot(2).active_order_count == 1  # type: ignore[union-attr]


def test_representation_failure_does_not_consume_sequence() -> None:
    router = ReferenceRouter(_catalog())

    with pytest.raises(ValueError, match="representation"):
        router.execute(_limit(60, 1, Side.BUY, 100, client_id=U32_MAX + 1))

    assert router.next_sequence == 1


def test_global_sequence_exhaustion_is_issued_exactly_once() -> None:
    router = ReferenceRouter(_catalog(), first_sequence=U64_MAX)

    final = router.execute(_limit(70, 99, Side.BUY, 100))
    exhausted = router.execute(_limit(71, 1, Side.BUY, 100))

    assert final.batch is not None
    assert final.batch.command_sequence == U64_MAX
    assert _reason(final) == RejectReason.UNKNOWN_INSTRUMENT
    assert exhausted.error == EngineError.SEQUENCE_EXHAUSTED
    assert router.sequence_exhausted
    assert router.next_sequence == 0
    assert router.snapshot().last_sequence == U64_MAX
