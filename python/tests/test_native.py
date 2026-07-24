from __future__ import annotations

import json
import os
from pathlib import Path

import pytest
from atlaslob.canonical import state_digest
from atlaslob.domain import (
    ATLASLOB_SEMANTICS_VERSION,
    BookSnapshot,
    CancelOrder,
    MatchingConfig,
    NewOrder,
    OrderSnapshot,
    OrderType,
    PriceLevelSnapshot,
    ReplaceOrder,
    Side,
    TimeInForce,
    event_type,
)
from atlaslob.native import (
    NativeInputConfig,
    NativeProtocolError,
    decode_jsonl,
    encode_stream,
    run_native,
)

_EMPTY_TRANSCRIPT = """\
{"schema":"atlas_diff_v1","kind":"config","mode":"exact","semantics_version":6,"instrument_id":"7","max_order_quantity":"1000","tick_increment":"1","max_active_orders":"16","snapshot_interval":"0"}
{"schema":"atlas_diff_v1","kind":"final","commands_processed":"0","state":{"active_order_count":"0","empty":true,"next_sequence":"1","sequence_exhausted":false,"best_bid":null,"best_ask":null,"state_digest":"19a8ffaeb1bee1b8aa87123c3508af1bfa87e3d634a09ba491e1b85fe597b219"},"snapshot":{"semantics_version":6,"instrument_id":"7","last_sequence":"0","sequence_exhausted":false,"active_order_count":"0","bids":[],"asks":[]}}
"""


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
) -> NewOrder:
    return NewOrder(
        client_id=client_id,
        order_id=order_id,
        instrument_id=7,
        side=side,
        order_type=OrderType.LIMIT,
        time_in_force=TimeInForce.GTC,
        limit_price=price,
        quantity=quantity,
    )


def _snapshot_transcript(snapshot: BookSnapshot) -> str:
    def level_values(
        levels: tuple[PriceLevelSnapshot, ...],
    ) -> list[dict[str, object]]:
        return [
            {
                "price": str(level.price),
                "aggregate_quantity": str(level.aggregate_quantity),
                "orders": [
                    {
                        "order_id": str(order.order_id),
                        "client_id": str(order.client_id),
                        "instrument_id": str(order.instrument_id),
                        "side": "buy" if order.side == Side.BUY else "sell",
                        "price": str(order.price),
                        "remaining_quantity": str(order.remaining_quantity),
                        "priority_sequence": str(order.priority_sequence),
                    }
                    for order in level.orders
                ],
            }
            for level in levels
        ]

    def top(levels: tuple[PriceLevelSnapshot, ...]) -> dict[str, str] | None:
        if not levels:
            return None
        return {
            "price": str(levels[0].price),
            "aggregate_quantity": str(levels[0].aggregate_quantity),
        }

    config = {
        "schema": "atlas_diff_v1",
        "kind": "config",
        "mode": "exact",
        "semantics_version": ATLASLOB_SEMANTICS_VERSION,
        "instrument_id": str(snapshot.instrument_id),
        "max_order_quantity": "18446744073709551615",
        "tick_increment": "1",
        "max_active_orders": "16",
        "snapshot_interval": "0",
    }
    snapshot_value = {
        "semantics_version": snapshot.semantics_version,
        "instrument_id": str(snapshot.instrument_id),
        "last_sequence": str(snapshot.last_sequence),
        "sequence_exhausted": snapshot.sequence_exhausted,
        "active_order_count": str(snapshot.active_order_count),
        "bids": level_values(snapshot.bids),
        "asks": level_values(snapshot.asks),
    }
    state = {
        "active_order_count": str(snapshot.active_order_count),
        "empty": snapshot.active_order_count == 0,
        "next_sequence": str(0 if snapshot.sequence_exhausted else snapshot.last_sequence + 1),
        "sequence_exhausted": snapshot.sequence_exhausted,
        "best_bid": top(snapshot.bids),
        "best_ask": top(snapshot.asks),
        "state_digest": state_digest(snapshot),
    }
    final = {
        "schema": "atlas_diff_v1",
        "kind": "final",
        "commands_processed": "0",
        "state": state,
        "snapshot": snapshot_value,
    }
    return "\n".join(json.dumps(record, separators=(",", ":")) for record in (config, final)) + "\n"


