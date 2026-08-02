from __future__ import annotations

import importlib
import os
import subprocess
import sys
from collections.abc import Callable, Iterable, Iterator
from copy import copy
from pathlib import Path
from types import ModuleType
from typing import TYPE_CHECKING, Self, cast

import pytest
from atlaslob.domain import (
    I64_MAX,
    I64_MIN,
    U8_MAX,
    U32_MAX,
    U64_MAX,
    AcceptedEvent,
    CancelOrder,
    Command,
    CommandType,
    EngineError,
    InstrumentConfig,
    MatchingConfig,
    NewOrder,
    OrderType,
    RejectReason,
    ReplaceOrder,
    Side,
    TimeInForce,
)

if TYPE_CHECKING:
    from atlaslob.domain import RejectedEvent
    from atlaslob.engine import (
        ColumnBatch,
        Durability,
        LogHeader,
        ObjectBatch,
        OutputMode,
        ReplayDivergence,
        ReplayMode,
        ReplayReport,
        TailPolicy,
    )
    from atlaslob.engine import (
        Engine as PublicEngine,
    )
    from atlaslob.engine import (
        PersistenceError as PublicPersistenceError,
    )
    from atlaslob.engine import (
        ReadOnlyRecoveryError as PublicReadOnlyRecoveryError,
    )
    from atlaslob.engine import (
        RecoveryError as PublicRecoveryError,
    )
    from atlaslob.engine import (
        SnapshotError as PublicSnapshotError,
    )

_DIGEST = "ab" * 32
_ENGINE_EXPORTS = (
    "BatchResult",
    "ColumnBatch",
    "Engine",
    "EngineResult",
    "LogHeader",
    "ObjectBatch",
    "OperationErrorDetails",
    "PersistenceError",
    "ReadOnlyRecoveryError",
    "RecoveryError",
    "RecoveryReport",
    "ReplayDivergence",
    "ReplayEvidence",
    "ReplayReport",
    "SkippedSnapshot",
    "SnapshotError",
    "SnapshotPublication",
    "SnapshotPublicationReport",
    "SummaryBatch",
)
_COLUMN_TYPES = {
    "command_event_offsets": "Q",
    "command_outcomes": "B",
    "engine_error_present": "B",
    "engine_errors": "B",
    "command_sequence": "Q",
    "event_index": "I",
    "event_type": "B",
    "instrument_id": "I",
    "accepted_command_type": "B",
    "accepted_command_type_present": "B",
    "rejected_command_type": "B",
    "rejected_command_type_present": "B",
    "reject_reason": "H",
    "reject_reason_present": "B",
    "rejected_order_id": "Q",
    "rejected_order_id_present": "B",
    "trade_aggressor_order_id": "Q",
    "trade_aggressor_order_id_present": "B",
    "trade_resting_order_id": "Q",
    "trade_resting_order_id_present": "B",
    "trade_aggressor_client_id": "I",
    "trade_aggressor_client_id_present": "B",
    "trade_resting_client_id": "I",
    "trade_resting_client_id_present": "B",
    "trade_aggressor_side": "B",
    "trade_aggressor_side_present": "B",
    "trade_execution_price": "q",
    "trade_execution_price_present": "B",
    "trade_execution_quantity": "Q",
    "trade_execution_quantity_present": "B",
    "trade_aggressor_remaining": "Q",
    "trade_aggressor_remaining_present": "B",
    "trade_resting_remaining": "Q",
    "trade_resting_remaining_present": "B",
    "rested_order_id": "Q",
    "rested_order_id_present": "B",
    "rested_client_id": "I",
    "rested_client_id_present": "B",
    "rested_side": "B",
    "rested_side_present": "B",
    "rested_price": "q",
    "rested_price_present": "B",
    "rested_remaining_quantity": "Q",
    "rested_remaining_quantity_present": "B",
    "canceled_order_id": "Q",
    "canceled_order_id_present": "B",
    "canceled_quantity": "Q",
    "canceled_quantity_present": "B",
    "replaced_old_order_id": "Q",
    "replaced_old_order_id_present": "B",
    "replaced_new_order_id": "Q",
    "replaced_new_order_id_present": "B",
    "done_order_id": "Q",
    "done_order_id_present": "B",
    "done_reason": "B",
    "done_reason_present": "B",
    "done_remaining_quantity": "Q",
    "done_remaining_quantity_present": "B",
    "book_changed_best_bid_price": "q",
    "book_changed_best_bid_price_present": "B",
    "book_changed_best_bid_aggregate_quantity": "Q",
    "book_changed_best_bid_aggregate_quantity_present": "B",
    "book_changed_best_ask_price": "q",
    "book_changed_best_ask_price_present": "B",
    "book_changed_best_ask_aggregate_quantity": "Q",
    "book_changed_best_ask_aggregate_quantity_present": "B",
}


class _FakePersistenceError(RuntimeError):
    pass


class _FakeRecoveryError(RuntimeError):
    pass


class _FakeSnapshotError(RuntimeError):
    pass


class _FakeReadOnlyError(RuntimeError):
    pass


def _error_details(**overrides: object) -> dict[str, object]:
    details: dict[str, object] = {
        "category": "io_failure",
        "byte_offset": 42,
        "system_error_value": 5,
        "system_error_message": "injected I/O failure",
        "session_poisoned": False,
        "prefix_batch": None,
        "recovery_report": None,
        "publication_report": None,
    }
    details.update(overrides)
    return details


