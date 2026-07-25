from __future__ import annotations

import json
import os
from collections.abc import Callable, Iterator
from pathlib import Path

import pytest
from atlaslob.domain import (
    I64_MAX,
    I64_MIN,
    U32_MAX,
    U64_MAX,
    CancelOrder,
    Command,
    MatchingConfig,
    NewOrder,
    OrderType,
    ReplaceOrder,
    Side,
    TimeInForce,
)
from atlaslob.native import (
    NativeConfigRecord,
    NativeFinalRecord,
    NativeInputConfig,
    NativeProtocolError,
    NativeResultRecord,
    NativeStreamDecoder,
    decode_command,
    decode_header,
    decode_jsonl,
    encode_command,
    encode_header,
    encode_stream,
    run_native,
)

INSTRUMENT = 7


def _executable() -> Path:
    configured = os.environ.get("ATLAS_DIFF_NATIVE")
    if configured is not None:
        candidate = Path(configured)
        if not candidate.is_file():
            raise FileNotFoundError(
                f"ATLAS_DIFF_NATIVE does not name a native evidence executable: {configured}"
            )
        return candidate.resolve()

    for candidate in (
        Path("build/dev-gcc/atlas_diff_native.exe"),
        Path("build/dev-gcc/atlas_diff_native"),
    ):
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError("atlas_diff_native has not been built")


def _commands() -> tuple[Command, ...]:
    return (
        NewOrder(
            client_id=11,
            order_id=1,
            instrument_id=INSTRUMENT,
            side=Side.BUY,
            order_type=OrderType.LIMIT,
            time_in_force=TimeInForce.GTC,
            limit_price=100,
            quantity=5,
        ),
        NewOrder(
            client_id=22,
            order_id=2,
            instrument_id=INSTRUMENT,
            side=Side.SELL,
            order_type=OrderType.LIMIT,
            time_in_force=TimeInForce.IOC,
            limit_price=100,
            quantity=2,
        ),
        CancelOrder(client_id=11, order_id=999, instrument_id=INSTRUMENT),
    )


def test_persisted_input_header_and_commands_round_trip_losslessly() -> None:
    config = NativeInputConfig(
        instrument_id=U32_MAX,
        engine=MatchingConfig(
            max_order_quantity=U64_MAX,
            tick_increment=I64_MAX,
            max_active_orders=U64_MAX,
        ),
        snapshot_interval=U64_MAX,
    )
    commands: tuple[Command, ...] = (
        NewOrder(
            client_id=0,
            order_id=U64_MAX,
            instrument_id=U32_MAX,
            side=255,
            order_type=0,
            time_in_force=254,
            limit_price=I64_MIN,
            quantity=0,
        ),
        NewOrder(
            client_id=U32_MAX,
            order_id=0,
            instrument_id=0,
            side=0,
            order_type=255,
            time_in_force=0,
            limit_price=None,
            quantity=U64_MAX,
        ),
        CancelOrder(client_id=0, order_id=U64_MAX, instrument_id=0),
        ReplaceOrder(
            client_id=U32_MAX,
            old_order_id=0,
            new_order_id=U64_MAX,
            instrument_id=U32_MAX,
            new_limit_price=I64_MAX,
            new_quantity=0,
        ),
    )

    assert decode_header(encode_header(config) + "\r\n") == config
    assert tuple(decode_command(encode_command(command)) for command in commands) == commands
    assert encode_stream(config, commands) == (
        "\n".join((encode_header(config), *(encode_command(command) for command in commands)))
        + "\n"
    )