def _snapshot_order(
    order_id: int,
    side: Side,
    price: int,
    priority: int,
    *,
    client_id: int = 11,
    instrument_id: int = 7,
    remaining: int = 5,
) -> OrderSnapshot:
    return OrderSnapshot(
        order_id=order_id,
        client_id=client_id,
        instrument_id=instrument_id,
        side=side,
        price=price,
        remaining_quantity=remaining,
        priority_sequence=priority,
    )


def _snapshot(
    *,
    active_count: int,
    last_sequence: int,
    bids: tuple[PriceLevelSnapshot, ...] = (),
    asks: tuple[PriceLevelSnapshot, ...] = (),
) -> BookSnapshot:
    return BookSnapshot(
        semantics_version=ATLASLOB_SEMANTICS_VERSION,
        instrument_id=7,
        last_sequence=last_sequence,
        sequence_exhausted=False,
        active_order_count=active_count,
        bids=bids,
        asks=asks,
    )


def test_command_encoding_is_lossless_and_canonical() -> None:
    config = NativeInputConfig(
        instrument_id=7,
        engine=MatchingConfig(max_order_quantity=(1 << 64) - 1, tick_increment=5),
        snapshot_interval=9,
    )
    commands = (
        NewOrder(1, (1 << 64) - 1, 7, 255, 254, 253, -(1 << 63), (1 << 64) - 1),
        NewOrder(1, 2, 7, Side.BUY, OrderType.MARKET, TimeInForce.IOC, None, 3),
        CancelOrder(1, 2, 7),
        ReplaceOrder(1, 2, 3, 7, (1 << 63) - 1, 4),
    )

    assert encode_stream(config, commands).splitlines() == [
        "ATLAS_DIFF_V1 7 18446744073709551615 5 18446744073709551615 9",
        "N 1 18446744073709551615 7 255 254 253 1 -9223372036854775808 18446744073709551615",
        "N 1 2 7 1 2 2 0 0 3",
        "C 1 2 7",
        "R 1 2 3 7 9223372036854775807 4",
    ]


def test_empty_transcript_decodes_and_cross_checks_the_canonical_state() -> None:
    transcript = decode_jsonl(_EMPTY_TRANSCRIPT)

    assert transcript.config is not None
    assert transcript.config.input.instrument_id == 7
    assert transcript.results == ()
    assert transcript.error is None
    assert transcript.final is not None
    assert transcript.final.snapshot.active_order_count == 0
    assert transcript.final.state.next_sequence == 1


@pytest.mark.parametrize(
    "text",
    [
        "",
        "not-json\n",
        '{"schema":"wrong","kind":"error","line":"1","code":"missing_header"}\n',
        '{"schema":"atlas_diff_v1","kind":"error","line":"01","code":"missing_header"}\n',
        '{"schema":"atlas_diff_v1","kind":"error","line":"1","code":"future_error"}\n',
        _EMPTY_TRANSCRIPT.replace(
            "19a8ffaeb1bee1b8aa87123c3508af1bfa87e3d634a09ba491e1b85fe597b219",
            "09a8ffaeb1bee1b8aa87123c3508af1bfa87e3d634a09ba491e1b85fe597b219",
        ),
    ],
)
def test_decoder_rejects_malformed_or_self_inconsistent_evidence(text: str) -> None:
    with pytest.raises(NativeProtocolError):
        decode_jsonl(text)


