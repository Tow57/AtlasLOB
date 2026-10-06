"""Recovery history integration: these tests require the real compiled extension."""

from __future__ import annotations

from dataclasses import FrozenInstanceError
from pathlib import Path

import pytest
from atlaslob import Engine
from atlaslob.domain import (
    InstrumentConfig,
    NewOrder,
    OrderType,
    Side,
    TimeInForce,
    TradeEvent,
)
from atlaslob.engine import RecoveryError


def order(order_id: int, side: Side, price: int, quantity: int, instrument: int = 7) -> NewOrder:
    return NewOrder(
        client_id=1,
        order_id=order_id,
        instrument_id=instrument,
        side=side,
        order_type=OrderType.LIMIT,
        time_in_force=TimeInForce.GTC,
        limit_price=price,
        quantity=quantity,
    )


def history_log(path: Path) -> tuple[tuple[TradeEvent, ...], str]:
    engine = Engine.create_logged(path, (InstrumentConfig(7), InstrumentConfig(42)))
    commands = (
        order(1, Side.BUY, 2**53 + 1, 7),
        order(2, Side.BUY, 2**53 + 1, 5),
        order(3, Side.SELL, 2**53 + 1, 9),
        order(4, Side.BUY, 123, 2, 42),
        order(5, Side.SELL, 123, 1, 42),
        order(6, Side.BUY, 100, 0),  # A sequenced rejection is not an execution.
    )
    trades = tuple(
        event
        for command in commands
        for event in engine.submit(command).events or ()
        if isinstance(event, TradeEvent)
    )
    digest = engine.state_digest()
    del engine
    return trades, digest


def test_recovered_trades_are_exact_owned_frozen_and_not_live(tmp_path: Path) -> None:
    path = tmp_path / "history.log"
    expected, digest = history_log(path)
    before = path.read_bytes()
    engine = Engine.recover(path, collect_trades=True, max_recovered_trades=len(expected))
    assert engine.recovered_trades == expected
    assert engine.state_digest() == digest
    assert path.read_bytes() == before
    observed = engine.recovered_trades
    assert isinstance(observed, tuple)
    with pytest.raises(AttributeError):
        engine.recovered_trades = ()  # type: ignore[misc]
    with pytest.raises(FrozenInstanceError):
        observed[0].execution_price = 0  # type: ignore[misc]
    assert observed[0].execution_price == 2**53 + 1
    assert observed[-1].execution_price == 123
    engine.submit(order(7, Side.SELL, 2**53 + 1, 1))
    assert engine.recovered_trades == expected
    del engine
    default = Engine.recover(path)
    assert default.recovered_trades is None


def test_empty_history_distinguishes_opt_in(tmp_path: Path) -> None:
    path = tmp_path / "empty.log"
    live = Engine((InstrumentConfig(7),))
    assert live.recovered_trades is None
    logged = Engine.create_logged(path, (InstrumentConfig(7),))
    assert logged.recovered_trades is None
    del logged
    recovered = Engine.recover(path, collect_trades=True)
    assert recovered.recovered_trades == ()


def test_collection_limit_is_visible_and_log_is_untouched(tmp_path: Path) -> None:
    path = tmp_path / "bounded.log"
    expected, _ = history_log(path)
    before = path.read_bytes()
    with pytest.raises(RecoveryError, match="max_recovered_trades"):
        Engine.recover(path, collect_trades=True, max_recovered_trades=len(expected) - 1)
    assert path.read_bytes() == before
    recovered = Engine.recover(path, collect_trades=True)
    assert recovered.recovered_trades == expected


@pytest.mark.parametrize(
    "options",
    [
        {"mode": "fast"},
        {"mode": "diagnostic"},
        {"tail_policy": "valid-prefix"},
        {"snapshot_path": "missing.snapshot"},
        {"snapshot_dir": "missing-directory"},
        {"max_recovered_trades": 0},
        {"max_recovered_trades": -1},
        {"max_recovered_trades": True},
        {"max_recovered_trades": 2**64},
    ],
)
def test_unsupported_options_are_rejected_before_io(
    tmp_path: Path, options: dict[str, object]
) -> None:
    missing = tmp_path / "never-opened.log"
    with pytest.raises(ValueError):
        Engine.recover(missing, collect_trades=True, **options)  # type: ignore[arg-type]
    assert not missing.exists()


def test_collect_flag_requires_bool(tmp_path: Path) -> None:
    with pytest.raises(TypeError, match="collect_trades"):
        Engine.recover(tmp_path / "missing.log", collect_trades=1)  # type: ignore[arg-type]


@pytest.mark.parametrize("damage", ["torn", "checksum"])
def test_damaged_log_is_untouched_and_never_returns_partial_history(
    tmp_path: Path, damage: str
) -> None:
    path = tmp_path / "damaged.log"
    history_log(path)
    original = path.read_bytes()
    damaged = original[:-1] if damage == "torn" else original[:-1] + bytes([original[-1] ^ 1])
    path.write_bytes(damaged)
    with pytest.raises(RecoveryError):
        Engine.recover(path, collect_trades=True)
    assert path.read_bytes() == damaged
