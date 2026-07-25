from __future__ import annotations

import os
from collections.abc import Callable
from dataclasses import dataclass, replace
from pathlib import Path

import pytest
from atlaslob.canonical import engine_state_digest, event_digest
from atlaslob.domain import (
    I64_MAX,
    I64_MIN,
    CancelOrder,
    Command,
    EventBatch,
    InstrumentConfig,
    MatchingConfig,
    MultiInstrumentEngineConfig,
    NewOrder,
    OrderType,
    RejectReason,
    ReplaceOrder,
    Side,
    TimeInForce,
    TradeEvent,
)
from atlaslob.generation import WorkloadProfile, resolve_workload_spec
from atlaslob.multi_differential import capture_reference_router
from atlaslob.multi_generation import MultiWorkloadSpec, iter_multi_commands
from atlaslob.multi_native import (
    MultiNativeInput,
    MultiNativeProtocolError,
    MultiOutputMode,
    assert_multi_native_parity,
    decode_multi_jsonl,
    encode_multi_stream,
    encode_multi_workload,
    multi_native_input_from_spec,
    run_multi_native,
    run_multi_workload,
)
from atlaslob.native import NativeInputConfig, encode_stream


def _executable() -> Path:
    configured = os.environ.get("ATLAS_DIFF_MULTI_NATIVE")
    if configured:
        path = Path(configured)
        if not path.is_file():
            raise FileNotFoundError(
                f"ATLAS_DIFF_MULTI_NATIVE does not name a native evidence executable: {configured}"
            )
        return path.resolve()
    candidates = (
        Path("build/dev-gcc/atlas_diff_multi_native.exe"),
        Path("build/dev-gcc/atlas_diff_multi_native"),
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError("build atlas_diff_multi_native or set ATLAS_DIFF_MULTI_NATIVE")


def _catalog(
    *,
    first_capacity: int = 8,
    second_capacity: int = 8,
) -> tuple[InstrumentConfig, ...]:
    return (
        InstrumentConfig(
            7,
            MatchingConfig(1_000, 1, first_capacity),
        ),
        InstrumentConfig(
            9,
            MatchingConfig(1_000, 1, second_capacity),
        ),
    )


@dataclass(frozen=True, slots=True)
class _FixtureCase:
    name: str
    commands: tuple[Command, ...]
    max_total_active_orders: int = 8
    expected_rejections: tuple[RejectReason, ...] = ()


_FIXTURE_CASES = (
    _FixtureCase(
        "global_duplicate",
        (
            NewOrder(11, 1, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 5),
            NewOrder(11, 1, 9, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 90, 5),
        ),
        expected_rejections=(RejectReason.DUPLICATE_ORDER_ID,),
    ),
    _FixtureCase(
        "ownership_before_instrument",
        (
            NewOrder(11, 1, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 5),
            CancelOrder(12, 1, 9),
            CancelOrder(11, 1, 9),
            ReplaceOrder(12, 1, 2, 9, 90, 5),
            ReplaceOrder(11, 1, 2, 9, 90, 5),
        ),
        expected_rejections=(
            RejectReason.OWNERSHIP_MISMATCH,
            RejectReason.INSTRUMENT_MISMATCH,
            RejectReason.OWNERSHIP_MISMATCH,
            RejectReason.INSTRUMENT_MISMATCH,
        ),
    ),
    _FixtureCase(
        "terminal_cross_instrument_reuse",
        (
            NewOrder(11, 1, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 5),
            CancelOrder(11, 1, 7),
            NewOrder(22, 1, 9, Side.SELL, OrderType.LIMIT, TimeInForce.GTC, 110, 7),
        ),
    ),
    _FixtureCase(
        "binding_global_capacity",
        (
            NewOrder(11, 1, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 5),
            NewOrder(22, 2, 9, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 90, 5),
        ),
        max_total_active_orders=1,
        expected_rejections=(RejectReason.CAPACITY_EXCEEDED,),
    ),
    _FixtureCase(
        "raw_invalid_enums_and_signed_bounds",
        (
            NewOrder(11, 1, 7, 255, OrderType.LIMIT, TimeInForce.GTC, 100, 5),
            NewOrder(11, 2, 7, Side.BUY, 254, TimeInForce.GTC, 100, 5),
            NewOrder(11, 3, 7, Side.BUY, OrderType.LIMIT, 253, 100, 5),
            NewOrder(11, 4, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, I64_MIN, 5),
            NewOrder(11, 0, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, I64_MAX, 5),
        ),
        expected_rejections=(
            RejectReason.INVALID_SIDE,
            RejectReason.INVALID_ORDER_TYPE,
            RejectReason.INVALID_TIME_IN_FORCE,
            RejectReason.INVALID_PRICE,
            RejectReason.INVALID_ORDER_ID,
        ),
    ),
)


def _run_case(case: _FixtureCase, mode: MultiOutputMode) -> None:
    config = MultiNativeInput(
        _catalog(),
        MultiInstrumentEngineConfig(case.max_total_active_orders),
        checkpoint_interval=1,
    )
    run = run_multi_native(
        _executable(),
        config,
        case.commands,
        mode=mode,
    )
    capture = capture_reference_router(
        config.catalog,
        case.commands,
        engine_config=config.engine,
    )

    assert run.returncode == 0
    assert_multi_native_parity(run, capture)
    reasons = tuple(
        record.reject_reason
        for record in run.transcript.results
        if record.reject_reason is not None
    )
    assert reasons == case.expected_rejections


@pytest.mark.parametrize("case", _FIXTURE_CASES, ids=lambda case: case.name)
@pytest.mark.parametrize("mode", ("exact", "compact"))
def test_named_v2_fixtures_match_reference_exactly(
    case: _FixtureCase,
    mode: MultiOutputMode,
) -> None:
    _run_case(case, mode)


def _campaign_spec() -> MultiWorkloadSpec:
    return MultiWorkloadSpec(
        streams=(
            resolve_workload_spec(
                WorkloadProfile.INVALID_MIX,
                command_count=40,
                instrument_id=7,
            ),
            resolve_workload_spec(
                WorkloadProfile.SWEEP_HEAVY,
                command_count=40,
                instrument_id=9,
            ),
        ),
        engine=MultiInstrumentEngineConfig(4),
    )


@pytest.mark.parametrize("mode", ("exact", "compact"))
def test_fixed_seed_v2_campaign_matches_reference(
    mode: MultiOutputMode,
) -> None:
    spec = _campaign_spec()
    commands = tuple(iter_multi_commands(spec, 0xA71A5))
    run = run_multi_workload(
        _executable(),
        spec,
        commands,
        mode=mode,
        checkpoint_interval=7,
    )
    capture = capture_reference_router(
        spec.catalog,
        commands,
        engine_config=spec.engine,
    )

    assert_multi_native_parity(run, capture)
    assert run.transcript.final is not None
    assert run.transcript.final.commands_processed == spec.command_count


def test_v2_text_encoder_is_exact_and_maps_multi_workload_specs() -> None:
    spec = MultiWorkloadSpec(
        streams=(
            resolve_workload_spec(
                WorkloadProfile.UNIFORM_SYNTHETIC,
                command_count=1,
                instrument_id=7,
            ),
            resolve_workload_spec(
                WorkloadProfile.UNIFORM_SYNTHETIC,
                command_count=1,
                instrument_id=9,
            ),
        ),
        engine=MultiInstrumentEngineConfig(8),
    )
    commands: tuple[Command, ...] = (
        NewOrder(11, 1, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 5),
        CancelOrder(11, 1, 7),
    )
    config = multi_native_input_from_spec(spec, checkpoint_interval=5)

    assert encode_multi_stream(config, commands) == (
        "ATLAS_DIFF_V2 8 2 2 5\nI 7 1000 1 128\nI 9 1000 1 128\nN 11 1 7 1 1 1 1 100 5\nC 11 1 7\n"
    )
    assert encode_multi_workload(spec, commands, checkpoint_interval=5) == encode_multi_stream(
        config,
        commands,
    )


def _valid_transcript() -> tuple[str, MultiNativeInput, tuple[Command, ...]]:
    config = MultiNativeInput(
        _catalog(),
        MultiInstrumentEngineConfig(8),
        checkpoint_interval=1,
    )
    commands: tuple[Command, ...] = (
        NewOrder(11, 1, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 5),
    )
    run = run_multi_native(_executable(), config, commands)
    return run.stdout, config, commands


def _replace_first_event_digest(value: str) -> str:
    marker = '"event_digest":"'
    start = value.index(marker) + len(marker)
    replacement = "0" if value[start] != "0" else "1"
    return value[:start] + replacement + value[start + 1 :]


@pytest.mark.parametrize(
    "mutation",
    (
        lambda value: value.rstrip("\n"),
        lambda value: value.replace('"command_index":"0"', '"command_index":0', 1),
        lambda value: "\n".join(value.splitlines()[1:2] + value.splitlines()[0:1]) + "\n",
        lambda value: value.replace('"commands_processed":"1"', '"commands_processed":"2"', 1),
        _replace_first_event_digest,
        lambda value: value.replace('"active_order_count":"1"', '"active_order_count":"2"', 1),
        lambda value: value.replace(
            '"kind":"config"',
            '"kind":"config","kind":"config"',
            1,
        ),
    ),
    ids=(
        "missing-final-lf",
        "wrong-json-type",
        "record-order",
        "count-mismatch",
        "event-digest",
        "snapshot-count",
        "duplicate-key",
    ),
)
def test_v2_decoder_rejects_type_order_count_digest_and_snapshot_corruption(
    mutation: Callable[[str], str],
) -> None:
    stdout, config, commands = _valid_transcript()

    with pytest.raises(MultiNativeProtocolError):
        decode_multi_jsonl(
            mutation(stdout),
            expected_mode="exact",
            expected_input=config,
            expected_commands=commands,
            returncode=0,
        )


def test_v2_decoder_rejects_impossible_error_terminals() -> None:
    stdout, config, commands = _valid_transcript()
    config_line, result_line, _ = stdout.splitlines()
    impossible_exit_class = (
        '{"schema":"atlas_diff_v2","kind":"error","line":"1","code":"engine_exception"}\n'
    )
    impossible_position = (
        config_line + "\n" + '{"schema":"atlas_diff_v2","kind":"error","line":"999",'
        '"code":"engine_exception"}\n'
    )
    error_after_exhaustion = (
        config_line
        + "\n"
        + result_line
        + "\n"
        + '{"schema":"atlas_diff_v2","kind":"error","line":"5",'
        '"code":"engine_exception"}\n'
    )

    for text, returncode in (
        (impossible_exit_class, 2),
        (impossible_position, 3),
        (error_after_exhaustion, 3),
    ):
        with pytest.raises(MultiNativeProtocolError):
            decode_multi_jsonl(
                text,
                expected_mode="exact",
                expected_input=config,
                expected_commands=commands,
                returncode=returncode,
            )

    with pytest.raises(MultiNativeProtocolError):
        decode_multi_jsonl(
            impossible_exit_class,
            expected_mode="exact",
            expected_input=config,
            returncode=3,
        )


def test_v2_decoder_binds_checkpoint_and_final_snapshots_to_sequence_progress() -> None:
    stdout, config, commands = _valid_transcript()
    invalid_checkpoint = stdout.replace(
        '"last_sequence":"1"',
        '"last_sequence":"2"',
        1,
    )

    with pytest.raises(MultiNativeProtocolError, match="checkpoint sequence"):
        decode_multi_jsonl(
            invalid_checkpoint,
            expected_mode="exact",
            expected_input=config,
            expected_commands=commands,
            returncode=0,
        )

    no_checkpoint = MultiNativeInput(
        _catalog(),
        MultiInstrumentEngineConfig(8),
        checkpoint_interval=0,
    )
    run = run_multi_native(_executable(), no_checkpoint, commands)
    final = run.transcript.final
    assert final is not None
    advanced_snapshot = replace(final.snapshot, last_sequence=2)
    advanced_digest = engine_state_digest(advanced_snapshot)
    invalid_final = run.stdout.replace(
        final.final_state_digest,
        advanced_digest,
    ).replace(
        '"last_sequence":"1"',
        '"last_sequence":"2"',
        1,
    )

    with pytest.raises(MultiNativeProtocolError, match="final snapshot sequence"):
        decode_multi_jsonl(
            invalid_final,
            expected_mode="exact",
            expected_input=no_checkpoint,
            expected_commands=commands,
            returncode=0,
        )


def test_v2_decoder_binds_event_instrument_to_submitted_command() -> None:
    config = MultiNativeInput(
        _catalog(),
        MultiInstrumentEngineConfig(8),
    )
    commands: tuple[Command, ...] = (
        NewOrder(11, 1, 7, 255, OrderType.LIMIT, TimeInForce.GTC, 100, 5),
    )
    run = run_multi_native(_executable(), config, commands)
    result = run.transcript.results[0]
    assert result.events is not None
    assert result.event_digest is not None
    event = result.events[0]
    routed_elsewhere = replace(
        event,
        header=replace(event.header, instrument_id=9),
    )
    routed_digest = event_digest(EventBatch((routed_elsewhere,)))
    lines = run.stdout.splitlines()
    lines[1] = (
        lines[1]
        .replace(
            '"event_index":"0","instrument_id":"7"',
            '"event_index":"0","instrument_id":"9"',
            1,
        )
        .replace(
            result.event_digest,
            routed_digest,
            1,
        )
    )
    invalid = "\n".join(lines) + "\n"

    with pytest.raises(MultiNativeProtocolError, match="event instrument"):
        decode_multi_jsonl(
            invalid,
            expected_mode="exact",
            expected_input=config,
            expected_commands=commands,
            returncode=0,
        )


def test_v2_decoder_checks_event_prices_against_instrument_config() -> None:
    config = MultiNativeInput(
        (
            InstrumentConfig(
                7,
                MatchingConfig(1_000, 5, 8),
            ),
        ),
        MultiInstrumentEngineConfig(8),
    )
    commands: tuple[Command, ...] = (
        NewOrder(11, 1, 7, Side.SELL, OrderType.LIMIT, TimeInForce.GTC, 95, 5),
        NewOrder(22, 2, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 5),
    )
    run = run_multi_native(_executable(), config, commands)
    result = run.transcript.results[1]
    assert result.events is not None
    assert result.event_digest is not None
    altered_events = tuple(
        replace(event, execution_price=96) if isinstance(event, TradeEvent) else event
        for event in result.events
    )
    altered_digest = event_digest(EventBatch(altered_events))
    lines = run.stdout.splitlines()
    lines[2] = (
        lines[2]
        .replace(
            '"execution_price":"95"',
            '"execution_price":"96"',
            1,
        )
        .replace(
            result.event_digest,
            altered_digest,
            1,
        )
    )
    invalid = "\n".join(lines) + "\n"

    with pytest.raises(MultiNativeProtocolError, match="aligned"):
        decode_multi_jsonl(
            invalid,
            expected_mode="exact",
            expected_input=config,
            expected_commands=commands,
            returncode=0,
        )


def test_importing_v2_adapter_does_not_change_v1_fixture_bytes() -> None:
    config = NativeInputConfig(
        instrument_id=7,
        engine=MatchingConfig(1_000, 1, 16),
        snapshot_interval=3,
    )
    commands: tuple[Command, ...] = (
        NewOrder(11, 1, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 5),
    )

    assert encode_stream(config, commands) == (
        "ATLAS_DIFF_V1 7 1000 1 16 3\nN 11 1 7 1 1 1 1 100 5\n"
    )