@pytest.mark.parametrize(
    "line",
    [
        "",
        "ATLAS_DIFF_V1 7 1 1 1",
        "ATLAS_DIFF_V2 7 1 1 1 0",
        "ATLAS_DIFF_V1 07 1 1 1 0",
        "ATLAS_DIFF_V1 0 1 1 1 0",
        "ATLAS_DIFF_V1 7 0 1 1 0",
        "ATLAS_DIFF_V1 7 1 -1 1 0",
        f"ATLAS_DIFF_V1 7 1 1 1 {U64_MAX + 1}",
        "ATLAS_DIFF_V1\u00a07 1 1 1 0",
        "ATLAS_DIFF_V1 7 1 1\n1 0",
    ],
)
def test_persisted_header_decoder_rejects_noncanonical_or_invalid_input(line: str) -> None:
    with pytest.raises(ValueError):
        decode_header(line)


@pytest.mark.parametrize(
    "line",
    [
        "",
        "X 1 2 3",
        "C 1 2",
        "C 01 2 3",
        f"C 1 {U64_MAX + 1} 3",
        "N 1 2 3 1 1 1 2 100 5",
        "N 1 2 3 256 1 1 1 100 5",
        "N 1 2 3 1 1 1 0 100 5",
        f"N 1 2 3 1 1 1 1 {I64_MAX + 1} 5",
        "R 1 2 3 7 100",
        "R 1 2 3 7 -0 5",
        "C\u00a01 2 3",
        "C 1 2\n3",
    ],
)
def test_persisted_command_decoder_rejects_noncanonical_or_invalid_input(line: str) -> None:
    with pytest.raises(ValueError):
        decode_command(line)


def test_incremental_decoder_matches_complete_decoder_without_retaining_results() -> None:
    commands = _commands()
    config = NativeInputConfig(
        INSTRUMENT,
        MatchingConfig(max_order_quantity=100, tick_increment=1, max_active_orders=10),
        snapshot_interval=1,
    )
    run = run_native(_executable(), config, commands)
    complete = decode_jsonl(
        run.stdout,
        expected_mode="exact",
        expected_input=config,
        expected_commands=commands,
        returncode=run.returncode,
    )
    decoder = NativeStreamDecoder(
        expected_mode="exact",
        expected_input=config,
        expected_commands=commands,
    )
    records = tuple(decoder.feed_line(line) for line in run.stdout.splitlines(keepends=True))
    summary = decoder.finish(run.returncode)

    assert tuple(record for record in records if isinstance(record, NativeResultRecord)) == (
        complete.results
    )
    assert next(record for record in records if isinstance(record, NativeConfigRecord)) == (
        complete.config
    )
    assert next(record for record in records if isinstance(record, NativeFinalRecord)) == (
        complete.final
    )
    assert summary.config == complete.config
    assert summary.result_count == len(commands)
    assert summary.last_result == complete.results[-1]
    assert summary.final == complete.final
    assert summary.error is None
    assert not hasattr(summary, "results")

    with pytest.raises(NativeProtocolError, match="already finished"):
        decoder.finish(run.returncode)
    with pytest.raises(NativeProtocolError, match="already finished"):
        decoder.feed_line(run.stdout.splitlines()[0])


def test_incremental_decoder_consumes_expected_commands_lazily() -> None:
    commands = _commands()
    config = NativeInputConfig(INSTRUMENT, snapshot_interval=1)
    run = run_native(_executable(), config, commands)
    consumed: list[int] = []

    def expected_commands() -> Iterator[Command]:
        for index, command in enumerate(commands):
            consumed.append(index)
            yield command

    decoder = NativeStreamDecoder(
        expected_mode="exact",
        expected_input=config,
        expected_commands=expected_commands(),
    )
    lines = run.stdout.splitlines()

    decoder.feed_line(lines[0])
    assert consumed == []
    decoder.feed_line(lines[1])
    assert consumed == [0]
    for line in lines[2:]:
        decoder.feed_line(line)
    summary = decoder.finish(run.returncode)

    assert consumed == [0, 1, 2]
    assert summary.result_count == len(commands)


