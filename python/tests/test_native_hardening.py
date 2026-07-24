from __future__ import annotations

import copy
import json
from collections.abc import Callable

import pytest
from atlaslob.canonical import event_digest, state_digest
from atlaslob.domain import (
    AcceptedEvent,
    BookChangedEvent,
    BookSnapshot,
    CommandType,
    DoneEvent,
    DoneReason,
    EventBatch,
    EventHeader,
    MatchingConfig,
    NewOrder,
    OrderSnapshot,
    OrderType,
    PriceLevelSnapshot,
    RestedEvent,
    Side,
    TimeInForce,
    TopOfBookLevel,
)
from atlaslob.native import NativeInputConfig, NativeProtocolError, decode_jsonl, run_native

_COMMAND = NewOrder(
    client_id=1,
    order_id=1,
    instrument_id=7,
    side=Side.BUY,
    order_type=OrderType.LIMIT,
    time_in_force=TimeInForce.GTC,
    limit_price=100,
    quantity=5,
)
_INPUT = NativeInputConfig(
    instrument_id=7,
    engine=MatchingConfig(max_order_quantity=10, tick_increment=1, max_active_orders=2),
    snapshot_interval=1,
)


def _snapshot_value(snapshot: BookSnapshot) -> dict[str, object]:
    def levels(values: tuple[PriceLevelSnapshot, ...]) -> list[dict[str, object]]:
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
            for level in values
        ]

    return {
        "semantics_version": snapshot.semantics_version,
        "instrument_id": str(snapshot.instrument_id),
        "last_sequence": str(snapshot.last_sequence),
        "sequence_exhausted": snapshot.sequence_exhausted,
        "active_order_count": str(snapshot.active_order_count),
        "bids": levels(snapshot.bids),
        "asks": levels(snapshot.asks),
    }


def _top_value(levels: tuple[PriceLevelSnapshot, ...]) -> dict[str, str] | None:
    if not levels:
        return None
    return {
        "price": str(levels[0].price),
        "aggregate_quantity": str(levels[0].aggregate_quantity),
    }


def _state_value(snapshot: BookSnapshot) -> dict[str, object]:
    return {
        "active_order_count": str(snapshot.active_order_count),
        "empty": snapshot.active_order_count == 0,
        "next_sequence": str(0 if snapshot.sequence_exhausted else snapshot.last_sequence + 1),
        "sequence_exhausted": snapshot.sequence_exhausted,
        "best_bid": _top_value(snapshot.bids),
        "best_ask": _top_value(snapshot.asks),
        "state_digest": state_digest(snapshot),
    }


def _records() -> list[dict[str, object]]:
    headers = tuple(EventHeader(1, index, 7) for index in range(3))
    events = (
        AcceptedEvent(headers[0], CommandType.NEW),
        RestedEvent(headers[1], 1, 1, Side.BUY, 100, 5),
        BookChangedEvent(headers[2], TopOfBookLevel(100, 5), None),
    )
    batch = EventBatch(events)
    snapshot = BookSnapshot(
        semantics_version=6,
        instrument_id=7,
        last_sequence=1,
        sequence_exhausted=False,
        active_order_count=1,
        bids=(
            PriceLevelSnapshot(
                100,
                5,
                (OrderSnapshot(1, 1, 7, Side.BUY, 100, 5, 1),),
            ),
        ),
        asks=(),
    )
    state = _state_value(snapshot)
    snapshot_value = _snapshot_value(snapshot)
    return [
        {
            "schema": "atlas_diff_v1",
            "kind": "config",
            "mode": "exact",
            "semantics_version": 6,
            "instrument_id": "7",
            "max_order_quantity": "10",
            "tick_increment": "1",
            "max_active_orders": "2",
            "snapshot_interval": "1",
        },
        {
            "schema": "atlas_diff_v1",
            "kind": "result",
            "command_index": "0",
            "line": "2",
            "command_type": "new",
            "outcome": "committed",
            "command_sequence": "1",
            "engine_error": None,
            "event_digest": event_digest(batch),
            "events": [
                {
                    "type": "accepted",
                    "command_sequence": "1",
                    "event_index": 0,
                    "instrument_id": "7",
                    "command_type": "new",
                },
                {
                    "type": "rested",
                    "command_sequence": "1",
                    "event_index": 1,
                    "instrument_id": "7",
                    "order_id": "1",
                    "client_id": "1",
                    "side": "buy",
                    "price": "100",
                    "remaining_quantity": "5",
                },
                {
                    "type": "book_changed",
                    "command_sequence": "1",
                    "event_index": 2,
                    "instrument_id": "7",
                    "best_bid": {"price": "100", "aggregate_quantity": "5"},
                    "best_ask": None,
                },
            ],
            "state": state,
            "snapshot": snapshot_value,
        },
        {
            "schema": "atlas_diff_v1",
            "kind": "final",
            "commands_processed": "1",
            "state": copy.deepcopy(state),
            "snapshot": copy.deepcopy(snapshot_value),
        },
    ]