@pytest.mark.parametrize(
    "snapshot",
    [
        pytest.param(
            _snapshot(
                active_count=2,
                last_sequence=1,
                bids=(
                    PriceLevelSnapshot(
                        100,
                        5,
                        (_snapshot_order(1, Side.BUY, 100, 1),),
                    ),
                ),
            ),
            id="active-count",
        ),
        pytest.param(
            _snapshot(
                active_count=2,
                last_sequence=2,
                bids=(
                    PriceLevelSnapshot(
                        100,
                        5,
                        (_snapshot_order(1, Side.BUY, 100, 1),),
                    ),
                    PriceLevelSnapshot(
                        99,
                        5,
                        (_snapshot_order(1, Side.BUY, 99, 2),),
                    ),
                ),
            ),
            id="duplicate-order-id",
        ),
        pytest.param(
            _snapshot(
                active_count=1,
                last_sequence=1,
                bids=(
                    PriceLevelSnapshot(
                        100,
                        5,
                        (_snapshot_order(1, Side.BUY, 100, 1, instrument_id=8),),
                    ),
                ),
            ),
            id="wrong-instrument",
        ),
        pytest.param(
            _snapshot(
                active_count=1,
                last_sequence=1,
                bids=(
                    PriceLevelSnapshot(
                        100,
                        0,
                        (_snapshot_order(1, Side.BUY, 100, 1, remaining=0),),
                    ),
                ),
            ),
            id="zero-remaining",
        ),
        pytest.param(
            _snapshot(
                active_count=2,
                last_sequence=2,
                bids=(
                    PriceLevelSnapshot(
                        100,
                        10,
                        (
                            _snapshot_order(1, Side.BUY, 100, 2),
                            _snapshot_order(2, Side.BUY, 100, 1),
                        ),
                    ),
                ),
            ),
            id="nonmonotonic-fifo",
        ),
        pytest.param(
            _snapshot(
                active_count=0,
                last_sequence=0,
                bids=(PriceLevelSnapshot(100, 0, ()),),
            ),
            id="empty-level",
        ),
        pytest.param(
            _snapshot(
                active_count=2,
                last_sequence=2,
                bids=(
                    PriceLevelSnapshot(
                        100,
                        5,
                        (_snapshot_order(1, Side.BUY, 100, 1),),
                    ),
                ),
                asks=(
                    PriceLevelSnapshot(
                        100,
                        5,
                        (_snapshot_order(2, Side.SELL, 100, 2),),
                    ),
                ),
            ),
            id="crossed-book",
        ),
    ],
)
def test_decoder_rejects_structurally_impossible_snapshots(snapshot: BookSnapshot) -> None:
    with pytest.raises(NativeProtocolError):
        decode_jsonl(_snapshot_transcript(snapshot))


def test_real_exact_driver_output_is_valid_json_and_covers_every_event_variant() -> None:
    commands = (
        _limit(1, Side.BUY, 100, 5),
        ReplaceOrder(11, 1, 2, 7, 101, 5),
        NewOrder(22, 3, 7, Side.SELL, OrderType.MARKET, TimeInForce.IOC, None, 2),
        CancelOrder(11, 2, 7),
        NewOrder(11, 4, 7, 0, OrderType.LIMIT, TimeInForce.GTC, 99, 1),
    )

    run = run_native(
        _executable(),
        NativeInputConfig(7, MatchingConfig(max_order_quantity=1000, max_active_orders=16), 2),
        commands,
    )

    assert run.returncode == 0
    assert run.stderr == ""
    assert run.transcript.error is None
    assert run.transcript.final is not None
    assert run.transcript.final.commands_processed == len(commands)
    assert run.transcript.final.state.next_sequence == 6
    types = {
        event_type(event) for result in run.transcript.results for event in (result.events or ())
    }
    assert {value.value for value in types} == set(range(1, 9))


def test_real_compact_driver_retains_digests_and_omits_events() -> None:
    run = run_native(
        _executable(),
        NativeInputConfig(7, snapshot_interval=1),
        (_limit(1, Side.BUY, 100, 5), CancelOrder(11, 1, 7)),
        mode="compact",
    )

    assert run.returncode == 0
    assert run.transcript.config is not None
    assert run.transcript.config.mode == "compact"
    assert all(result.events is None for result in run.transcript.results)
    assert all(result.event_digest is not None for result in run.transcript.results)
    assert all(result.snapshot is not None for result in run.transcript.results)