def test_incremental_decoder_rejects_result_bound_to_the_wrong_command_immediately() -> None:
    commands = _commands()
    config = NativeInputConfig(INSTRUMENT, snapshot_interval=1)
    run = run_native(_executable(), config, commands)
    decoder = NativeStreamDecoder(
        expected_mode="exact",
        expected_input=config,
        expected_commands=(
            CancelOrder(client_id=11, order_id=1, instrument_id=INSTRUMENT),
            *commands[1:],
        ),
    )
    lines = run.stdout.splitlines()

    decoder.feed_line(lines[0])
    with pytest.raises(NativeProtocolError, match="command type"):
        decoder.feed_line(lines[1])


def test_incremental_decoder_rejects_side_summary_mutation_on_rejected_command() -> None:
    commands = _commands()
    config = NativeInputConfig(INSTRUMENT, snapshot_interval=0)
    run = run_native(_executable(), config, commands)
    lines = run.stdout.splitlines()
    rejected = json.loads(lines[3])
    rejected["state"]["bid_aggregate_quantity"] = "4"
    decoder = NativeStreamDecoder(
        expected_mode="exact",
        expected_input=config,
        expected_commands=commands,
    )

    for line in lines[:3]:
        decoder.feed_line(line)
    with pytest.raises(NativeProtocolError, match="rejected command changed visible"):
        decoder.feed_line(json.dumps(rejected, separators=(",", ":")))


def test_incremental_decoder_rejects_timeline_and_post_terminal_records() -> None:
    commands = _commands()
    config = NativeInputConfig(INSTRUMENT, snapshot_interval=1)
    run = run_native(_executable(), config, commands)
    lines = run.stdout.splitlines()
    result = json.loads(lines[1])
    result["command_index"] = "1"
    decoder = NativeStreamDecoder()

    decoder.feed_line(lines[0])
    with pytest.raises(NativeProtocolError, match="indices"):
        decoder.feed_line(json.dumps(result))

    complete_decoder = NativeStreamDecoder()
    for line in lines:
        complete_decoder.feed_line(line)
    with pytest.raises(NativeProtocolError, match="after a terminal"):
        complete_decoder.feed_line(lines[-1])


def test_incremental_decoder_finish_requires_terminal_and_valid_process_state() -> None:
    commands = _commands()
    config = NativeInputConfig(INSTRUMENT, snapshot_interval=1)
    run = run_native(_executable(), config, commands)
    lines = run.stdout.splitlines()
    incomplete = NativeStreamDecoder()
    incomplete.feed_line(lines[0])

    with pytest.raises(NativeProtocolError, match="no terminal"):
        incomplete.finish()

    wrong_count = NativeStreamDecoder(expected_commands=(*commands, commands[0]))
    for line in lines:
        wrong_count.feed_line(line)
    with pytest.raises(NativeProtocolError, match="every requested command"):
        wrong_count.finish(0)


def test_incremental_decoder_accepts_adapter_error_without_config() -> None:
    line = json.dumps(
        {
            "schema": "atlas_diff_v1",
            "kind": "error",
            "line": "1",
            "code": "missing_header",
        },
        separators=(",", ":"),
    )
    decoder = NativeStreamDecoder()

    error = decoder.feed_line(line)
    summary = decoder.finish(2)

    assert error == summary.error
    assert summary.config is None
    assert summary.result_count == 0
    assert summary.last_result is None
    assert summary.final is None


@pytest.mark.parametrize(
    ("mutate", "message"),
    [
        pytest.param(
            lambda lines: lines[0] + "\n" + lines[1],
            "multiple logical lines",
            id="multiple-records",
        ),
        pytest.param(lambda _lines: "", "blank", id="blank"),
    ],
)
def test_incremental_decoder_requires_exactly_one_json_record_per_feed(
    mutate: Callable[[list[str]], str],
    message: str,
) -> None:
    run = run_native(
        _executable(),
        NativeInputConfig(INSTRUMENT),
        (),
    )
    decoder = NativeStreamDecoder()

    with pytest.raises(NativeProtocolError, match=message):
        decoder.feed_line(mutate(run.stdout.splitlines()))