def _text(records: list[dict[str, object]]) -> str:
    return "\n".join(json.dumps(record, separators=(",", ":")) for record in records) + "\n"


def _splice_empty_final(records: list[dict[str, object]]) -> None:
    empty = BookSnapshot(6, 7, 0, False, 0, (), ())
    records[2]["state"] = _state_value(empty)
    records[2]["snapshot"] = _snapshot_value(empty)


def test_decoder_binds_a_valid_transcript_to_the_request() -> None:
    transcript = decode_jsonl(
        _text(_records()),
        expected_mode="exact",
        expected_input=_INPUT,
        expected_commands=(_COMMAND,),
        returncode=0,
    )

    assert transcript.final is not None
    assert transcript.final.commands_processed == 1


def test_return_code_validation_without_an_expected_command_list_accepts_results() -> None:
    transcript = decode_jsonl(_text(_records()), returncode=0)

    assert len(transcript.results) == 1


@pytest.mark.parametrize(
    "mutate",
    [
        pytest.param(
            lambda records: records[1].__setitem__("command_type", "cancel"),
            id="result-event-command-type",
        ),
        pytest.param(
            lambda records: records[1].__setitem__("snapshot", None),
            id="missing-checkpoint",
        ),
        pytest.param(
            lambda records: cast_dict(records[1]["state"]).__setitem__("empty", True),
            id="observer-count",
        ),
        pytest.param(
            _splice_empty_final,
            id="spliced-final",
        ),
    ],
)
def test_decoder_rejects_cross_record_contradictions(
    mutate: Callable[[list[dict[str, object]]], None],
) -> None:
    records = _records()
    mutate(records)

    with pytest.raises(NativeProtocolError):
        decode_jsonl(_text(records))


def test_committed_envelope_must_begin_with_acceptance() -> None:
    records = _records()
    done = DoneEvent(EventHeader(1, 0, 7), 1, DoneReason.FILLED, 0)
    records[1]["events"] = [
        {
            "type": "done",
            "command_sequence": "1",
            "event_index": 0,
            "instrument_id": "7",
            "order_id": "1",
            "reason": "filled",
            "remaining_quantity": "0",
        }
    ]
    records[1]["event_digest"] = event_digest(EventBatch((done,)))

    with pytest.raises(NativeProtocolError, match="accepted"):
        decode_jsonl(_text(records))


@pytest.mark.parametrize(
    "snapshot",
    [
        BookSnapshot(
            6,
            7,
            1,
            False,
            1,
            (PriceLevelSnapshot(-1, 1, (OrderSnapshot(1, 1, 7, Side.BUY, -1, 1, 1),)),),
            (),
        ),
        BookSnapshot(6, 7, 1, True, 0, (), ()),
    ],
    ids=("negative-price", "impossible-exhaustion"),
)
def test_decoder_rejects_impossible_snapshot_semantics(snapshot: BookSnapshot) -> None:
    records = _records()
    records[2]["state"] = _state_value(snapshot)
    records[2]["snapshot"] = _snapshot_value(snapshot)
    records[2]["commands_processed"] = "0"
    del records[1]

    with pytest.raises(NativeProtocolError):
        decode_jsonl(_text(records))


