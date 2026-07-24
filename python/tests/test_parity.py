from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

import pytest
from atlaslob.canonical import event_digest
from atlaslob.domain import (
    U64_MAX,
    BookSnapshot,
    CancelOrder,
    Command,
    MatchingConfig,
    NewOrder,
    OrderType,
    ReferenceResult,
    ReplaceOrder,
    Side,
    TimeInForce,
    command_type,
)
from atlaslob.native import NativeInputConfig, NativeResultRecord, run_native
from atlaslob.reference import BookTop, ReferenceEngine

INSTRUMENT = 7


@dataclass(frozen=True, slots=True)
class _ExpectedStep:
    result: ReferenceResult
    snapshot: BookSnapshot
    top: BookTop
    next_sequence: int
    state_digest: str


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
        Path("build/fix-debug/atlas_diff_native.exe"),
        Path("build/dev-gcc/atlas_diff_native.exe"),
        Path("build/dev-gcc/atlas_diff_native"),
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError(
        "atlas_diff_native has not been built in a standard development location; "
        "build it or set ATLAS_DIFF_NATIVE"
    )


def test_native_executable_is_required_and_explicit_path_is_authoritative(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    missing = tmp_path / "missing-atlas-diff-native"
    fallback = tmp_path / "build" / "fix-debug" / "atlas_diff_native.exe"
    monkeypatch.chdir(tmp_path)
    monkeypatch.delenv("ATLAS_DIFF_NATIVE", raising=False)

    with pytest.raises(FileNotFoundError, match="has not been built"):
        _executable()

    fallback.parent.mkdir(parents=True)
    fallback.touch()
    monkeypatch.setenv("ATLAS_DIFF_NATIVE", str(missing))

    with pytest.raises(FileNotFoundError, match="ATLAS_DIFF_NATIVE"):
        _executable()

    selected = tmp_path / "selected-atlas-diff-native"
    selected.touch()
    monkeypatch.setenv("ATLAS_DIFF_NATIVE", str(selected))
    assert _executable() == selected.resolve()


def _limit(
    order_id: int,
    side: Side,
    price: int,
    quantity: int,
    *,
    client_id: int = 11,
    tif: TimeInForce = TimeInForce.GTC,
    instrument_id: int = INSTRUMENT,
) -> NewOrder:
    return NewOrder(
        client_id=client_id,
        order_id=order_id,
        instrument_id=instrument_id,
        side=side,
        order_type=OrderType.LIMIT,
        time_in_force=tif,
        limit_price=price,
        quantity=quantity,
    )


def _market(
    order_id: int,
    side: Side,
    quantity: int,
    *,
    client_id: int = 22,
) -> NewOrder:
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


def _capture_reference(
    commands: tuple[Command, ...],
    config: MatchingConfig,
) -> tuple[_ExpectedStep, ...]:
    engine = ReferenceEngine(INSTRUMENT, config)
    output: list[_ExpectedStep] = []
    for command in commands:
        result = engine.execute(command)
        output.append(
            _ExpectedStep(
                result=result,
                snapshot=engine.snapshot(),
                top=engine.top(),
                next_sequence=engine.next_sequence,
                state_digest=engine.state_digest(),
            )
        )
    return tuple(output)


def _compare_result(native: NativeResultRecord, expected: _ExpectedStep, command: Command) -> None:
    assert native.command_type == command_type(command)
    assert native.snapshot == expected.snapshot
    assert native.state.active_order_count == expected.snapshot.active_order_count
    assert native.state.empty == (expected.snapshot.active_order_count == 0)
    assert native.state.next_sequence == expected.next_sequence
    assert native.state.sequence_exhausted == expected.snapshot.sequence_exhausted
    assert native.state.best_bid == expected.top.best_bid
    assert native.state.best_ask == expected.top.best_ask
    assert native.state.state_digest == expected.state_digest

    if expected.result.batch is None:
        assert native.outcome == "engine_error"
        assert native.command_sequence is None
        assert native.engine_error == expected.result.error
        assert native.event_digest is None
        assert native.events is None
        return

    batch = expected.result.batch
    assert native.outcome == ("rejected" if batch.rejected else "committed")
    assert native.command_sequence == batch.command_sequence
    assert native.engine_error is None
    assert native.events == batch.events
    assert native.event_digest == event_digest(batch)


def _assert_parity(commands: tuple[Command, ...], config: MatchingConfig) -> None:
    # The independent result is deliberately produced before the native process is invoked.
    expected = _capture_reference(commands, config)
    native = run_native(
        _executable(),
        NativeInputConfig(INSTRUMENT, config, snapshot_interval=1),
        commands,
    )

    assert native.returncode == 0
    assert native.stderr == ""
    assert native.transcript.error is None
    assert native.transcript.config is not None
    assert native.transcript.config.input.engine == config
    assert len(native.transcript.results) == len(expected)
    for command, native_result, expected_step in zip(
        commands, native.transcript.results, expected, strict=True
    ):
        _compare_result(native_result, expected_step, command)

    assert native.transcript.final is not None
    final = native.transcript.final
    assert final.commands_processed == len(commands)
    assert final.snapshot == expected[-1].snapshot
    assert final.state.state_digest == expected[-1].state_digest


@pytest.mark.parametrize(
    ("commands", "config"),
    [
        pytest.param(
            (
                _limit(1, Side.BUY, 100, 5, client_id=10),
                _limit(2, Side.BUY, 100, 7, client_id=11),
                _limit(3, Side.BUY, 99, 9, client_id=12),
            ),
            MatchingConfig(),
            id="rest-fifo-and-coalesced-bbo",
        ),
        pytest.param(
            (
                _limit(1, Side.BUY, 102, 2, client_id=11),
                _limit(2, Side.BUY, 101, 3, client_id=12),
                _limit(3, Side.BUY, 100, 7, client_id=13),
                _limit(100, Side.SELL, 101, 8, client_id=20),
            ),
            MatchingConfig(),
            id="multi-level-sweep-and-gtc-residual",
        ),
        pytest.param(
            (
                _limit(1, Side.SELL, 100, 5, client_id=11),
                _limit(2, Side.SELL, 100, 7, client_id=12),
                _market(100, Side.BUY, 8, client_id=20),
            ),
            MatchingConfig(),
            id="same-level-fifo-partial-priority",
        ),
        pytest.param(
            (
                _limit(1, Side.SELL, 100, 5, client_id=11),
                _limit(2, Side.SELL, 100, 7, client_id=12),
                _market(100, Side.BUY, 3),
                CancelOrder(11, 1, INSTRUMENT),
                _limit(101, Side.BUY, 100, 20, tif=TimeInForce.IOC),
                _market(102, Side.BUY, 4),
            ),
            MatchingConfig(),
            id="partial-cancel-ioc-and-market-terminal-reasons",
        ),
        pytest.param(
            (
                _limit(1, Side.BUY, 100, 5),
                _limit(2, Side.BUY, 99, 4),
                _market(3, Side.SELL, 5),
                _limit(2, Side.BUY, 99, 4),
            ),
            MatchingConfig(max_active_orders=1),
            id="projected-final-capacity-and-terminal-id-reuse",
        ),
        pytest.param(
            (
                _limit(50, Side.SELL, 100, 2),
                _market(60, Side.BUY, 2),
                _limit(50, Side.BUY, 99, 1),
                CancelOrder(11, 50, INSTRUMENT),
                _limit(50, Side.SELL, 101, 1),
            ),
            MatchingConfig(),
            id="same-id-reuse-after-passive-fill-and-cancel",
        ),
        pytest.param(
            (
                _market(1, Side.BUY, 5),
                _limit(2, Side.BUY, 100, 1),
            ),
            MatchingConfig(max_active_orders=0),
            id="zero-capacity-allows-terminal-command",
        ),
        pytest.param(
            (
                _limit(1, Side.BUY, 100, U64_MAX),
                _limit(2, Side.BUY, 100, 1),
            ),
            MatchingConfig(max_order_quantity=U64_MAX, max_active_orders=2),
            id="u64-level-overflow-is-atomic-capacity-rejection",
        ),
        pytest.param(
            (
                _limit(1, Side.BUY, 100, 5, client_id=10),
                _limit(2, Side.BUY, 100, 7, client_id=20),
                ReplaceOrder(10, 1, 3, INSTRUMENT, 100, 5),
                _market(100, Side.SELL, 8, client_id=30),
            ),
            MatchingConfig(),
            id="same-price-replace-resets-fifo-priority",
        ),
        pytest.param(
            (
                _limit(1, Side.BUY, 99, 4, client_id=10),
                _limit(2, Side.SELL, 101, 2, client_id=20),
                _limit(3, Side.SELL, 102, 3, client_id=30),
                _limit(4, Side.SELL, 103, 4, client_id=40),
                ReplaceOrder(10, 1, 100, INSTRUMENT, 102, 8),
            ),
            MatchingConfig(),
            id="replace-crosses-levels-then-rests",
        ),
        pytest.param(
            (
                _limit(2, Side.SELL, 100, 10, client_id=20),
                _limit(1, Side.BUY, 99, 4, client_id=10),
                ReplaceOrder(10, 1, 3, INSTRUMENT, 100, 6),
            ),
            MatchingConfig(),
            id="replace-full-fill-partially-reduces-passive",
        ),
        pytest.param(
            (
                NewOrder(0, 0, 0, 0, 0, 0, None, 0),
                NewOrder(1, 1, INSTRUMENT, 255, OrderType.LIMIT, TimeInForce.GTC, 100, 5),
                NewOrder(1, 2, INSTRUMENT, Side.BUY, 255, TimeInForce.GTC, 100, 5),
                NewOrder(1, 3, INSTRUMENT, Side.BUY, OrderType.LIMIT, 255, 100, 5),
                NewOrder(
                    1,
                    4,
                    INSTRUMENT,
                    Side.BUY,
                    OrderType.LIMIT,
                    TimeInForce.FOK,
                    None,
                    5,
                ),
                _limit(10, Side.BUY, 100, 5, client_id=10),
                _limit(11, Side.BUY, 100, 5, client_id=11),
                CancelOrder(99, 10, INSTRUMENT),
                CancelOrder(10, 99, INSTRUMENT),
                ReplaceOrder(10, 99, 12, INSTRUMENT, 100, 5),
                ReplaceOrder(10, 10, 11, INSTRUMENT, 100, 5),
                _limit(13, Side.BUY, 101, 11),
                _limit(14, Side.BUY, 102, 1),
            ),
            MatchingConfig(max_order_quantity=10, tick_increment=5, max_active_orders=4),
            id="pure-and-state-rejection-precedence",
        ),
        pytest.param(
            (
                _limit(1, Side.BUY, 100, 10, client_id=10),
                _limit(2, Side.BUY, 100, U64_MAX - 10, client_id=20),
                ReplaceOrder(10, 1, 3, INSTRUMENT, 100, 10),
            ),
            MatchingConfig(max_order_quantity=U64_MAX, max_active_orders=2),
            id="replace-subtracts-old-before-u64-addition",
        ),
    ],
)
def test_named_cross_language_scenarios(
    commands: tuple[Command, ...],
    config: MatchingConfig,
) -> None:
    _assert_parity(commands, config)
