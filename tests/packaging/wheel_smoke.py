#!/usr/bin/env python3
"""Stdlib-only acceptance suite for an installed native AtlasLOB distribution."""

from __future__ import annotations

import gc
import shutil
import subprocess
import sys
import tempfile
import threading
from collections.abc import Iterable
from importlib import import_module
from importlib.metadata import version
from pathlib import Path
from typing import cast

import atlaslob._native_engine as native_backend
from atlaslob import (
    ATLASLOB_SEMANTICS_VERSION,
    CancelOrder,
    ColumnBatch,
    Command,
    Engine,
    EngineSnapshot,
    Event,
    InstrumentConfig,
    NewOrder,
    ObjectBatch,
    OrderType,
    ReadOnlyRecoveryError,
    RecoveryError,
    RecoveryReport,
    ReferenceResult,
    ReferenceRouter,
    RejectedEvent,
    RejectReason,
    ReplaceOrder,
    ReplayReport,
    Side,
    SummaryBatch,
    TimeInForce,
)
from atlaslob.domain import event_type
from atlaslob.engine import BatchResult, TailPolicy

U64_MAX = (1 << 64) - 1


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def _limit_order(
    *,
    client_id: int,
    order_id: int,
    instrument_id: int,
    side: int,
    price: int,
    quantity: int,
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


def _catalog() -> tuple[InstrumentConfig, ...]:
    return (InstrumentConfig(instrument_id=7), InstrumentConfig(instrument_id=8))


def _workload() -> tuple[Command, ...]:
    return (
        _limit_order(
            client_id=1,
            order_id=101,
            instrument_id=7,
            side=Side.BUY,
            price=100,
            quantity=5,
        ),
        _limit_order(
            client_id=2,
            order_id=201,
            instrument_id=8,
            side=Side.SELL,
            price=200,
            quantity=3,
        ),
        _limit_order(
            client_id=3,
            order_id=102,
            instrument_id=7,
            side=Side.SELL,
            price=100,
            quantity=2,
        ),
        ReplaceOrder(
            client_id=1,
            old_order_id=101,
            new_order_id=103,
            instrument_id=7,
            new_limit_price=99,
            new_quantity=4,
        ),
        CancelOrder(client_id=9, order_id=999, instrument_id=7),
        CancelOrder(client_id=2, order_id=201, instrument_id=8),
        NewOrder(
            client_id=4,
            order_id=104,
            instrument_id=7,
            side=Side.BUY,
            order_type=OrderType.MARKET,
            time_in_force=TimeInForce.IOC,
            limit_price=None,
            quantity=1,
        ),
    )


def _reference_results(
    catalog: tuple[InstrumentConfig, ...], commands: tuple[Command, ...]
) -> tuple[ReferenceRouter, tuple[ReferenceResult, ...]]:
    router = ReferenceRouter(catalog)
    return router, tuple(router.execute(command) for command in commands)


def _verify_object_results(payload: ObjectBatch, expected: tuple[ReferenceResult, ...]) -> None:
    _require(len(payload.results) == len(expected), "object result count differs from oracle")
    for index, (actual, reference) in enumerate(zip(payload.results, expected, strict=True)):
        if reference.error is not None:
            _require(actual.error == reference.error, f"result {index} engine error differs")
            _require(actual.events is None, f"result {index} unexpectedly has events")
            continue
        if reference.batch is None:
            raise AssertionError(f"oracle result {index} has no outcome")
        _require(actual.error is None, f"result {index} unexpectedly has an engine error")
        _require(
            actual.events == reference.batch.events,
            f"result {index} events differ from ReferenceRouter",
        )


def _verify_column_results(columns: ColumnBatch, objects: ObjectBatch) -> None:
    offsets = [0]
    flattened_events: list[Event] = []
    command_outcomes: list[int] = []
    for result in objects.results:
        events = () if result.events is None else result.events
        flattened_events.extend(events)
        offsets.append(len(flattened_events))
        command_outcomes.append(1 if result.committed else 2 if result.rejected else 3)

    _require(
        list(columns["command_event_offsets"]) == offsets,
        "column command-to-event offsets differ from object output",
    )
    _require(
        list(columns["command_outcomes"]) == command_outcomes,
        "column command outcomes differ from object output",
    )
    _require(
        list(columns["command_sequence"])
        == [event.header.command_sequence for event in flattened_events],
        "column command sequences differ from object output",
    )
    _require(
        list(columns["event_index"]) == [event.header.event_index for event in flattened_events],
        "column event indices differ from object output",
    )
    _require(
        list(columns["event_type"]) == [int(event_type(event)) for event in flattened_events],
        "column event types differ from object output",
    )
    _require(
        list(columns["instrument_id"])
        == [event.header.instrument_id for event in flattened_events],
        "column instrument IDs differ from object output",
    )
    _require(
        sum(columns["trade_execution_price_present"]) >= 1,
        "column output did not retain trade fields",
    )
    _require(
        sum(columns["rejected_order_id_present"]) >= 1,
        "column output did not retain rejection fields",
    )
    _require(
        sum(columns["replaced_new_order_id_present"]) >= 1,
        "column output did not retain replacement fields",
    )


def _exercise_batch_parity() -> tuple[ObjectBatch, ColumnBatch, EngineSnapshot]:
    catalog = _catalog()
    commands = _workload()
    reference, reference_results = _reference_results(catalog, commands)
    reference_digest = reference.state_digest()
    reference_snapshot = reference.snapshot()
    expected_committed = sum(result.committed for result in reference_results)
    expected_rejected = sum(result.rejected for result in reference_results)

    object_engine = Engine(catalog)
    object_result = object_engine.submit_batch(commands, output="objects")
    if not isinstance(object_result.payload, ObjectBatch):
        raise AssertionError("objects mode returned wrong payload")
    object_payload = object_result.payload
    _verify_object_results(object_payload, reference_results)

    column_engine = Engine(catalog)
    column_result = column_engine.submit_batch(commands, output="columns")
    if not isinstance(column_result.payload, ColumnBatch):
        raise AssertionError("columns mode returned wrong payload")
    column_payload = column_result.payload
    _verify_column_results(column_payload, object_payload)

    summary_engine = Engine(catalog)
    summary_result = summary_engine.submit_batch(commands, output="summary")
    _require(
        isinstance(summary_result.payload, SummaryBatch),
        "summary mode returned wrong payload",
    )

    for label, result, engine in (
        ("objects", object_result, object_engine),
        ("columns", column_result, column_engine),
        ("summary", summary_result, summary_engine),
    ):
        _require(result.submitted_count == len(commands), f"{label} submitted count differs")
        _require(result.processed_count == len(commands), f"{label} processed count differs")
        _require(result.committed_count == expected_committed, f"{label} committed count differs")
        _require(result.rejected_count == expected_rejected, f"{label} rejected count differs")
        _require(result.terminal_error is None, f"{label} unexpectedly terminated")
        _require(result.final_state_digest == reference_digest, f"{label} digest differs")
        _require(engine.state_digest() == reference_digest, f"{label} engine digest differs")
        _require(engine.snapshot() == reference_snapshot, f"{label} snapshot differs")

    object_snapshot = object_engine.snapshot()
    digest_before_column_mutation = column_engine.state_digest()
    original_event_type = column_payload["event_type"][0]
    column_payload["event_type"][0] = 255
    _require(
        column_engine.state_digest() == digest_before_column_mutation,
        "mutating returned columns changed engine state",
    )
    column_payload["event_type"][0] = original_event_type
    return object_payload, column_payload, object_snapshot


def _exercise_conversion_boundaries() -> None:
    catalog = (InstrumentConfig(instrument_id=7),)

    malformed_engine = Engine(catalog)
    before = malformed_engine.snapshot()
    malformed_commands = cast(
        Iterable[Command],
        (
            _limit_order(
                client_id=1,
                order_id=1,
                instrument_id=7,
                side=Side.BUY,
                price=100,
                quantity=1,
            ),
            object(),
        ),
    )
    try:
        malformed_engine.submit_batch(malformed_commands)
    except TypeError:
        pass
    else:
        raise AssertionError("late malformed batch element was accepted")
    _require(
        malformed_engine.snapshot() == before,
        "late malformed batch element allowed prefix execution",
    )

    bool_engine = Engine(catalog)
    bool_command = _limit_order(
        client_id=1,
        order_id=2,
        instrument_id=7,
        side=Side.BUY,
        price=100,
        quantity=True,
    )
    try:
        bool_engine.submit(bool_command)
    except TypeError:
        pass
    else:
        raise AssertionError("bool was accepted as an integer command field")
    _require(bool_engine.snapshot().last_sequence == 0, "bool failure consumed a sequence")

    raw_enum_result = bool_engine.submit(
        _limit_order(
            client_id=1,
            order_id=3,
            instrument_id=7,
            side=255,
            price=100,
            quantity=1,
        )
    )
    _require(raw_enum_result.rejected, "raw invalid enum did not reach domain rejection")
    _require(raw_enum_result.command_sequence == 1, "raw invalid enum did not consume a sequence")
    if raw_enum_result.events is None:
        raise AssertionError("raw invalid enum produced no event")
    first_event = raw_enum_result.events[0]
    if not isinstance(first_event, RejectedEvent):
        raise AssertionError("raw invalid enum did not emit rejection")
    _require(first_event.reason is RejectReason.INVALID_SIDE, "raw invalid enum reason differs")


def _exercise_owned_lifetimes(
    objects: ObjectBatch, columns: ColumnBatch, snapshot: EngineSnapshot
) -> None:
    event_copy = tuple(result.events for result in objects.results)
    offsets_copy = tuple(columns["command_event_offsets"])
    snapshot_copy = snapshot
    gc.collect()
    _require(
        tuple(result.events for result in objects.results) == event_copy,
        "events lost ownership",
    )
    _require(tuple(columns["command_event_offsets"]) == offsets_copy, "columns lost ownership")
    _require(snapshot == snapshot_copy, "snapshot lost ownership")


def _expect_recovery_error(path: Path, *, tail_policy: TailPolicy) -> None:
    try:
        Engine.recover(path, tail_policy=tail_policy)
    except RecoveryError:
        return
    raise AssertionError(f"{tail_policy} recovery unexpectedly accepted {path.name}")


def _replay_report(engine: Engine) -> ReplayReport:
    report = engine.recovery_report
    if isinstance(report, ReplayReport):
        return report
    if isinstance(report, RecoveryReport):
        return report.replay
    raise AssertionError("recovered engine has no replay report")


def _copy_prefix(source: Path, destination: Path, byte_count: int) -> None:
    with source.open("rb") as input_file, destination.open("xb") as output_file:
        remaining = byte_count
        while remaining:
            chunk = input_file.read(min(remaining, 64 * 1024))
            if not chunk:
                raise AssertionError("valid replay boundary exceeds torn input")
            output_file.write(chunk)
            remaining -= len(chunk)
        output_file.flush()


def _exercise_persistence_and_recovery() -> None:
    catalog = (InstrumentConfig(instrument_id=7),)
    with tempfile.TemporaryDirectory(prefix="atlaslob-λ-wheel-") as temporary:
        root = Path(temporary)
        log_path = root / "命令-log.atlas"
        snapshot_directory = root / "快照"
        snapshot_directory.mkdir()

        logged = Engine.create_logged(log_path, catalog)
        resting = _limit_order(
            client_id=1,
            order_id=7001,
            instrument_id=7,
            side=Side.BUY,
            price=101,
            quantity=7,
        )
        _require(logged.submit(resting).committed, "logged engine did not commit valid order")
        publication = logged.write_snapshot(snapshot_directory)
        _require(publication.path.exists(), "published snapshot is not visible")
        snapshot_digest = logged.state_digest()
        del logged
        gc.collect()

        directory_recovered = Engine.recover(
            log_path, snapshot_dir=snapshot_directory, mode="diagnostic"
        )
        _require(
            directory_recovered.logged and not directory_recovered.read_only,
            "clean snapshot-directory recovery is not writable",
        )
        _require(
            directory_recovered.state_digest() == snapshot_digest,
            "snapshot-directory recovery digest differs",
        )
        directory_report = directory_recovered.recovery_report
        if not isinstance(directory_report, RecoveryReport):
            raise AssertionError("snapshot-directory recovery has no recovery report")
        _require(
            directory_report.selected_snapshot == publication.path,
            "snapshot-directory recovery selected the wrong snapshot",
        )
        _require(
            directory_recovered.submit(
                CancelOrder(client_id=9, order_id=999, instrument_id=7)
            ).rejected,
            "clean snapshot-directory recovery did not remain writable",
        )
        directory_digest = directory_recovered.state_digest()
        del directory_recovered
        gc.collect()

        recovered = Engine.recover(log_path, snapshot_path=publication.path, mode="diagnostic")
        _require(recovered.logged and not recovered.read_only, "clean recovery is not writable")
        _require(recovered.state_digest() == directory_digest, "snapshot recovery digest differs")
        _require(
            recovered.submit(CancelOrder(client_id=1, order_id=7001, instrument_id=7)).committed,
            "clean recovered engine did not remain writable",
        )
        clean_digest = recovered.state_digest()
        del recovered
        gc.collect()

        torn_path = root / "torn.atlas"
        shutil.copyfile(log_path, torn_path)
        with torn_path.open("ab") as torn_file:
            torn_file.write(b"\x00\x00")

        _expect_recovery_error(torn_path, tail_policy="strict")
        read_only = Engine.recover(torn_path, tail_policy="valid-prefix")
        _require(read_only.read_only and not read_only.logged, "valid prefix is not read-only")
        _require(read_only.state_digest() == clean_digest, "valid-prefix digest differs")
        replay = _replay_report(read_only)
        _require(replay.used_valid_prefix, "valid-prefix recovery did not report its warning")

        directory_read_only = Engine.recover(
            torn_path,
            snapshot_dir=snapshot_directory,
            mode="diagnostic",
            tail_policy="valid-prefix",
        )
        _require(
            directory_read_only.read_only and not directory_read_only.logged,
            "torn snapshot-directory recovery is not read-only",
        )
        _require(
            directory_read_only.state_digest() == clean_digest,
            "torn snapshot-directory valid-prefix digest differs",
        )
        del directory_read_only
        gc.collect()

        mutation = _limit_order(
            client_id=2,
            order_id=7002,
            instrument_id=7,
            side=Side.SELL,
            price=102,
            quantity=1,
        )
        try:
            read_only.submit(mutation)
        except ReadOnlyRecoveryError:
            pass
        else:
            raise AssertionError("valid-prefix read-only engine accepted a command")
        try:
            read_only.write_snapshot(snapshot_directory)
        except ReadOnlyRecoveryError:
            pass
        else:
            raise AssertionError("valid-prefix read-only engine published a snapshot")

        valid_end_offset = replay.valid_end_offset
        del read_only
        gc.collect()
        repaired_path = root / "repaired-copy.atlas"
        _copy_prefix(torn_path, repaired_path, valid_end_offset)
        repaired = Engine.recover(repaired_path, tail_policy="strict")
        _require(repaired.logged and not repaired.read_only, "repaired prefix is not writable")
        _require(repaired.state_digest() == clean_digest, "repaired prefix digest differs")
        _require(repaired.submit(mutation).committed, "repaired strict recovery did not append")
        del repaired
        gc.collect()

        corrupt_path = root / "corrupt.atlas"
        corrupt_bytes = bytearray(log_path.read_bytes())
        _require(bool(corrupt_bytes), "command log is unexpectedly empty")
        corrupt_bytes[-1] ^= 0x01
        corrupt_path.write_bytes(corrupt_bytes)
        _expect_recovery_error(corrupt_path, tail_policy="strict")
        _expect_recovery_error(corrupt_path, tail_policy="valid-prefix")


def _object_sequences(result: BatchResult) -> tuple[int, ...]:
    payload = result.payload
    if not isinstance(payload, ObjectBatch):
        raise AssertionError("threaded batch returned wrong payload")
    sequences = tuple(item.command_sequence for item in payload.results)
    _require(all(sequence is not None for sequence in sequences), "threaded result has no sequence")
    return cast(tuple[int, ...], sequences)


def _exercise_same_engine_noninterleaving() -> None:
    engine = Engine((InstrumentConfig(instrument_id=7),))
    command = CancelOrder(client_id=1, order_id=999, instrument_id=7)
    commands = (command,) * 512
    barrier = threading.Barrier(3)
    outputs: list[BatchResult | None] = [None, None]
    failures: list[BaseException] = []

    def worker(index: int) -> None:
        try:
            barrier.wait()
            outputs[index] = engine.submit_batch(commands, output="objects")
        except BaseException as error:
            failures.append(error)

    threads = [threading.Thread(target=worker, args=(index,)) for index in range(2)]
    for thread in threads:
        thread.start()
    barrier.wait()
    for thread in threads:
        thread.join(timeout=30)
    _require(not any(thread.is_alive() for thread in threads), "threaded batches did not finish")
    if failures:
        raise AssertionError("threaded batch failed") from failures[0]
    _require(all(output is not None for output in outputs), "threaded batch produced no output")

    ranges = []
    for output in outputs:
        if output is None:
            raise AssertionError("threaded batch produced no output")
        sequences = _object_sequences(output)
        _require(
            sequences == tuple(range(sequences[0], sequences[0] + len(sequences))),
            "one native batch received a noncontiguous sequence range",
        )
        ranges.append((sequences[0], sequences[-1]))
    _require(
        sorted(ranges) == [(1, len(commands)), (len(commands) + 1, len(commands) * 2)],
        "same-engine batches interleaved",
    )


def _exercise_observable_gil_release() -> None:
    catalog: list[dict[str, object]] = [
        {
            "instrument_id": 7,
            "max_order_quantity": U64_MAX,
            "tick_increment": 1,
            "max_active_orders": U64_MAX,
        }
    ]
    engine = native_backend.NativeEngine(catalog, U64_MAX)
    command: dict[str, object] = {
        "type": "cancel",
        "client_id": 1,
        "order_id": 999,
        "instrument_id": 7,
    }
    commands = [command] * 100_000
    ready = threading.Event()
    stop = threading.Event()
    counter = [0]

    def observer() -> None:
        ready.set()
        while not stop.is_set():
            counter[0] += 1

    thread = threading.Thread(target=observer)
    thread.start()
    _require(ready.wait(timeout=10), "observer thread did not start")
    baseline = counter[0]
    try:
        result = engine.submit_batch(commands, "summary")
    finally:
        stop.set()
        thread.join(timeout=30)
    _require(not thread.is_alive(), "GIL observer thread did not stop")
    _require(result.get("processed_count") == len(commands), "large native batch was incomplete")
    _require(
        counter[0] - baseline > 1_000,
        "another Python thread made no observable progress during native execution",
    )


def _exercise_exception_type_lifetime() -> None:
    child = """
import gc
import tempfile
from pathlib import Path

import atlaslob._native_engine as native

del native.NativePersistenceError
gc.collect()
with tempfile.TemporaryDirectory(prefix="atlaslob-exception-lifetime-") as temporary:
    existing = Path(temporary) / "existing.log"
    existing.touch()
    catalog = [{
        "instrument_id": 7,
        "max_order_quantity": (1 << 64) - 1,
        "tick_increment": 1,
        "max_active_orders": (1 << 64) - 1,
    }]
    try:
        native.NativeEngine.create_logged(
            str(existing), catalog, (1 << 64) - 1, "sync_each_record"
        )
    except BaseException as error:
        if type(error).__name__ != "NativePersistenceError":
            raise AssertionError(f"unexpected native exception type: {type(error)!r}") from error
    else:
        raise AssertionError("existing log path did not fail")
"""
    subprocess.run(
        [sys.executable, "-c", child],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def _verify_installation() -> None:
    _require(version("atlaslob") == "0.2.0", "unexpected installed AtlasLOB version")
    _require(ATLASLOB_SEMANTICS_VERSION == 6, "unexpected AtlasLOB semantics version")
    native_engine = import_module("atlaslob._native_engine")
    _require(native_engine.__file__ is not None, "native extension has no import path")
    extension_path = Path(cast(str, native_engine.__file__)).resolve()
    _require(
        extension_path.suffix in {".pyd", ".so"},
        f"native extension was not loaded: {extension_path}",
    )
    _require(native_backend.BINDING_ABI == 1, "unexpected private binding ABI")


def _pip_check() -> None:
    subprocess.run(
        [sys.executable, "-m", "pip", "check"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def main() -> int:
    _verify_installation()
    objects, columns, snapshot = _exercise_batch_parity()
    _exercise_conversion_boundaries()
    _exercise_owned_lifetimes(objects, columns, snapshot)
    _exercise_persistence_and_recovery()
    _exercise_same_engine_noninterleaving()
    _exercise_observable_gil_release()
    _exercise_exception_type_lifetime()
    _pip_check()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