def _replay_report(*, tail_policy: str = "strict", tail: str = "clean") -> dict[str, object]:
    return {
        "kind": "replay",
        "mode": "verify",
        "tail_policy": tail_policy,
        "tail": tail,
        "header": {
            "format_version": 1,
            "semantics_version": 6,
            "log_id": "01" * 16,
            "first_sequence": 1,
            "engine_config": {"max_total_active_orders": 17},
            "catalog": [
                {
                    "instrument_id": 7,
                    "max_order_quantity": 2_000,
                    "tick_increment": 5,
                    "max_active_orders": 9,
                }
            ],
        },
        "last_sequence": 0,
        "valid_end_offset": 256,
        "records_scanned": 0,
        "records_replayed": 0,
        "committed": 0,
        "rejected": 0,
        "used_valid_prefix": tail == "torn",
        "warning": None,
        "error": None,
        "divergence": None,
        "final_state_digest": _DIGEST,
    }


def _event_result(
    sequence: int,
    command: dict[str, object],
) -> dict[str, object]:
    rejected = command.get("side") == 255
    if rejected:
        event: dict[str, object] = {
            "type": 2,
            "command_sequence": sequence,
            "event_index": 0,
            "instrument_id": command["instrument_id"],
            "command_type": 1,
            "reason": int(RejectReason.INVALID_SIDE),
            "order_id": command["order_id"],
        }
    else:
        command_type = {"new": 1, "cancel": 2, "replace": 3}[cast(str, command["type"])]
        event = {
            "type": 1,
            "command_sequence": sequence,
            "event_index": 0,
            "instrument_id": command["instrument_id"],
            "command_type": command_type,
        }
    return {
        "error": None,
        "events": [event],
        "command_sequence": sequence,
        "instrument_id": command["instrument_id"],
        "committed": not rejected,
        "rejected": rejected,
    }


def _column_payload(results: list[dict[str, object]]) -> dict[str, object]:
    event_count = len(results)
    payload: dict[str, list[int]] = {name: [] for name in _COLUMN_TYPES}
    payload["command_event_offsets"] = list(range(event_count + 1))
    for result in results:
        event = cast(list[dict[str, object]], result["events"])[0]
        rejected = bool(result["rejected"])
        payload["command_outcomes"].append(2 if rejected else 1)
        payload["engine_error_present"].append(0)
        payload["engine_errors"].append(0)
        payload["command_sequence"].append(cast(int, event["command_sequence"]))
        payload["event_index"].append(0)
        payload["event_type"].append(cast(int, event["type"]))
        payload["instrument_id"].append(cast(int, event["instrument_id"]))
        for name in _COLUMN_TYPES:
            if name not in payload or name in {
                "command_event_offsets",
                "command_outcomes",
                "engine_error_present",
                "engine_errors",
                "command_sequence",
                "event_index",
                "event_type",
                "instrument_id",
            }:
                continue
            payload[name].append(0)
        if rejected:
            payload["rejected_command_type"][-1] = 1
            payload["rejected_command_type_present"][-1] = 1
            payload["reject_reason"][-1] = int(RejectReason.INVALID_SIDE)
            payload["reject_reason_present"][-1] = 1
            payload["rejected_order_id"][-1] = cast(int, event["order_id"])
            payload["rejected_order_id_present"][-1] = 1
        else:
            payload["accepted_command_type"][-1] = cast(int, event["command_type"])
            payload["accepted_command_type_present"][-1] = 1
    return cast(dict[str, object], payload)


