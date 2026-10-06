"""Market Walk uses real matching at every prefix, never fabricated chart data."""

from collections import Counter, defaultdict

from atlaslob import Engine
from atlaslob.domain import (
    CancelOrder,
    InstrumentConfig,
    MatchingConfig,
    NewOrder,
    ReplaceOrder,
    TradeEvent,
)
from atlaslob.ui.scenarios import SCENARIOS, decode, digest, market_walk


def engine() -> Engine:
    return Engine(
        tuple(InstrumentConfig(iid, MatchingConfig(max_active_orders=1_000)) for iid in (7, 42)),
        max_total_active_orders=2_000,
    )


def test_market_walk_definition_and_frozen_commands_are_deterministic() -> None:
    scenario = market_walk()
    assert [s.id for s in SCENARIOS] == [
        "fifo",
        "sweep",
        "replace",
        "ioc",
        "instruments",
        "market-walk",
    ]
    assert len(scenario.steps) == 600
    assert scenario.freeze() == market_walk().freeze() == SCENARIOS[-1].freeze()
    frozen = scenario.freeze()
    assert frozen["sha256"] == digest(frozen["payload"])
    decoded = tuple(decode(step["command"]) for step in frozen["payload"]["steps"])
    assert decoded == tuple(command for _, command in scenario.steps)
    ids = []
    for sequence, (description, command) in enumerate(scenario.steps, 1):
        assert description and command.instrument_id in (7, 42)
        assert command.client_id in (1, 2, 3)
        if isinstance(command, NewOrder):
            ids.append(command.order_id)
            assert command.order_id == sequence
            assert 0 < command.quantity <= 20
        elif isinstance(command, ReplaceOrder):
            ids.append(command.new_order_id)
            assert command.new_order_id == sequence
            assert 0 < command.new_quantity <= 20
        else:
            assert isinstance(command, CancelOrder)
    assert len(ids) == len(set(ids))
    assert Counter(command.instrument_id for command in decoded) == {7: 300, 42: 300}


def test_market_walk_every_native_prefix_is_valid_bounded_and_reproducible() -> None:
    scenario = market_walk()
    frozen_steps = scenario.freeze()["payload"]["steps"]
    direct, frozen = engine(), engine()
    trade_counts: Counter[int] = Counter()
    quantities: Counter[int] = Counter()
    chapter_prices: dict[int, list[int]] = defaultdict(list)
    try:
        assert direct.snapshot().last_sequence == 0
        assert direct.snapshot().active_order_count == 0
        for sequence, ((_, command), step) in enumerate(
            zip(scenario.steps, frozen_steps, strict=True), 1
        ):
            before = direct.snapshot()
            result = direct.submit(command)
            decoded_result = frozen.submit(decode(step["command"]))
            assert result == decoded_result
            assert result.committed and not result.rejected and result.error is None
            assert result.command_sequence == sequence
            after = direct.snapshot()
            assert after == frozen.snapshot()
            assert direct.state_digest() == frozen.state_digest()
            assert after.last_sequence == sequence
            assert after.active_order_count <= 10
            for book in after.instruments:
                assert book.active_order_count <= 6
                assert len(book.bids) + len(book.asks) <= 6
                for level in (*book.bids, *book.asks):
                    assert all(0 < order.remaining_quantity <= 20 for order in level.orders)
                if book.instrument_id != command.instrument_id:
                    assert book == next(
                        old for old in before.instruments if old.instrument_id == book.instrument_id
                    )
            trades = [event for event in result.events or () if isinstance(event, TradeEvent)]
            for trade in trades:
                trade_counts[trade.header.instrument_id] += 1
                quantities[trade.header.instrument_id] += trade.execution_quantity
            offset = (sequence - 1) % 30 + 1
            base = sequence - offset
            if offset in (7, 8):
                assert len(trades) == 2  # Each sweep genuinely consumes two native levels.
            if offset == 7:
                chapter_prices[command.instrument_id].append(trades[0].execution_price)
            if offset == 12:
                assert [trade.resting_order_id for trade in trades] == [base + 10, base + 11]
                assert [trade.execution_quantity for trade in trades] == [6, 1]
            if offset == 17:
                assert len(trades) == 1 and trades[0].execution_quantity == 5
        assert trade_counts == {7: 150, 42: 150}
        assert quantities == {7: 920, 42: 920}
        for prices in chapter_prices.values():
            changes = [right - left for left, right in zip(prices, prices[1:], strict=False)]
            assert any(change > 0 for change in changes)
            assert any(change < 0 for change in changes)
        for book in direct.snapshot().instruments:
            assert book.active_order_count == 4
            assert len(book.bids) == len(book.asks) == 2
            assert book.bids[0].aggregate_quantity == book.asks[0].aggregate_quantity == 16
    finally:
        del direct, frozen