def test_malformed_config_is_normalized_to_protocol_error() -> None:
    records = _records()
    records[0]["max_order_quantity"] = "0"

    with pytest.raises(NativeProtocolError):
        decode_jsonl(_text(records))


def test_oversized_raw_json_integer_is_normalized_to_protocol_error() -> None:
    text = _text(_records()).replace(
        '"semantics_version":6',
        '"semantics_version":' + "9" * 5000,
        1,
    )

    with pytest.raises(NativeProtocolError):
        decode_jsonl(text)


def test_request_and_process_terminal_mismatches_are_rejected() -> None:
    with pytest.raises(NativeProtocolError, match="mode"):
        decode_jsonl(_text(_records()), expected_mode="compact")

    wrong_route = NativeInputConfig(8, _INPUT.engine, _INPUT.snapshot_interval)
    with pytest.raises(NativeProtocolError, match="requested config"):
        decode_jsonl(_text(_records()), expected_input=wrong_route)

    with pytest.raises(NativeProtocolError, match="return"):
        decode_jsonl(
            _text(_records()),
            expected_commands=(_COMMAND,),
            returncode=9,
        )

    with pytest.raises(NativeProtocolError, match="every requested"):
        decode_jsonl(
            _text(_records()),
            expected_commands=(_COMMAND, _COMMAND),
            returncode=0,
        )


@pytest.mark.parametrize(
    ("line", "code", "returncode"),
    [
        pytest.param("999", "engine_exception", 3, id="unbound-source-line"),
        pytest.param("2", "unknown_command", 2, id="impossible-canonical-syntax"),
    ],
)
def test_request_bound_terminal_errors_reject_impossible_evidence(
    line: str,
    code: str,
    returncode: int,
) -> None:
    records: list[dict[str, object]] = [
        _records()[0],
        {
            "schema": "atlas_diff_v1",
            "kind": "error",
            "line": line,
            "code": code,
        },
    ]

    with pytest.raises(NativeProtocolError):
        decode_jsonl(
            _text(records),
            expected_mode="exact",
            expected_input=_INPUT,
            expected_commands=(_COMMAND,),
            returncode=returncode,
        )


def test_request_bound_input_read_failure_at_the_next_line_is_representable() -> None:
    records: list[dict[str, object]] = [
        _records()[0],
        {
            "schema": "atlas_diff_v1",
            "kind": "error",
            "line": "2",
            "code": "input_read_failure",
        },
    ]

    transcript = decode_jsonl(
        _text(records),
        expected_mode="exact",
        expected_input=_INPUT,
        expected_commands=(_COMMAND,),
        returncode=2,
    )

    assert transcript.error is not None
    assert transcript.error.code == "input_read_failure"


def test_adapter_error_cannot_follow_a_terminal_engine_error_result() -> None:
    records = _records()
    empty = BookSnapshot(6, 7, 0, False, 0, (), ())
    result = records[1]
    result["outcome"] = "engine_error"
    result["command_sequence"] = None
    result["engine_error"] = "internal_failure"
    result["event_digest"] = None
    result["events"] = None
    result["state"] = _state_value(empty)
    result["snapshot"] = _snapshot_value(empty)
    records[2] = {
        "schema": "atlas_diff_v1",
        "kind": "error",
        "line": "3",
        "code": "resource_failure",
    }

    with pytest.raises(NativeProtocolError, match="engine-error"):
        decode_jsonl(_text(records))


def test_run_native_normalizes_invalid_output_text(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def invalid_text(*_args: object, **_kwargs: object) -> None:
        raise UnicodeDecodeError("utf-8", b"\xff", 0, 1, "invalid start byte")

    monkeypatch.setattr("atlaslob.native.subprocess.run", invalid_text)

    with pytest.raises(NativeProtocolError, match="valid text"):
        run_native("unused-native-adapter", _INPUT, (_COMMAND,))


def cast_dict(value: object) -> dict[str, object]:
    assert isinstance(value, dict)
    return value