class _FakeNativeEngine:
    instances: list[_FakeNativeEngine] = []
    recover_failure: BaseException | None = None

    def __init__(
        self,
        catalog: list[dict[str, object]],
        max_total_active_orders: int,
    ) -> None:
        self.catalog = catalog
        self.max_total_active_orders = max_total_active_orders
        self.logged = False
        self.read_only = False
        self.poisoned = False
        self.recovery_report: dict[str, object] | None = None
        self.next_sequence = 1
        self.submit_calls = 0
        self.last_commands: list[dict[str, object]] = []
        self.raise_next: BaseException | None = None
        self.snapshot_failure: BaseException | None = None
        self.terminal_error_next = False
        self.path: str | None = None
        self.durability: str | None = None
        self.__class__.instances.append(self)

    @classmethod
    def create_logged(
        cls,
        path: str,
        catalog: list[dict[str, object]],
        max_total_active_orders: int,
        durability: str,
    ) -> Self:
        instance = cls(catalog, max_total_active_orders)
        instance.logged = True
        instance.path = path
        instance.durability = durability
        return instance

    @classmethod
    def recover(
        cls,
        log_path: str,
        snapshot_path: str | None,
        snapshot_dir: str | None,
        mode: str,
        tail_policy: str,
        durability: str,
    ) -> Self:
        del snapshot_path, snapshot_dir, mode
        if cls.recover_failure is not None:
            failure = cls.recover_failure
            cls.recover_failure = None
            raise failure
        instance = cls([_native_catalog_entry(7)], U64_MAX)
        instance.path = log_path
        instance.durability = durability
        instance.read_only = tail_policy == "valid-prefix"
        instance.logged = not instance.read_only
        instance.recovery_report = _replay_report(
            tail_policy=tail_policy, tail="torn" if instance.read_only else "clean"
        )
        return instance

    def submit(self, command: dict[str, object]) -> dict[str, object]:
        batch = self.submit_batch([command], "objects")
        return cast(list[dict[str, object]], batch["payload"])[0]

    def submit_batch(
        self,
        commands: list[dict[str, object]],
        output: str,
    ) -> dict[str, object]:
        self.submit_calls += 1
        self.last_commands = commands
        if self.raise_next is not None:
            failure = self.raise_next
            self.raise_next = None
            raise failure
        if self.terminal_error_next and commands:
            self.terminal_error_next = False
            result = {
                "error": 1,
                "events": [],
                "command_sequence": None,
                "instrument_id": None,
                "committed": False,
                "rejected": False,
            }
            return {
                "submitted_count": len(commands),
                "processed_count": 1,
                "committed_count": 0,
                "rejected_count": 0,
                "terminal_error": 1,
                "final_state_digest": _DIGEST,
                "output": output,
                "payload": [result] if output == "objects" else None,
            }
        results = [
            _event_result(self.next_sequence + index, command)
            for index, command in enumerate(commands)
        ]
        self.next_sequence += len(commands)
        committed = sum(not bool(result["rejected"]) for result in results)
        if output == "objects":
            payload: object = results
        elif output == "columns":
            payload = _column_payload(results)
        else:
            payload = None
        return {
            "submitted_count": len(commands),
            "processed_count": len(commands),
            "committed_count": committed,
            "rejected_count": len(commands) - committed,
            "terminal_error": None,
            "final_state_digest": _DIGEST,
            "output": output,
            "payload": payload,
        }

    def _submit_batch_for_measurement(
        self,
        commands: list[dict[str, object]],
        output: str,
    ) -> dict[str, object]:
        result = self.submit_batch(commands, output)
        result.pop("final_state_digest")
        return result

    def top(self, instrument_id: int) -> dict[str, object] | None:
        if instrument_id != 7:
            return None
        return {
            "best_bid": {"price": 100, "aggregate_quantity": 5},
            "best_ask": None,
        }

    def snapshot(self, instrument_id: int | None) -> dict[str, object] | None:
        instrument = {
            "instrument_id": 7,
            "active_order_count": 0,
            "bids": [],
            "asks": [],
        }
        if instrument_id is not None:
            return instrument if instrument_id == 7 else None
        return {
            "semantics_version": 6,
            "engine_config": {
                "max_total_active_orders": self.max_total_active_orders,
            },
            "catalog": [
                {
                    "instrument_id": 7,
                    "matching": {
                        "max_order_quantity": U64_MAX,
                        "tick_increment": 1,
                        "max_active_orders": U64_MAX,
                    },
                }
            ],
            "last_sequence": self.next_sequence - 1,
            "sequence_exhausted": False,
            "active_order_count": 0,
            "instruments": [instrument],
        }

    def state_digest(self) -> str:
        return _DIGEST

    def write_snapshot(self, directory: str) -> dict[str, object]:
        if self.snapshot_failure is not None:
            failure = self.snapshot_failure
            self.snapshot_failure = None
            raise failure
        return {
            "path": str(Path(directory) / "atlaslob-test.snapshot"),
            "covered_sequence": self.next_sequence - 1,
            "covered_log_byte_offset": 512,
            "encoded_bytes": 128,
            "final_file_visible": True,
            "error": None,
        }


def _native_catalog_entry(instrument_id: int) -> dict[str, object]:
    return {
        "instrument_id": instrument_id,
        "max_order_quantity": U64_MAX,
        "tick_increment": 1,
        "max_active_orders": U64_MAX,
    }


def _install_fake_module(monkeypatch: pytest.MonkeyPatch) -> ModuleType:
    module = ModuleType("atlaslob._native_engine")
    module.__dict__.update(
        {
            "BINDING_ABI": 2,
            "SEMANTICS_VERSION": 6,
            "NativePersistenceError": _FakePersistenceError,
            "NativeRecoveryError": _FakeRecoveryError,
            "NativeSnapshotError": _FakeSnapshotError,
            "NativeReadOnlyError": _FakeReadOnlyError,
            "NativeEngine": _FakeNativeEngine,
        }
    )
    monkeypatch.setitem(sys.modules, "atlaslob._native_engine", module)
    return module


@pytest.fixture
def engine_api(
    monkeypatch: pytest.MonkeyPatch,
) -> Iterator[tuple[ModuleType, type[_FakeNativeEngine]]]:
    import atlaslob

    old_engine_module = sys.modules.pop("atlaslob.engine", None)
    package_namespace = cast(dict[str, object], vars(atlaslob))
    for name in _ENGINE_EXPORTS:
        package_namespace.pop(name, None)
    _FakeNativeEngine.instances.clear()
    _FakeNativeEngine.recover_failure = None
    _install_fake_module(monkeypatch)
    module = importlib.import_module("atlaslob.engine")
    try:
        yield module, _FakeNativeEngine
    finally:
        sys.modules.pop("atlaslob.engine", None)
        for name in _ENGINE_EXPORTS:
            package_namespace.pop(name, None)
        if old_engine_module is not None:
            sys.modules["atlaslob.engine"] = old_engine_module


def _engine_type(module: ModuleType) -> type[PublicEngine]:
    return cast("type[PublicEngine]", module.__dict__["Engine"])


def _catalog() -> tuple[InstrumentConfig, ...]:
    return (
        InstrumentConfig(9, MatchingConfig(1_000, 5, 8)),
        InstrumentConfig(7, MatchingConfig(2_000, 1, 9)),
    )


