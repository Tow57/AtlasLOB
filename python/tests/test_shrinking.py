from __future__ import annotations

from atlaslob.domain import (
    CancelOrder,
    Command,
    NewOrder,
    OrderType,
    Side,
    TimeInForce,
)
from atlaslob.shrinking import (
    ShrinkBudget,
    ShrinkContext,
    shrink_failure,
)

INSTRUMENT = 7


def _limit(
    order_id: int,
    side: Side,
    price: int,
    quantity: int,
    *,
    client_id: int,
) -> NewOrder:
    return NewOrder(
        client_id=client_id,
        order_id=order_id,
        instrument_id=INSTRUMENT,
        side=side,
        order_type=OrderType.LIMIT,
        time_in_force=TimeInForce.GTC,
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


def _partial_second_fill_signature(
    commands: tuple[Command, ...],
    _remaining: float | None = None,
) -> str | None:
    resting = [
        command
        for command in commands
        if isinstance(command, NewOrder)
        and command.side == Side.SELL
        and command.order_type == OrderType.LIMIT
        and command.time_in_force == TimeInForce.GTC
        and command.limit_price is not None
    ]
    markets = [
        command
        for command in commands
        if isinstance(command, NewOrder)
        and command.side == Side.BUY
        and command.order_type == OrderType.MARKET
    ]
    if len(resting) < 2 or not markets:
        return None
    first, second = resting[-2:]
    market = markets[-1]
    if (
        first.limit_price == second.limit_price
        and first.quantity < market.quantity < first.quantity + second.quantity
    ):
        return "fifo_partial_second_fill"
    return None


def test_shrinker_deletes_noise_and_simplifies_fields_deterministically() -> None:
    commands: tuple[Command, ...] = (
        _limit(900, Side.BUY, 50, 17, client_id=88),
        CancelOrder(88, 900, INSTRUMENT),
        _limit(400, Side.SELL, 900, 7, client_id=40),
        _limit(800, Side.SELL, 900, 11, client_id=80),
        _market(999, Side.BUY, 13, client_id=99),
        _limit(1000, Side.BUY, 10, 3, client_id=100),
    )
    context = ShrinkContext(
        routed_instrument=INSTRUMENT,
        tick_increment=1,
        max_quantity=1_000,
    )

    first = shrink_failure(
        commands,
        _partial_second_fill_signature,
        "fifo_partial_second_fill",
        context=context,
    )
    second = shrink_failure(
        commands,
        _partial_second_fill_signature,
        "fifo_partial_second_fill",
        context=context,
    )

    assert first.commands == second.commands
    assert first.signature == second.signature
    assert first.evaluations == second.evaluations
    assert first.cache_hits == second.cache_hits
    assert first.completed_stages == second.completed_stages
    assert first.original_count == 6
    assert len(first.commands) == 3
    assert _partial_second_fill_signature(first.commands) == first.signature
    assert not first.budget_exhausted
    assert first.completed_stages[-1] == "final_deletion"

    resting = [
        command
        for command in first.commands
        if isinstance(command, NewOrder) and command.side == Side.SELL
    ]
    market = next(
        command
        for command in first.commands
        if isinstance(command, NewOrder) and command.order_type == OrderType.MARKET
    )
    new_commands = [command for command in first.commands if isinstance(command, NewOrder)]
    assert len(new_commands) == len(first.commands)
    assert [command.order_id for command in new_commands] == [1, 2, 3]
    assert [command.client_id for command in new_commands] == [1, 2, 3]
    assert {command.limit_price for command in resting} == {1}
    assert resting[0].quantity < market.quantity < sum(command.quantity for command in resting)


def test_shrinker_reports_budget_exhaustion_without_losing_reproducer() -> None:
    commands: tuple[Command, ...] = (
        _limit(1, Side.SELL, 100, 1, client_id=1),
        _limit(2, Side.SELL, 100, 2, client_id=2),
        _market(3, Side.BUY, 2, client_id=3),
    )

    result = shrink_failure(
        commands,
        _partial_second_fill_signature,
        "fifo_partial_second_fill",
        context=ShrinkContext(INSTRUMENT),
        budget=ShrinkBudget(max_evaluations=1),
    )

    assert result.budget_exhausted
    assert result.evaluations == 1
    assert result.commands == commands


def test_shrinker_rejects_a_nonreproducing_original() -> None:
    commands = (_limit(1, Side.SELL, 100, 1, client_id=1),)

    try:
        shrink_failure(
            commands,
            _partial_second_fill_signature,
            "fifo_partial_second_fill",
            context=ShrinkContext(INSTRUMENT),
        )
    except ValueError as error:
        assert "does not reproduce" in str(error)
    else:
        raise AssertionError("nonreproducing input unexpectedly entered the reducer")
