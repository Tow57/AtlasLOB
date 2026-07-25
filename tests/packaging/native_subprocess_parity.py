"""Compare the installed pybind engine with both Phase 4 correctness oracles."""

from __future__ import annotations

import argparse
from pathlib import Path

from atlaslob.canonical import engine_state_digest, event_digest
from atlaslob.domain import (
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
)
from atlaslob.engine import Engine, ObjectBatch
from atlaslob.multi_differential import (
    capture_reference_router,
    normalized_economic_state,
)
from atlaslob.multi_native import (
    MultiNativeInput,
    assert_multi_native_parity,
    run_multi_native,
)


def _catalog() -> tuple[InstrumentConfig, ...]:
    return (
        InstrumentConfig(7, MatchingConfig(1_000, 1, 8)),
        InstrumentConfig(9, MatchingConfig(1_000, 1, 8)),
    )


def _commands() -> tuple[Command, ...]:
    return (
        NewOrder(11, 100, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 8),
        NewOrder(12, 101, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 99, 4),
        NewOrder(22, 200, 9, Side.SELL, OrderType.LIMIT, TimeInForce.GTC, 110, 7),
        NewOrder(13, 201, 7, Side.SELL, OrderType.LIMIT, TimeInForce.IOC, 100, 3),
        ReplaceOrder(11, 100, 102, 7, 101, 5),
        NewOrder(22, 200, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 98, 2),
        CancelOrder(22, 200, 7),
        NewOrder(23, 204, 9, Side.SELL, OrderType.LIMIT, TimeInForce.GTC, 111, 2),
        NewOrder(24, 205, 9, Side.SELL, OrderType.LIMIT, TimeInForce.GTC, 112, 2),
        CancelOrder(22, 200, 9),
        NewOrder(25, 206, 9, Side.BUY, OrderType.MARKET, TimeInForce.IOC, None, 1),
        NewOrder(26, 207, 7, 255, OrderType.LIMIT, TimeInForce.GTC, 98, 1),
        NewOrder(27, 208, 11, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 98, 1),
        NewOrder(28, 209, 9, Side.SELL, OrderType.LIMIT, TimeInForce.GTC, 105, 6),
        NewOrder(29, 210, 9, Side.BUY, OrderType.LIMIT, TimeInForce.IOC, 106, 4),
        CancelOrder(28, 209, 9),
        CancelOrder(12, 101, 7),
        CancelOrder(11, 102, 7),
    )


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def _run(native_executable: Path) -> None:
    catalog = _catalog()
    commands = _commands()
    engine_config = MultiInstrumentEngineConfig(max_total_active_orders=4)
    adapter = run_multi_native(
        native_executable,
        MultiNativeInput(catalog, engine_config, checkpoint_interval=1),
        commands,
        mode="exact",
    )
    reference = capture_reference_router(
        catalog,
        commands,
        engine_config=engine_config,
    )
    assert_multi_native_parity(adapter, reference)

    transcript = adapter.transcript
    _require(adapter.returncode == 0, "ATLAS_DIFF_V2 adapter did not exit successfully")
    final = transcript.final
    if final is None:
        raise AssertionError("ATLAS_DIFF_V2 adapter omitted its final record")
    _require(
        len(transcript.results) == len(reference.records),
        "adapter and reference processed different command counts",
    )

    incremental = Engine(
        catalog,
        max_total_active_orders=engine_config.max_total_active_orders,
    )
    incremental_results = []
    for index, (command, adapter_record, reference_record) in enumerate(
        zip(commands, transcript.results, reference.records, strict=True)
    ):
        binding_result = incremental.submit(command)
        incremental_results.append(binding_result)
        expected = reference_record.result
        _require(
            binding_result.error == expected.error == adapter_record.engine_error,
            f"engine error differs at command {index}",
        )
        if expected.batch is None:
            _require(binding_result.events is None, f"unexpected events at command {index}")
        else:
            binding_events = binding_result.events
            if binding_events is None:
                raise AssertionError(f"binding omitted events at command {index}")
            _require(
                binding_events == expected.batch.events == adapter_record.events,
                f"events differ at command {index}",
            )
            _require(
                event_digest(EventBatch(binding_events)) == adapter_record.event_digest,
                f"event digest differs at command {index}",
            )

        binding_snapshot = incremental.snapshot()
        _require(
            binding_snapshot == reference_record.post_snapshot,
            f"binding and reference snapshots differ at command {index}",
        )
        _require(
            binding_snapshot == adapter_record.checkpoint_snapshot,
            f"binding and adapter snapshots differ at command {index}",
        )
        _require(
            normalized_economic_state(binding_snapshot)
            == normalized_economic_state(reference_record.post_snapshot),
            f"normalized economic state differs at command {index}",
        )
        _require(
            incremental.state_digest()
            == reference_record.post_state_digest
            == adapter_record.post_state_digest
            == engine_state_digest(binding_snapshot),
            f"state digest differs at command {index}",
        )

    batch_engine = Engine(
        catalog,
        max_total_active_orders=engine_config.max_total_active_orders,
    )
    batch = batch_engine.submit_batch(commands, output="objects")
    payload = batch.payload
    if not isinstance(payload, ObjectBatch):
        raise AssertionError("object batch returned the wrong payload")
    _require(
        payload.results == tuple(incremental_results),
        "batch and one-at-a-time binding results differ",
    )
    _require(batch.submitted_count == len(commands), "binding submitted count differs")
    _require(batch.processed_count == len(commands), "binding processed count differs")
    _require(
        batch.committed_count == final.committed,
        "binding and adapter committed counts differ",
    )
    _require(
        batch.rejected_count == final.rejected,
        "binding and adapter rejected counts differ",
    )
    _require(batch.terminal_error is None, "binding unexpectedly returned a terminal error")
    _require(
        batch_engine.snapshot()
        == incremental.snapshot()
        == reference.final_snapshot
        == final.snapshot,
        "final snapshots differ",
    )
    _require(
        batch.final_state_digest
        == batch_engine.state_digest()
        == reference.final_state_digest
        == final.final_state_digest,
        "final state digests differ",
    )
    adapter_rejections = tuple(
        record.reject_reason for record in transcript.results if record.reject_reason is not None
    )
    _require(
        adapter_rejections
        == (
            RejectReason.DUPLICATE_ORDER_ID,
            RejectReason.INSTRUMENT_MISMATCH,
            RejectReason.CAPACITY_EXCEEDED,
            RejectReason.INVALID_SIDE,
            RejectReason.UNKNOWN_INSTRUMENT,
        ),
        "acceptance workload did not exercise the expected rejection precedence",
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="compare the installed AtlasLOB extension with ATLAS_DIFF_V2"
    )
    parser.add_argument(
        "--native",
        type=Path,
        required=True,
        help="path to the atlas_diff_multi_native executable",
    )
    arguments = parser.parse_args()
    native_executable = arguments.native.resolve()
    if not native_executable.is_file():
        parser.error(f"native adapter is not a file: {native_executable}")
    _run(native_executable)
    print("installed pybind engine matches ATLAS_DIFF_V2 and ReferenceRouter")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