def _new(*, side: int = Side.BUY, order_id: int = 1) -> NewOrder:
    return NewOrder(
        11,
        order_id,
        7,
        side,
        OrderType.LIMIT,
        TimeInForce.GTC,
        100,
        5,
    )


def test_catalog_is_exact_validated_and_canonically_normalized(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
) -> None:
    module, native_type = engine_api
    engine_type = _engine_type(module)
    engine = engine_type(_catalog(), max_total_active_orders=17)

    native = native_type.instances[-1]
    assert [entry["instrument_id"] for entry in native.catalog] == [7, 9]
    assert native.max_total_active_orders == 17
    assert native.catalog[0] == {
        "instrument_id": 7,
        "max_order_quantity": 2_000,
        "tick_increment": 1,
        "max_active_orders": 9,
    }
    assert not engine.logged
    assert not engine.read_only

    with pytest.raises(TypeError, match="exact InstrumentConfig"):
        engine_type(cast(Iterable[InstrumentConfig], ({"instrument_id": 7},)))
    with pytest.raises(ValueError, match="unique"):
        engine_type((InstrumentConfig(7), InstrumentConfig(7)))
    with pytest.raises(ValueError, match="max_total_active_orders"):
        engine_type((InstrumentConfig(7),), max_total_active_orders=cast(int, True))


@pytest.mark.parametrize(
    ("command", "expected_error"),
    (
        (_new(side=cast(int, True)), TypeError),
        (_new(order_id=U64_MAX + 1), OverflowError),
        (cast(Command, object()), TypeError),
    ),
)
def test_commands_reject_wrong_exact_types_bool_and_overflow_before_native(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
    command: Command,
    expected_error: type[Exception],
) -> None:
    module, native_type = engine_api
    engine = _engine_type(module)((InstrumentConfig(7),))

    with pytest.raises(expected_error):
        engine.submit(command)
    assert native_type.instances[-1].submit_calls == 0


@pytest.mark.parametrize(
    ("command", "field"),
    (
        (_new(), "client_id"),
        (_new(), "order_id"),
        (_new(), "instrument_id"),
        (_new(), "side"),
        (_new(), "order_type"),
        (_new(), "time_in_force"),
        (_new(), "limit_price"),
        (_new(), "quantity"),
        (CancelOrder(1, 2, 7), "client_id"),
        (CancelOrder(1, 2, 7), "order_id"),
        (CancelOrder(1, 2, 7), "instrument_id"),
        (ReplaceOrder(1, 2, 3, 7, 100, 5), "client_id"),
        (ReplaceOrder(1, 2, 3, 7, 100, 5), "old_order_id"),
        (ReplaceOrder(1, 2, 3, 7, 100, 5), "new_order_id"),
        (ReplaceOrder(1, 2, 3, 7, 100, 5), "instrument_id"),
        (ReplaceOrder(1, 2, 3, 7, 100, 5), "new_limit_price"),
        (ReplaceOrder(1, 2, 3, 7, 100, 5), "new_quantity"),
    ),
)
def test_every_command_integer_field_rejects_bool_before_native(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
    command: Command,
    field: str,
) -> None:
    module, native_type = engine_api
    engine = _engine_type(module)((InstrumentConfig(7),))
    malformed = copy(command)
    object.__setattr__(malformed, field, True)

    with pytest.raises(TypeError, match="integer"):
        engine.submit(malformed)

    assert native_type.instances[-1].submit_calls == 0


@pytest.mark.parametrize(
    "command",
    (
        NewOrder(-1, 1, 7, 1, 1, 1, 100, 5),
        NewOrder(U32_MAX + 1, 1, 7, 1, 1, 1, 100, 5),
        NewOrder(1, -1, 7, 1, 1, 1, 100, 5),
        NewOrder(1, U64_MAX + 1, 7, 1, 1, 1, 100, 5),
        NewOrder(1, 1, -1, 1, 1, 1, 100, 5),
        NewOrder(1, 1, U32_MAX + 1, 1, 1, 1, 100, 5),
        NewOrder(1, 1, 7, -1, 1, 1, 100, 5),
        NewOrder(1, 1, 7, U8_MAX + 1, 1, 1, 100, 5),
        NewOrder(1, 1, 7, 1, -1, 1, 100, 5),
        NewOrder(1, 1, 7, 1, U8_MAX + 1, 1, 100, 5),
        NewOrder(1, 1, 7, 1, 1, -1, 100, 5),
        NewOrder(1, 1, 7, 1, 1, U8_MAX + 1, 100, 5),
        NewOrder(1, 1, 7, 1, 1, 1, I64_MIN - 1, 5),
        NewOrder(1, 1, 7, 1, 1, 1, I64_MAX + 1, 5),
        NewOrder(1, 1, 7, 1, 1, 1, 100, -1),
        NewOrder(1, 1, 7, 1, 1, 1, 100, U64_MAX + 1),
        CancelOrder(-1, 1, 7),
        CancelOrder(1, U64_MAX + 1, 7),
        CancelOrder(1, 1, U32_MAX + 1),
        ReplaceOrder(1, -1, 2, 7, 100, 5),
        ReplaceOrder(1, 1, U64_MAX + 1, 7, 100, 5),
        ReplaceOrder(1, 1, 2, 7, I64_MIN - 1, 5),
        ReplaceOrder(1, 1, 2, 7, 100, U64_MAX + 1),
    ),
)
def test_every_command_representation_boundary_raises_overflow_before_native(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
    command: Command,
) -> None:
    module, native_type = engine_api
    engine = _engine_type(module)((InstrumentConfig(7),))

    with pytest.raises(OverflowError):
        engine.submit(command)

    assert native_type.instances[-1].submit_calls == 0


def test_cancel_and_replace_normalize_exact_variant_fields(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
) -> None:
    module, native_type = engine_api
    engine = _engine_type(module)((InstrumentConfig(7),))

    engine.submit(CancelOrder(11, 22, 7))
    assert native_type.instances[-1].last_commands == [
        {
            "type": "cancel",
            "client_id": 11,
            "order_id": 22,
            "instrument_id": 7,
        }
    ]
    engine.submit(ReplaceOrder(11, 22, 23, 7, -5, 9))
    assert native_type.instances[-1].last_commands == [
        {
            "type": "replace",
            "client_id": 11,
            "old_order_id": 22,
            "new_order_id": 23,
            "instrument_id": 7,
            "new_limit_price": -5,
            "new_quantity": 9,
        }
    ]


def test_late_malformed_batch_element_executes_nothing(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
) -> None:
    module, native_type = engine_api
    engine = _engine_type(module)((InstrumentConfig(7),))
    malformed = cast(Command, object())

    with pytest.raises(TypeError, match="exact NewOrder"):
        engine.submit_batch((_new(), _new(order_id=2), malformed))

    assert native_type.instances[-1].submit_calls == 0


def test_iteration_failure_executes_nothing_and_literal_options_are_closed(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
    tmp_path: Path,
) -> None:
    module, native_type = engine_api
    engine_type = _engine_type(module)
    engine = engine_type((InstrumentConfig(7),))

    def failing_commands() -> Iterator[Command]:
        yield _new()
        raise LookupError("injected iteration failure")

    with pytest.raises(LookupError, match="injected"):
        engine.submit_batch(failing_commands())
    with pytest.raises(TypeError, match="output"):
        engine.submit_batch((_new(),), output=cast("OutputMode", "OBJECTS"))
    with pytest.raises(TypeError, match="durability"):
        engine_type.create_logged(
            tmp_path / "log",
            (InstrumentConfig(7),),
            durability=cast("Durability", "sometimes"),
        )
    with pytest.raises(TypeError, match="mode"):
        engine_type.recover(tmp_path / "log", mode=cast("ReplayMode", "quick"))
    with pytest.raises(TypeError, match="tail_policy"):
        engine_type.recover(tmp_path / "log", tail_policy=cast("TailPolicy", "ignore"))

    assert native_type.instances[-1].submit_calls == 0


def test_submit_uses_one_object_batch_and_raw_enum_reaches_native(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
) -> None:
    module, native_type = engine_api
    engine = _engine_type(module)((InstrumentConfig(7),))

    accepted = engine.submit(_new())
    rejected = engine.submit(_new(side=255, order_id=2))

    native = native_type.instances[-1]
    assert native.submit_calls == 2
    assert native.last_commands[0]["side"] == 255
    assert accepted.committed
    assert accepted.command_sequence == 1
    assert rejected.rejected
    assert rejected.command_sequence == 2
    assert rejected.events is not None
    rejection = cast("RejectedEvent", rejected.events[0])
    assert rejection.reason is RejectReason.INVALID_SIDE


def test_terminal_engine_error_stops_batch_and_is_counted_as_processed(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
) -> None:
    module, native_type = engine_api
    engine = _engine_type(module)((InstrumentConfig(7),))
    native_type.instances[-1].terminal_error_next = True

    result = engine.submit_batch((_new(), _new(order_id=2)))

    assert result.submitted_count == 2
    assert result.processed_count == 1
    assert result.committed_count == 0
    assert result.rejected_count == 0
    assert result.terminal_error is not None
    payload = cast("ObjectBatch", result.payload)
    assert len(payload.results) == 1
    assert payload.results[0].error is result.terminal_error


def test_object_column_and_summary_batches_are_owned_and_consistent(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
) -> None:
    module, _ = engine_api
    engine = _engine_type(module)((InstrumentConfig(7),))

    objects = engine.submit_batch((_new(),), output="objects")
    columns = engine.submit_batch((_new(order_id=2),), output="columns")
    summary = engine.submit_batch((_new(order_id=3),), output="summary")

    assert objects.processed_count == columns.processed_count == summary.processed_count == 1
    assert objects.committed_count == columns.committed_count == summary.committed_count == 1
    assert (
        objects.final_state_digest
        == columns.final_state_digest
        == summary.final_state_digest
        == _DIGEST
    )
    column_payload = cast("ColumnBatch", columns.payload)
    assert column_payload.command_event_offsets.tolist() == [0, 1]
    assert column_payload.event_type.tolist() == [1]
    column_payload.event_type[0] = 99
    later = engine.submit_batch((_new(order_id=4),), output="columns")
    assert cast("ColumnBatch", later.payload).event_type.tolist() == [1]


def test_observers_decode_owned_domain_values(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
) -> None:
    module, _ = engine_api
    engine = _engine_type(module)((InstrumentConfig(7),))

    top = engine.top(7)
    instrument = engine.snapshot(7)
    whole = engine.snapshot()

    assert top is not None and top.best_bid is not None
    assert top.best_bid.price == 100
    assert engine.top(999) is None
    assert instrument is not None and instrument.instrument_id == 7
    assert whole.catalog == (InstrumentConfig(7),)
    assert engine.state_digest() == _DIGEST
    with pytest.raises(TypeError, match="integer"):
        engine.top(cast(int, True))


def test_logged_creation_paths_and_snapshot_publication_are_unicode_safe(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
    tmp_path: Path,
) -> None:
    module, native_type = engine_api
    engine_type = _engine_type(module)
    log_path = tmp_path / "日志-🚀.atlaslog"
    engine = engine_type.create_logged(
        log_path,
        (InstrumentConfig(7),),
        durability="flush_each_record",
    )

    assert engine.logged and not engine.read_only
    assert native_type.instances[-1].path == os.fspath(log_path)
    assert native_type.instances[-1].durability == "flush_each_record"
    publication = engine.write_snapshot(tmp_path / "快照")
    assert publication.path.name == "atlaslob-test.snapshot"
    assert publication.encoded_bytes == 128
    with pytest.raises(TypeError, match="not bytes"):
        engine_type.create_logged(
            cast(str, b"log"),
            (InstrumentConfig(7),),
        )


def test_valid_prefix_recovery_is_read_only_before_native_mutation(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
    tmp_path: Path,
) -> None:
    module, native_type = engine_api
    engine_type = _engine_type(module)
    engine = engine_type.recover(
        tmp_path / "torn.atlaslog",
        mode="verify",
        tail_policy="valid-prefix",
    )
    native = native_type.instances[-1]
    before = native.submit_calls

    assert not engine.logged
    assert engine.read_only
    assert engine.recovery_report is not None
    assert cast("ReplayReport", engine.recovery_report).tail == "torn"
    read_only_error = cast(
        "type[PublicReadOnlyRecoveryError]",
        module.__dict__["ReadOnlyRecoveryError"],
    )
    with pytest.raises(
        read_only_error,
        match="repair-tail",
    ) as prechecked:
        engine.submit(_new())
    assert prechecked.value.details.category == "read_only_recovery"
    assert prechecked.value.details.byte_offset == 0
    assert prechecked.value.details.system_error_value == 0
    assert prechecked.value.details.system_error_message == ""
    with pytest.raises(read_only_error):
        engine.write_snapshot(tmp_path)
    assert native.submit_calls == before

    with pytest.raises(TypeError, match="mutually exclusive"):
        engine_type.recover(
            tmp_path / "log",
            snapshot_path=tmp_path / "one",
            snapshot_dir=tmp_path / "many",
        )


def test_native_read_only_fallback_preserves_owned_error_details(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
) -> None:
    module, native_type = engine_api
    engine = _engine_type(module)((InstrumentConfig(7),))
    native = native_type.instances[-1]
    native.raise_next = _FakeReadOnlyError(
        "native read-only rejection",
        _error_details(
            category="read_only_recovery",
            byte_offset=73,
            system_error_value=9,
            system_error_message="native detail",
        ),
    )
    error_type = cast(
        "type[PublicReadOnlyRecoveryError]",
        module.__dict__["ReadOnlyRecoveryError"],
    )

    with pytest.raises(error_type, match="native read-only rejection") as raised:
        engine.submit(_new())

    assert raised.value.details.category == "read_only_recovery"
    assert raised.value.details.byte_offset == 73
    assert raised.value.details.system_error_value == 9
    assert raised.value.details.system_error_message == "native detail"


def test_persistence_failure_retains_poison_and_published_prefix(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
) -> None:
    module, native_type = engine_api
    engine = _engine_type(module)((InstrumentConfig(7),))
    native = native_type.instances[-1]
    prefix_result = _event_result(1, _native_command(_new()))
    prefix_batch = {
        "submitted_count": 2,
        "processed_count": 1,
        "committed_count": 1,
        "rejected_count": 0,
        "terminal_error": None,
        "final_state_digest": _DIGEST,
        "output": "objects",
        "payload": [prefix_result],
    }
    native.raise_next = _FakePersistenceError(
        "append failed",
        _error_details(session_poisoned=True, prefix_batch=prefix_batch),
    )
    public_error = cast("type[PublicPersistenceError]", module.__dict__["PersistenceError"])

    with pytest.raises(public_error, match="append failed") as raised:
        engine.submit_batch((_new(), _new(order_id=2)))

    failure = raised.value
    assert failure.session_poisoned is True
    prefix = failure.prefix_result
    assert prefix is not None
    assert prefix.processed_count == 1
    assert prefix.submitted_count == 2
    assert cast("ObjectBatch", prefix.payload).results[0].committed
    details = failure.details
    assert details.category == "io_failure"
    assert details.byte_offset == 42


def test_recovery_and_snapshot_failures_preserve_owned_structured_reports(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
    tmp_path: Path,
) -> None:
    module, native_type = engine_api
    engine_type = _engine_type(module)
    replay = _replay_report()
    replay["error"] = {
        "category": "bad_record_checksum",
        "byte_offset": 99,
        "system_error_value": 0,
        "system_error_message": "",
    }
    native_type.recover_failure = _FakeRecoveryError(
        "corrupt log",
        _error_details(
            category="bad_record_checksum",
            byte_offset=99,
            recovery_report=replay,
        ),
    )
    recovery_error = cast("type[PublicRecoveryError]", module.__dict__["RecoveryError"])

    with pytest.raises(recovery_error, match="corrupt log") as recovered:
        engine_type.recover(tmp_path / "corrupt.atlaslog")

    assert recovered.value.details.category == "bad_record_checksum"
    assert recovered.value.report is not None
    replay_report = cast("ReplayReport", recovered.value.report)
    assert replay_report.error is not None
    assert replay_report.error.byte_offset == 99

    engine = engine_type.create_logged(tmp_path / "good.atlaslog", (InstrumentConfig(7),))
    publication_report = {
        "path": None,
        "covered_sequence": 0,
        "covered_log_byte_offset": 256,
        "encoded_bytes": 0,
        "final_file_visible": False,
        "error": {
            "category": "io_failure",
            "byte_offset": 0,
            "system_error_value": 5,
            "system_error_message": "rename failed",
        },
    }
    native_type.instances[-1].snapshot_failure = _FakeSnapshotError(
        "snapshot failed",
        _error_details(publication_report=publication_report),
    )
    snapshot_error = cast("type[PublicSnapshotError]", module.__dict__["SnapshotError"])

    with pytest.raises(snapshot_error, match="snapshot failed") as published:
        engine.write_snapshot(tmp_path)

    assert published.value.publication is not None
    assert published.value.publication.path is None
    assert not published.value.publication.final_file_visible
    assert published.value.publication.error is not None
    assert published.value.publication.error.system_error_value == 5


def test_replay_report_preserves_complete_owned_header_and_divergence(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
    tmp_path: Path,
) -> None:
    module, native_type = engine_api
    source = _replay_report()
    source["last_sequence"] = 7
    expected: dict[str, object] = {
        "outcome": "committed",
        "rejection_reason": None,
        "event_count": 1,
        "event_digest": "cd" * 32,
    }
    actual: dict[str, object] = {
        "outcome": "rejected",
        "rejection_reason": int(RejectReason.INVALID_SIDE),
        "event_count": 1,
        "event_digest": "ef" * 32,
    }
    actual_event: dict[str, object] = {
        "type": 1,
        "command_sequence": 7,
        "event_index": 0,
        "instrument_id": 7,
        "command_type": 1,
    }
    actual_events: list[dict[str, object]] = [actual_event]
    source_command: dict[str, object] = {
        "type": "new",
        "client_id": 11,
        "order_id": 41,
        "instrument_id": 7,
        "side": 255,
        "order_type": 254,
        "time_in_force": 253,
        "limit_price": None,
        "quantity": 5,
    }
    source["divergence"] = {
        "record_offset": 321,
        "sequence": 7,
        "category": "outcome_mismatch",
        "command": source_command,
        "expected": expected,
        "actual": actual,
        "actual_engine_error": int(EngineError.SEQUENCE_EXHAUSTED),
        "actual_events": actual_events,
    }
    native_type.recover_failure = _FakeRecoveryError(
        "replay diverged",
        _error_details(category="replay_divergence", recovery_report=source),
    )
    error_type = cast("type[PublicRecoveryError]", module.__dict__["RecoveryError"])

    with pytest.raises(error_type) as raised:
        _engine_type(module).recover(tmp_path / "divergent.atlaslog")

    report = cast("ReplayReport", raised.value.report)
    header = cast("LogHeader", report.header)
    divergence = cast("ReplayDivergence", report.divergence)
    assert header.format_version == 1
    assert header.semantics_version == 6
    assert header.log_id == "01" * 16
    assert header.first_sequence == 1
    assert header.engine_config.max_total_active_orders == 17
    assert header.catalog == (InstrumentConfig(7, MatchingConfig(2_000, 5, 9)),)
    assert divergence.record_offset == 321
    assert divergence.sequence == 7
    assert divergence.category == "outcome_mismatch"
    assert divergence.command == NewOrder(11, 41, 7, 255, 254, 253, None, 5)
    expected_evidence = divergence.expected
    assert expected_evidence.outcome == "committed"
    assert expected_evidence.rejection_reason is None
    assert expected_evidence.event_count == 1
    assert expected_evidence.event_digest == "cd" * 32
    assert divergence.actual.outcome == "rejected"
    assert divergence.actual.rejection_reason is RejectReason.INVALID_SIDE
    assert divergence.actual.event_count == 1
    assert divergence.actual.event_digest == "ef" * 32
    assert divergence.actual_engine_error is EngineError.SEQUENCE_EXHAUSTED
    assert divergence.actual_events == (
        AcceptedEvent(
            header=divergence.actual_events[0].header,
            command_type=CommandType.NEW,
        ),
    )
    assert divergence.actual_events[0].header.command_sequence == 7
    assert report.divergence_category == "outcome_mismatch"
    assert report.divergence_sequence == 7

    source_header = cast(dict[str, object], source["header"])
    source_catalog = cast(list[dict[str, object]], source_header["catalog"])
    source_header["log_id"] = "ff" * 16
    source_catalog[0]["instrument_id"] = 99
    source_catalog.append(_native_catalog_entry(11))
    expected["outcome"] = "rejected"
    actual["event_digest"] = None
    source_command["order_id"] = 99
    source_command["limit_price"] = 100
    actual_event["command_sequence"] = 999
    actual_events.clear()

    assert header.log_id == "01" * 16
    assert header.catalog == (InstrumentConfig(7, MatchingConfig(2_000, 5, 9)),)
    assert divergence.expected.outcome == "committed"
    assert divergence.actual.event_digest == "ef" * 32
    assert divergence.command == NewOrder(11, 41, 7, 255, 254, 253, None, 5)
    assert len(divergence.actual_events) == 1
    assert divergence.actual_events[0].header.command_sequence == 7


@pytest.mark.parametrize("log_id", ["01" * 15, "AB" * 16])
def test_replay_header_rejects_noncanonical_log_id(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
    log_id: str,
) -> None:
    module, _ = engine_api
    source = _replay_report()
    header = cast(dict[str, object], source["header"])
    header["log_id"] = log_id
    decode = cast(
        "Callable[[object], ReplayReport]",
        module.__dict__["_decode_replay_report"],
    )

    with pytest.raises(RuntimeError, match="lowercase 128-bit log ID"):
        decode(source)


@pytest.mark.parametrize(
    ("command", "expected"),
    [
        (
            {
                "type": "new",
                "client_id": 3,
                "order_id": 4,
                "instrument_id": 7,
                "side": 255,
                "order_type": 254,
                "time_in_force": 253,
                "limit_price": None,
                "quantity": 8,
            },
            NewOrder(3, 4, 7, 255, 254, 253, None, 8),
        ),
        (
            {
                "type": "cancel",
                "client_id": 3,
                "order_id": 4,
                "instrument_id": 7,
            },
            CancelOrder(3, 4, 7),
        ),
        (
            {
                "type": "replace",
                "client_id": 3,
                "old_order_id": 4,
                "new_order_id": 5,
                "instrument_id": 7,
                "new_limit_price": -10,
                "new_quantity": 8,
            },
            ReplaceOrder(3, 4, 5, 7, -10, 8),
        ),
    ],
)
def test_replay_divergence_decodes_every_command_shape(
    engine_api: tuple[ModuleType, type[_FakeNativeEngine]],
    command: dict[str, object],
    expected: Command,
) -> None:
    module, _ = engine_api
    source = _replay_report()
    source["divergence"] = {
        "record_offset": 321,
        "sequence": 7,
        "category": "command_mismatch",
        "command": command,
        "expected": {
            "outcome": None,
            "rejection_reason": None,
            "event_count": None,
            "event_digest": None,
        },
        "actual": {
            "outcome": None,
            "rejection_reason": None,
            "event_count": None,
            "event_digest": None,
        },
        "actual_engine_error": None,
        "actual_events": [],
    }
    decode = cast(
        "Callable[[object], ReplayReport]",
        module.__dict__["_decode_replay_report"],
    )

    report = decode(source)

    assert report.divergence is not None
    assert report.divergence.command == expected


def _native_command(command: NewOrder) -> dict[str, object]:
    return {
        "type": "new",
        "client_id": command.client_id,
        "order_id": command.order_id,
        "instrument_id": command.instrument_id,
        "side": int(command.side),
        "order_type": int(command.order_type),
        "time_in_force": int(command.time_in_force),
        "limit_price": command.limit_price,
        "quantity": command.quantity,
    }


def test_binding_abi_mismatch_fails_clearly_without_reference_fallback(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    old_engine_module = sys.modules.pop("atlaslob.engine", None)
    module = _install_fake_module(monkeypatch)
    module.__dict__["BINDING_ABI"] = 999
    try:
        with pytest.raises(ImportError, match="ABI mismatch"):
            importlib.import_module("atlaslob.engine")
    finally:
        sys.modules.pop("atlaslob.engine", None)
        if old_engine_module is not None:
            sys.modules["atlaslob.engine"] = old_engine_module


@pytest.mark.parametrize(
    ("marker", "message"),
    [
        ("BINDING_ABI", "ABI mismatch"),
        ("SEMANTICS_VERSION", "semantic-version mismatch"),
    ],
)
def test_missing_native_compatibility_marker_fails_with_clear_import_error(
    monkeypatch: pytest.MonkeyPatch,
    marker: str,
    message: str,
) -> None:
    old_engine_module = sys.modules.pop("atlaslob.engine", None)
    module = _install_fake_module(monkeypatch)
    module.__dict__.pop(marker)
    try:
        with pytest.raises(ImportError, match=message) as raised:
            importlib.import_module("atlaslob.engine")
        assert "<missing>" in str(raised.value)
    finally:
        sys.modules.pop("atlaslob.engine", None)
        if old_engine_module is not None:
            sys.modules["atlaslob.engine"] = old_engine_module


def test_oracle_imports_work_when_native_extension_is_deliberately_blocked() -> None:
    script = """
import importlib.abc
import sys

class BlockNative(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "atlaslob._native_engine":
            raise ImportError("deliberately blocked")
        return None

sys.meta_path.insert(0, BlockNative())
import atlaslob
from atlaslob import ReferenceEngine, ReferenceRouter
from atlaslob.canonical import state_digest
assert ReferenceEngine is not None
assert ReferenceRouter is not None
assert state_digest is not None
try:
    from atlaslob import Engine
except ImportError as exc:
    text = str(exc)
    assert "native engine is unavailable" in text
    assert "fallback" in text
else:
    raise AssertionError("native Engine unexpectedly imported")
"""
    environment = os.environ.copy()
    source = str(Path("python/src").resolve())
    environment["PYTHONPATH"] = (
        source
        if not environment.get("PYTHONPATH")
        else source + os.pathsep + environment["PYTHONPATH"]
    )
    completed = subprocess.run(
        [sys.executable, "-c", script],
        check=False,
        capture_output=True,
        text=True,
        env=environment,
    )
    assert completed.returncode == 0, completed.stderr
