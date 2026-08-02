"""Native multi-instrument engine API.

This module is intentionally separate from the independent Python oracle.  It
loads the private CPython extension, validates its binding ABI, converts every
input before execution, and returns owned value objects.
"""

from __future__ import annotations

import os
import re
from array import array
from collections.abc import Iterable, Iterator, Mapping
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path
from types import MappingProxyType
from typing import Literal, TypeAlias, TypeVar, cast, overload

from atlaslob.domain import (
    ATLASLOB_SEMANTICS_VERSION,
    I64_MAX,
    I64_MIN,
    U8_MAX,
    U16_MAX,
    U32_MAX,
    U64_MAX,
    AcceptedEvent,
    BookChangedEvent,
    BookTop,
    CanceledEvent,
    CancelOrder,
    Command,
    CommandType,
    DoneEvent,
    DoneReason,
    EngineError,
    EngineSnapshot,
    Event,
    EventHeader,
    EventType,
    InstrumentConfig,
    InstrumentSnapshot,
    MatchingConfig,
    MultiInstrumentEngineConfig,
    NewOrder,
    OrderSnapshot,
    PriceLevelSnapshot,
    RejectedEvent,
    RejectReason,
    ReplacedEvent,
    ReplaceOrder,
    RestedEvent,
    Side,
    TopOfBookLevel,
    TradeEvent,
)

try:
    from atlaslob import _native_engine as _native
except ImportError as exc:
    raise ImportError(
        "AtlasLOB's native engine is unavailable. Install an atlaslob wheel that matches "
        "this CPython version and platform; the reference model is not used as a fallback."
    ) from exc

_BINDING_ABI = 2
_native_binding_abi = getattr(_native, "BINDING_ABI", None)
if _native_binding_abi != _BINDING_ABI:
    observed_abi = "<missing>" if _native_binding_abi is None else repr(_native_binding_abi)
    raise ImportError(
        "AtlasLOB native binding ABI mismatch: "
        f"Python expects {_BINDING_ABI}, extension provides {observed_abi}. "
        "Reinstall a matching atlaslob wheel."
    )
_native_semantics_version = getattr(_native, "SEMANTICS_VERSION", None)
if _native_semantics_version != ATLASLOB_SEMANTICS_VERSION:
    observed_semantics = (
        "<missing>" if _native_semantics_version is None else repr(_native_semantics_version)
    )
    raise ImportError(
        "AtlasLOB semantic-version mismatch: "
        f"Python expects {ATLASLOB_SEMANTICS_VERSION}, extension provides "
        f"{observed_semantics}. Reinstall a matching atlaslob wheel."
    )

OutputMode: TypeAlias = Literal["objects", "columns", "summary"]
Durability: TypeAlias = Literal["buffered", "flush_each_record", "sync_each_record"]
ReplayMode: TypeAlias = Literal["fast", "verify", "diagnostic"]
TailPolicy: TypeAlias = Literal["strict", "valid-prefix"]
_PathInput: TypeAlias = str | os.PathLike[str]
_NativeRecord: TypeAlias = dict[str, object]

_OUTPUT_MODES = frozenset(("objects", "columns", "summary"))
_DURABILITY_MODES = frozenset(("buffered", "flush_each_record", "sync_each_record"))
_REPLAY_MODES = frozenset(("fast", "verify", "diagnostic"))
_TAIL_POLICIES = frozenset(("strict", "valid-prefix"))
_REPLAY_OUTCOMES = frozenset(("committed", "rejected"))
_DIGEST = re.compile(r"[0-9a-f]{64}\Z")
_LOG_ID = re.compile(r"[0-9a-f]{32}\Z")
_READ_ONLY_MESSAGE = (
    "this engine was recovered from a torn log's valid prefix and is read-only; run "
    "`atlas_inspect repair-tail <input> <new-output>` and then recover the new file "
    "with tail_policy='strict' before mutating it"
)


@dataclass(frozen=True, slots=True)
class EngineResult:
    """One sequenced event batch or one terminal engine error."""

    events: tuple[Event, ...] | None = None
    error: EngineError | None = None

    def __post_init__(self) -> None:
        if (self.events is None) == (self.error is None):
            raise ValueError("engine result must contain exactly one event tuple or engine error")
        if self.events is not None and not self.events:
            raise ValueError("a successful engine result must contain at least one event")

    @property
    def committed(self) -> bool:
        return self.events is not None and isinstance(self.events[0], AcceptedEvent)

    @property
    def rejected(self) -> bool:
        return self.events is not None and isinstance(self.events[0], RejectedEvent)

    @property
    def command_sequence(self) -> int | None:
        return None if self.events is None else self.events[0].header.command_sequence

    @property
    def instrument_id(self) -> int | None:
        return None if self.events is None else self.events[0].header.instrument_id


@dataclass(frozen=True, slots=True)
class ObjectBatch:
    """Owned object-mode results in command order."""

    results: tuple[EngineResult, ...]


@dataclass(frozen=True, slots=True)
class SummaryBatch:
    """Explicit marker for a summary-mode batch with no per-command payload."""


@dataclass(frozen=True, slots=True)
class ColumnBatch:
    """Owned standard-library arrays for column-oriented processing.

    The mapping itself is immutable.  Its arrays are intentionally caller-owned
    and mutable; changing them cannot affect the engine or another result.
    """

    columns: Mapping[str, array[int]]

    def __post_init__(self) -> None:
        copied = {name: array(values.typecode, values) for name, values in self.columns.items()}
        object.__setattr__(self, "columns", MappingProxyType(copied))

    def __getitem__(self, name: str) -> array[int]:
        return self.columns[name]

    def __iter__(self) -> Iterator[str]:
        return iter(self.columns)

    def keys(self) -> Iterator[str]:
        return iter(self.columns)

    @property
    def command_event_offsets(self) -> array[int]:
        return self.columns["command_event_offsets"]

    @property
    def command_outcome(self) -> array[int]:
        return self.columns["command_outcomes"]

    @property
    def command_outcomes(self) -> array[int]:
        return self.columns["command_outcomes"]

    @property
    def engine_error_present(self) -> array[int]:
        return self.columns["engine_error_present"]

    @property
    def engine_errors(self) -> array[int]:
        return self.columns["engine_errors"]

    @property
    def event_command_sequence(self) -> array[int]:
        return self.columns["command_sequence"]

    @property
    def event_index(self) -> array[int]:
        return self.columns["event_index"]

    @property
    def event_type(self) -> array[int]:
        return self.columns["event_type"]

    @property
    def event_instrument_id(self) -> array[int]:
        return self.columns["instrument_id"]


BatchPayload: TypeAlias = ObjectBatch | ColumnBatch | SummaryBatch


@dataclass(frozen=True, slots=True)
class BatchResult:
    """Result of a prefix-committing native batch."""

    submitted_count: int
    processed_count: int
    committed_count: int
    rejected_count: int
    terminal_error: EngineError | None
    final_state_digest: str
    payload: BatchPayload


@dataclass(frozen=True, slots=True)
class _MeasurementBatchResult:
    """Private batch result whose final digest is validated at a region boundary."""

    submitted_count: int
    processed_count: int
    committed_count: int
    rejected_count: int
    terminal_error: EngineError | None
    payload: BatchPayload


@dataclass(frozen=True, slots=True)
class _DecodedBatch:
    record: Mapping[str, object]
    submitted_count: int
    processed_count: int
    committed_count: int
    rejected_count: int
    terminal_error: EngineError | None
    payload: BatchPayload


@dataclass(frozen=True, slots=True)
class OperationErrorDetails:
    """Stable, owned details for persistence and recovery failures."""

    category: str
    byte_offset: int = 0
    system_error_value: int = 0
    system_error_message: str = ""


@dataclass(frozen=True, slots=True)
class LogHeader:
    """Canonical configuration and identity decoded from a command-log header."""

    format_version: int
    semantics_version: int
    log_id: str
    first_sequence: int
    engine_config: MultiInstrumentEngineConfig
    catalog: tuple[InstrumentConfig, ...]


@dataclass(frozen=True, slots=True)
class ReplayEvidence:
    """Expected or actual classification evidence for one replayed record."""

    outcome: Literal["committed", "rejected"] | None
    rejection_reason: RejectReason | None
    event_count: int | None
    event_digest: str | None


@dataclass(frozen=True, slots=True)
class ReplayDivergence:
    """Owned details for the first replay result that differs from its log record."""

    record_offset: int
    sequence: int
    category: str
    command: Command | None
    expected: ReplayEvidence
    actual: ReplayEvidence
    actual_engine_error: EngineError | None
    actual_events: tuple[Event, ...]


@dataclass(frozen=True, slots=True)
class ReplayReport:
    mode: ReplayMode
    tail_policy: TailPolicy
    tail: str
    header: LogHeader | None
    last_sequence: int | None
    valid_end_offset: int
    records_scanned: int
    records_replayed: int
    committed_count: int
    rejected_count: int
    used_valid_prefix: bool
    final_state_digest: str | None
    warning: OperationErrorDetails | None
    error: OperationErrorDetails | None
    divergence: ReplayDivergence | None

    @property
    def divergence_category(self) -> str | None:
        """Compatibility view of the full divergence details."""

        return None if self.divergence is None else self.divergence.category

    @property
    def divergence_sequence(self) -> int | None:
        """Compatibility view of the full divergence details."""

        return None if self.divergence is None else self.divergence.sequence


@dataclass(frozen=True, slots=True)
class SkippedSnapshot:
    path: Path
    filename_sequence: int | None
    error: OperationErrorDetails


@dataclass(frozen=True, slots=True)
class RecoveryReport:
    recovery_source: str
    selected_snapshot: Path | None
    covered_sequence: int | None
    covered_log_byte_offset: int | None
    snapshot_state_digest: str | None
    skipped_snapshots: tuple[SkippedSnapshot, ...]
    snapshot_error: OperationErrorDetails | None
    replay: ReplayReport


@dataclass(frozen=True, slots=True)
class SnapshotPublication:
    path: Path
    covered_sequence: int
    covered_log_byte_offset: int
    encoded_bytes: int


@dataclass(frozen=True, slots=True)
class SnapshotPublicationReport:
    path: Path | None
    covered_sequence: int
    covered_log_byte_offset: int
    encoded_bytes: int
    final_file_visible: bool
    error: OperationErrorDetails | None


class PersistenceError(RuntimeError):
    """A write, flush, synchronization, or logged-session operation failed."""

    def __init__(
        self,
        message: str,
        *,
        details: OperationErrorDetails,
        session_poisoned: bool,
        prefix_result: BatchResult | None = None,
        report: RecoveryReport | ReplayReport | None = None,
    ) -> None:
        super().__init__(message)
        self.details = details
        self.session_poisoned = session_poisoned
        self.prefix_result = prefix_result
        self.report = report


class RecoveryError(RuntimeError):
    """A log or snapshot could not be structurally and semantically recovered."""

    def __init__(
        self,
        message: str,
        *,
        details: OperationErrorDetails,
        report: RecoveryReport | ReplayReport | None = None,
    ) -> None:
        super().__init__(message)
        self.details = details
        self.report = report


class SnapshotError(RuntimeError):
    """Snapshot encoding, validation, or publication failed."""

    def __init__(
        self,
        message: str,
        *,
        details: OperationErrorDetails,
        publication: SnapshotPublicationReport | None = None,
    ) -> None:
        super().__init__(message)
        self.details = details
        self.publication = publication


class ReadOnlyRecoveryError(RuntimeError):
    """A mutation was attempted on valid-prefix inspection state."""

    def __init__(
        self,
        message: str = _READ_ONLY_MESSAGE,
        *,
        details: OperationErrorDetails | None = None,
    ) -> None:
        super().__init__(message)
        self.details = details or OperationErrorDetails("read_only_recovery")


class Engine:
    """Native deterministic multi-instrument matching engine."""

    __slots__ = ("_backend", "_recovery_report")

    def __init__(
        self,
        catalog: Iterable[InstrumentConfig],
        *,
        max_total_active_orders: int = U64_MAX,
    ) -> None:
        native_catalog = _normalize_catalog(catalog)
        native_capacity = _config_uint("max_total_active_orders", max_total_active_orders, U64_MAX)
        try:
            self._backend = _native.NativeEngine(native_catalog, native_capacity)
        except (
            _native.NativePersistenceError,
            _native.NativeRecoveryError,
            _native.NativeSnapshotError,
            _native.NativeReadOnlyError,
        ) as exc:
            _raise_mapped_native_error(exc)
        self._recovery_report = _decode_optional_recovery_report(self._backend.recovery_report)

    @classmethod
    def create_logged(
        cls,
        path: _PathInput,
        catalog: Iterable[InstrumentConfig],
        *,
        max_total_active_orders: int = U64_MAX,
        durability: Durability = "sync_each_record",
    ) -> Engine:
        native_path = _normalize_path("path", path)
        native_catalog = _normalize_catalog(catalog)
        native_capacity = _config_uint("max_total_active_orders", max_total_active_orders, U64_MAX)
        native_durability = _literal("durability", durability, _DURABILITY_MODES)
        try:
            backend = _native.NativeEngine.create_logged(
                native_path, native_catalog, native_capacity, native_durability
            )
        except (
            _native.NativePersistenceError,
            _native.NativeRecoveryError,
            _native.NativeSnapshotError,
            _native.NativeReadOnlyError,
        ) as exc:
            _raise_mapped_native_error(exc)
        return cls._from_backend(backend)

    @classmethod
    def recover(
        cls,
        log_path: _PathInput,
        *,
        snapshot_path: _PathInput | None = None,
        snapshot_dir: _PathInput | None = None,
        mode: ReplayMode = "verify",
        tail_policy: TailPolicy = "strict",
        durability: Durability = "sync_each_record",
    ) -> Engine:
        if snapshot_path is not None and snapshot_dir is not None:
            raise TypeError("snapshot_path and snapshot_dir are mutually exclusive")
        native_log_path = _normalize_path("log_path", log_path)
        native_snapshot_path = (
            None if snapshot_path is None else _normalize_path("snapshot_path", snapshot_path)
        )
        native_snapshot_dir = (
            None if snapshot_dir is None else _normalize_path("snapshot_dir", snapshot_dir)
        )
        native_mode = _literal("mode", mode, _REPLAY_MODES)
        native_tail_policy = _literal("tail_policy", tail_policy, _TAIL_POLICIES)
        native_durability = _literal("durability", durability, _DURABILITY_MODES)
        try:
            backend = _native.NativeEngine.recover(
                native_log_path,
                native_snapshot_path,
                native_snapshot_dir,
                native_mode,
                native_tail_policy,
                native_durability,
            )
        except (
            _native.NativePersistenceError,
            _native.NativeRecoveryError,
            _native.NativeSnapshotError,
            _native.NativeReadOnlyError,
        ) as exc:
            _raise_mapped_native_error(exc)
        return cls._from_backend(backend)

    @classmethod
    def _from_backend(cls, backend: _native.NativeEngine) -> Engine:
        instance = cls.__new__(cls)
        instance._backend = backend
        instance._recovery_report = _decode_optional_recovery_report(backend.recovery_report)
        return instance

    @property
    def logged(self) -> bool:
        return self._backend.logged

    @property
    def read_only(self) -> bool:
        return self._backend.read_only

    @property
    def poisoned(self) -> bool:
        return self._backend.poisoned

    @property
    def recovery_report(self) -> RecoveryReport | ReplayReport | None:
        return self._recovery_report

    def submit(self, command: Command) -> EngineResult:
        self._ensure_writable()
        normalized = _normalize_command(command)
        native_batch = self._submit_normalized((normalized,), "objects")
        if not isinstance(native_batch.payload, ObjectBatch):
            raise RuntimeError("native binding returned the wrong payload for object mode")
        if len(native_batch.payload.results) != 1:
            raise RuntimeError("native one-command batch returned an invalid result count")
        return native_batch.payload.results[0]

    def submit_batch(
        self,
        commands: Iterable[Command],
        *,
        output: OutputMode = "objects",
    ) -> BatchResult:
        self._ensure_writable()
        native_output = _literal("output", output, _OUTPUT_MODES)
        normalized = tuple(_normalize_command(command) for command in commands)
        return self._submit_normalized(normalized, cast(OutputMode, native_output))

    def _submit_batch_for_measurement(
        self,
        commands: Iterable[Command],
        *,
        output: OutputMode = "objects",
    ) -> _MeasurementBatchResult:
        """Submit one logical batch without producing a redundant state digest."""

        self._ensure_writable()
        if self.logged:
            raise RuntimeError("measurement batches require a live in-memory engine")
        native_output = _literal("output", output, _OUTPUT_MODES)
        normalized = tuple(_normalize_command(command) for command in commands)
        record = self._submit_native(normalized, cast(OutputMode, native_output), measurement=True)
        return _decode_measurement_batch_result(record, cast(OutputMode, native_output))

    def _submit_normalized(
        self,
        commands: tuple[_NativeRecord, ...],
        output: OutputMode,
    ) -> BatchResult:
        record = self._submit_native(commands, output, measurement=False)
        return _decode_batch_result(record, output)

    def _submit_native(
        self,
        commands: tuple[_NativeRecord, ...],
        output: OutputMode,
        *,
        measurement: bool,
    ) -> object:
        try:
            if measurement:
                return self._backend._submit_batch_for_measurement(list(commands), output)
            return self._backend.submit_batch(list(commands), output)
        except (
            _native.NativePersistenceError,
            _native.NativeRecoveryError,
            _native.NativeSnapshotError,
            _native.NativeReadOnlyError,
        ) as exc:
            _raise_mapped_native_error(exc, output=output)
            raise AssertionError("native error mapping unexpectedly returned") from exc

    def top(self, instrument_id: int) -> BookTop | None:
        native_instrument = _command_uint("instrument_id", instrument_id, U32_MAX)
        try:
            record = self._backend.top(native_instrument)
        except (
            _native.NativePersistenceError,
            _native.NativeRecoveryError,
            _native.NativeSnapshotError,
            _native.NativeReadOnlyError,
        ) as exc:
            _raise_mapped_native_error(exc)
        return None if record is None else _decode_book_top(record)

    @overload
    def snapshot(self, instrument_id: None = None) -> EngineSnapshot: ...

    @overload
    def snapshot(self, instrument_id: int) -> InstrumentSnapshot | None: ...

    def snapshot(
        self, instrument_id: int | None = None
    ) -> EngineSnapshot | InstrumentSnapshot | None:
        native_instrument = (
            None
            if instrument_id is None
            else _command_uint("instrument_id", instrument_id, U32_MAX)
        )
        try:
            record = self._backend.snapshot(native_instrument)
        except (
            _native.NativePersistenceError,
            _native.NativeRecoveryError,
            _native.NativeSnapshotError,
            _native.NativeReadOnlyError,
        ) as exc:
            _raise_mapped_native_error(exc)
        if record is None:
            return None
        if instrument_id is None:
            return _decode_engine_snapshot(record)
        return _decode_instrument_snapshot(record)

    def state_digest(self) -> str:
        try:
            digest = self._backend.state_digest()
        except (
            _native.NativePersistenceError,
            _native.NativeRecoveryError,
            _native.NativeSnapshotError,
            _native.NativeReadOnlyError,
        ) as exc:
            _raise_mapped_native_error(exc)
        return _decode_digest(digest, "state_digest")

    def write_snapshot(self, directory: _PathInput) -> SnapshotPublication:
        self._ensure_writable()
        native_directory = _normalize_path("directory", directory)
        try:
            record = self._backend.write_snapshot(native_directory)
        except (
            _native.NativePersistenceError,
            _native.NativeRecoveryError,
            _native.NativeSnapshotError,
            _native.NativeReadOnlyError,
        ) as exc:
            _raise_mapped_native_error(exc)
        return _decode_publication(record)

    def _ensure_writable(self) -> None:
        if self.read_only:
            raise ReadOnlyRecoveryError(_READ_ONLY_MESSAGE)


def _normalize_catalog(catalog: Iterable[InstrumentConfig]) -> list[_NativeRecord]:
    try:
        entries = tuple(catalog)
    except TypeError as exc:
        raise TypeError(
            "catalog must be a finite iterable of exact InstrumentConfig values"
        ) from exc
    if not entries:
        raise ValueError("catalog must contain at least one instrument")

    normalized: list[_NativeRecord] = []
    seen: set[int] = set()
    for index, entry in enumerate(entries):
        if type(entry) is not InstrumentConfig:
            raise TypeError(f"catalog[{index}] must be an exact InstrumentConfig")
        matching = entry.matching
        if type(matching) is not MatchingConfig:
            raise TypeError(f"catalog[{index}].matching must be an exact MatchingConfig")
        instrument_id = _config_uint(
            f"catalog[{index}].instrument_id", entry.instrument_id, U32_MAX, nonzero=True
        )
        if instrument_id in seen:
            raise ValueError("catalog instrument IDs must be unique")
        seen.add(instrument_id)
        normalized.append(
            {
                "instrument_id": instrument_id,
                "max_order_quantity": _config_uint(
                    f"catalog[{index}].matching.max_order_quantity",
                    matching.max_order_quantity,
                    U64_MAX,
                    nonzero=True,
                ),
                "tick_increment": _config_i64(
                    f"catalog[{index}].matching.tick_increment",
                    matching.tick_increment,
                    positive=True,
                ),
                "max_active_orders": _config_uint(
                    f"catalog[{index}].matching.max_active_orders",
                    matching.max_active_orders,
                    U64_MAX,
                ),
            }
        )
    normalized.sort(key=lambda item: cast(int, item["instrument_id"]))
    return normalized


def _normalize_command(command: Command) -> _NativeRecord:
    if type(command) is NewOrder:
        new = command
        return {
            "type": "new",
            "client_id": _command_uint("client_id", new.client_id, U32_MAX),
            "order_id": _command_uint("order_id", new.order_id, U64_MAX),
            "instrument_id": _command_uint("instrument_id", new.instrument_id, U32_MAX),
            "side": _command_uint("side", new.side, U8_MAX),
            "order_type": _command_uint("order_type", new.order_type, U8_MAX),
            "time_in_force": _command_uint("time_in_force", new.time_in_force, U8_MAX),
            "limit_price": (
                None if new.limit_price is None else _command_i64("limit_price", new.limit_price)
            ),
            "quantity": _command_uint("quantity", new.quantity, U64_MAX),
        }
    if type(command) is CancelOrder:
        cancel = command
        return {
            "type": "cancel",
            "client_id": _command_uint("client_id", cancel.client_id, U32_MAX),
            "order_id": _command_uint("order_id", cancel.order_id, U64_MAX),
            "instrument_id": _command_uint("instrument_id", cancel.instrument_id, U32_MAX),
        }
    if type(command) is ReplaceOrder:
        replace = command
        return {
            "type": "replace",
            "client_id": _command_uint("client_id", replace.client_id, U32_MAX),
            "old_order_id": _command_uint("old_order_id", replace.old_order_id, U64_MAX),
            "new_order_id": _command_uint("new_order_id", replace.new_order_id, U64_MAX),
            "instrument_id": _command_uint("instrument_id", replace.instrument_id, U32_MAX),
            "new_limit_price": _command_i64("new_limit_price", replace.new_limit_price),
            "new_quantity": _command_uint("new_quantity", replace.new_quantity, U64_MAX),
        }
    raise TypeError("command must be an exact NewOrder, CancelOrder, or ReplaceOrder instance")


def _command_uint(name: str, value: object, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name} must be an integer, not bool or another scalar type")
    if not 0 <= value <= maximum:
        raise OverflowError(f"{name} is outside the unsigned fixed-width representation")
    return value


def _command_i64(name: str, value: object) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name} must be an integer, not bool or another scalar type")
    if not I64_MIN <= value <= I64_MAX:
        raise OverflowError(f"{name} is outside the signed 64-bit representation")
    return value


def _config_uint(name: str, value: object, maximum: int, *, nonzero: bool = False) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or not 0 <= value <= maximum
        or (nonzero and value == 0)
    ):
        qualifier = "nonzero " if nonzero else ""
        raise ValueError(f"{name} must be a {qualifier}unsigned fixed-width integer")
    return value


def _config_i64(name: str, value: object, *, positive: bool = False) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or not I64_MIN <= value <= I64_MAX
        or (positive and value <= 0)
    ):
        qualifier = "positive " if positive else ""
        raise ValueError(f"{name} must be a {qualifier}signed 64-bit integer")
    return value


def _normalize_path(name: str, value: _PathInput) -> str:
    if isinstance(value, bytes):
        raise TypeError(f"{name} must be str or os.PathLike[str], not bytes")
    try:
        normalized = os.fspath(value)
    except TypeError as exc:
        raise TypeError(f"{name} must be str or os.PathLike[str]") from exc
    if not isinstance(normalized, str):
        raise TypeError(f"{name} must resolve to str, not bytes")
    return normalized


def _literal(name: str, value: object, allowed: frozenset[str]) -> str:
    if type(value) is not str or value not in allowed:
        choices = ", ".join(sorted(allowed))
        raise TypeError(f"{name} must be one of: {choices}")
    return value


def _decode_batch_result(record: object, expected_output: OutputMode) -> BatchResult:
    decoded = _decode_batch_common(record, expected_output, measurement=False)
    return BatchResult(
        submitted_count=decoded.submitted_count,
        processed_count=decoded.processed_count,
        committed_count=decoded.committed_count,
        rejected_count=decoded.rejected_count,
        terminal_error=decoded.terminal_error,
        final_state_digest=_decode_digest(
            decoded.record.get("final_state_digest"), "final_state_digest"
        ),
        payload=decoded.payload,
    )


def _decode_measurement_batch_result(
    record: object, expected_output: OutputMode
) -> _MeasurementBatchResult:
    decoded = _decode_batch_common(record, expected_output, measurement=True)
    if "final_state_digest" in decoded.record:
        raise RuntimeError("native measurement batch unexpectedly produced a state digest")
    return _MeasurementBatchResult(
        submitted_count=decoded.submitted_count,
        processed_count=decoded.processed_count,
        committed_count=decoded.committed_count,
        rejected_count=decoded.rejected_count,
        terminal_error=decoded.terminal_error,
        payload=decoded.payload,
    )


def _decode_batch_common(
    record: object,
    expected_output: OutputMode,
    *,
    measurement: bool,
) -> _DecodedBatch:
    value = _record(record, "measurement batch result" if measurement else "batch result")
    actual_output = _string(value, "output")
    if actual_output != expected_output:
        raise RuntimeError(
            f"native batch output mismatch: requested {expected_output}, got {actual_output}"
        )
    terminal_raw = value.get("terminal_error")
    terminal = None if terminal_raw is None else _enum(EngineError, terminal_raw, "terminal_error")
    payload_raw = value.get("payload")
    if expected_output == "objects":
        payload_items = _sequence(payload_raw, "payload")
        payload: BatchPayload = ObjectBatch(
            tuple(_decode_engine_result(item) for item in payload_items)
        )
    elif expected_output == "columns":
        payload = _decode_column_batch(payload_raw)
    else:
        if payload_raw is not None:
            raise RuntimeError("native summary batch unexpectedly materialized a payload")
        payload = SummaryBatch()
    submitted = _uint(value.get("submitted_count"), "submitted_count", U64_MAX)
    processed = _uint(value.get("processed_count"), "processed_count", U64_MAX)
    committed = _uint(value.get("committed_count"), "committed_count", U64_MAX)
    rejected = _uint(value.get("rejected_count"), "rejected_count", U64_MAX)
    if processed > submitted:
        raise RuntimeError("native batch processed count exceeds submitted count")
    if committed + rejected > processed:
        raise RuntimeError("native batch outcome counts exceed processed count")
    if isinstance(payload, ObjectBatch) and len(payload.results) != processed:
        raise RuntimeError("native object payload length differs from processed count")
    return _DecodedBatch(
        record=value,
        submitted_count=submitted,
        processed_count=processed,
        committed_count=committed,
        rejected_count=rejected,
        terminal_error=terminal,
        payload=payload,
    )


def _decode_engine_result(record: object) -> EngineResult:
    value = _record(record, "engine result")
    error_raw = value.get("error")
    events_raw = _sequence(value.get("events"), "events")
    if error_raw is not None:
        if events_raw:
            raise RuntimeError("native engine-error result also contains events")
        return EngineResult(error=_enum(EngineError, error_raw, "error"))
    events = tuple(_decode_event(item) for item in events_raw)
    result = EngineResult(events=events)
    sequence = value.get("command_sequence")
    instrument = value.get("instrument_id")
    if (
        sequence is not None
        and _uint(sequence, "command_sequence", U64_MAX) != result.command_sequence
    ):
        raise RuntimeError("native result command sequence disagrees with events")
    if (
        instrument is not None
        and _uint(instrument, "instrument_id", U32_MAX) != result.instrument_id
    ):
        raise RuntimeError("native result instrument ID disagrees with events")
    return result


def _decode_event(record: object) -> Event:
    value = _record(record, "event")
    kind = _enum(EventType, value.get("type"), "event.type")
    header = EventHeader(
        _uint(value.get("command_sequence"), "event.command_sequence", U64_MAX),
        _uint(value.get("event_index"), "event.event_index", U32_MAX),
        _uint(value.get("instrument_id"), "event.instrument_id", U32_MAX),
    )
    if kind is EventType.ACCEPTED:
        return AcceptedEvent(header, _enum(CommandType, value.get("command_type"), "command_type"))
    if kind is EventType.REJECTED:
        order_id = value.get("order_id")
        return RejectedEvent(
            header,
            _enum(CommandType, value.get("command_type"), "command_type"),
            _enum(RejectReason, value.get("reason"), "reason"),
            None if order_id is None else _uint(order_id, "order_id", U64_MAX),
        )
    if kind is EventType.TRADE:
        return TradeEvent(
            header,
            _uint(value.get("aggressor_order_id"), "aggressor_order_id", U64_MAX),
            _uint(value.get("resting_order_id"), "resting_order_id", U64_MAX),
            _uint(value.get("aggressor_client_id"), "aggressor_client_id", U32_MAX),
            _uint(value.get("resting_client_id"), "resting_client_id", U32_MAX),
            _enum(Side, value.get("aggressor_side"), "aggressor_side"),
            _int64(value.get("execution_price"), "execution_price"),
            _uint(value.get("execution_quantity"), "execution_quantity", U64_MAX),
            _uint(value.get("aggressor_remaining"), "aggressor_remaining", U64_MAX),
            _uint(value.get("resting_remaining"), "resting_remaining", U64_MAX),
        )
    if kind is EventType.RESTED:
        return RestedEvent(
            header,
            _uint(value.get("order_id"), "order_id", U64_MAX),
            _uint(value.get("client_id"), "client_id", U32_MAX),
            _enum(Side, value.get("side"), "side"),
            _int64(value.get("price"), "price"),
            _uint(value.get("remaining_quantity"), "remaining_quantity", U64_MAX),
        )
    if kind is EventType.CANCELED:
        return CanceledEvent(
            header,
            _uint(value.get("order_id"), "order_id", U64_MAX),
            _uint(value.get("canceled_quantity"), "canceled_quantity", U64_MAX),
        )
    if kind is EventType.REPLACED:
        return ReplacedEvent(
            header,
            _uint(value.get("old_order_id"), "old_order_id", U64_MAX),
            _uint(value.get("new_order_id"), "new_order_id", U64_MAX),
        )
    if kind is EventType.DONE:
        return DoneEvent(
            header,
            _uint(value.get("order_id"), "order_id", U64_MAX),
            _enum(DoneReason, value.get("reason"), "reason"),
            _uint(value.get("remaining_quantity"), "remaining_quantity", U64_MAX),
        )
    return BookChangedEvent(
        header,
        _decode_optional_top_level(value.get("best_bid"), "best_bid"),
        _decode_optional_top_level(value.get("best_ask"), "best_ask"),
    )


def _decode_column_batch(value: object) -> ColumnBatch:
    record = _record(value, "column payload")
    columns: dict[str, array[int]] = {}
    for name, raw_values in record.items():
        typecode = _COLUMN_TYPECODES.get(name)
        if typecode is None:
            raise RuntimeError(f"native binding returned an unknown column: {name}")
        items = _sequence(raw_values, f"column {name}")
        converted = array(typecode)
        try:
            converted.extend(_column_integer(item, name) for item in items)
        except (OverflowError, TypeError) as exc:
            raise RuntimeError(f"native column {name} contains an invalid value") from exc
        columns[name] = converted
    missing = _REQUIRED_COLUMNS - columns.keys()
    if missing:
        raise RuntimeError(f"native column payload is missing columns: {sorted(missing)}")
    return ColumnBatch(columns)


def _column_integer(value: object, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise RuntimeError(f"native column {name} contains a non-integer")
    return value


def _decode_book_top(value: object) -> BookTop:
    record = _record(value, "book top")
    return BookTop(
        _decode_optional_top_level(record.get("best_bid"), "best_bid"),
        _decode_optional_top_level(record.get("best_ask"), "best_ask"),
    )


def _decode_optional_top_level(value: object, name: str) -> TopOfBookLevel | None:
    if value is None:
        return None
    record = _record(value, name)
    return TopOfBookLevel(
        _int64(record.get("price"), f"{name}.price"),
        _uint(
            record.get("aggregate_quantity"),
            f"{name}.aggregate_quantity",
            U64_MAX,
        ),
    )


def _decode_engine_snapshot(value: object) -> EngineSnapshot:
    record = _record(value, "engine snapshot")
    config_record = _record(record.get("engine_config"), "engine_config")
    return EngineSnapshot(
        semantics_version=_uint(record.get("semantics_version"), "semantics_version", U16_MAX),
        engine_config=MultiInstrumentEngineConfig(
            _uint(
                config_record.get("max_total_active_orders"),
                "max_total_active_orders",
                U64_MAX,
            )
        ),
        catalog=tuple(
            _decode_instrument_config(item) for item in _sequence(record.get("catalog"), "catalog")
        ),
        last_sequence=_uint(record.get("last_sequence"), "last_sequence", U64_MAX),
        sequence_exhausted=_boolean(record.get("sequence_exhausted"), "sequence_exhausted"),
        active_order_count=_uint(record.get("active_order_count"), "active_order_count", U64_MAX),
        instruments=tuple(
            _decode_instrument_snapshot(item)
            for item in _sequence(record.get("instruments"), "instruments")
        ),
    )


def _decode_instrument_config(value: object) -> InstrumentConfig:
    record = _record(value, "instrument config")
    matching_raw = record.get("matching")
    matching = record if matching_raw is None else _record(matching_raw, "matching")
    return InstrumentConfig(
        _uint(record.get("instrument_id"), "instrument_id", U32_MAX),
        MatchingConfig(
            _uint(
                matching.get("max_order_quantity"),
                "max_order_quantity",
                U64_MAX,
            ),
            _int64(matching.get("tick_increment"), "tick_increment"),
            _uint(matching.get("max_active_orders"), "max_active_orders", U64_MAX),
        ),
    )


def _decode_instrument_snapshot(value: object) -> InstrumentSnapshot:
    record = _record(value, "instrument snapshot")
    return InstrumentSnapshot(
        instrument_id=_uint(record.get("instrument_id"), "instrument_id", U32_MAX),
        active_order_count=_uint(record.get("active_order_count"), "active_order_count", U64_MAX),
        bids=tuple(_decode_price_level(item) for item in _sequence(record.get("bids"), "bids")),
        asks=tuple(_decode_price_level(item) for item in _sequence(record.get("asks"), "asks")),
    )


def _decode_price_level(value: object) -> PriceLevelSnapshot:
    record = _record(value, "price level")
    return PriceLevelSnapshot(
        price=_int64(record.get("price"), "price"),
        aggregate_quantity=_uint(record.get("aggregate_quantity"), "aggregate_quantity", U64_MAX),
        orders=tuple(
            _decode_order_snapshot(item) for item in _sequence(record.get("orders"), "orders")
        ),
    )


def _decode_order_snapshot(value: object) -> OrderSnapshot:
    record = _record(value, "order snapshot")
    return OrderSnapshot(
        order_id=_uint(record.get("order_id"), "order_id", U64_MAX),
        client_id=_uint(record.get("client_id"), "client_id", U32_MAX),
        instrument_id=_uint(record.get("instrument_id"), "instrument_id", U32_MAX),
        side=_enum(Side, record.get("side"), "side"),
        price=_int64(record.get("price"), "price"),
        remaining_quantity=_uint(record.get("remaining_quantity"), "remaining_quantity", U64_MAX),
        priority_sequence=_uint(record.get("priority_sequence"), "priority_sequence", U64_MAX),
    )


def _decode_optional_recovery_report(
    value: object,
) -> RecoveryReport | ReplayReport | None:
    if value is None:
        return None
    record = _record(value, "recovery report")
    if "replay" in record:
        return _decode_recovery_report(record)
    return _decode_replay_report(record)


def _decode_recovery_report(value: object) -> RecoveryReport:
    record = _record(value, "snapshot recovery report")
    selected = record.get("selected_snapshot")
    skipped: list[SkippedSnapshot] = []
    for item in _sequence(record.get("skipped_snapshots", ()), "skipped_snapshots"):
        skipped_record = _record(item, "skipped snapshot")
        error = _decode_optional_error_details(skipped_record.get("error"))
        if error is None:
            raise RuntimeError("skipped snapshot is missing its error")
        filename_sequence = skipped_record.get("filename_sequence")
        skipped.append(
            SkippedSnapshot(
                _decode_path(skipped_record.get("path"), "path"),
                None
                if filename_sequence is None
                else _uint(filename_sequence, "filename_sequence", U64_MAX),
                error,
            )
        )
    return RecoveryReport(
        recovery_source=_string(record, "recovery_source"),
        selected_snapshot=(
            None if selected is None else _decode_path(selected, "selected_snapshot")
        ),
        covered_sequence=_optional_uint(record.get("covered_sequence"), "covered_sequence"),
        covered_log_byte_offset=_optional_uint(
            record.get("covered_log_byte_offset"), "covered_log_byte_offset"
        ),
        snapshot_state_digest=_optional_digest(
            record.get("snapshot_state_digest"), "snapshot_state_digest"
        ),
        skipped_snapshots=tuple(skipped),
        snapshot_error=_decode_optional_error_details(record.get("snapshot_error")),
        replay=_decode_replay_report(record.get("replay")),
    )


def _decode_replay_report(value: object) -> ReplayReport:
    record = _record(value, "replay report")
    mode = _string(record, "mode")
    tail_policy = _string(record, "tail_policy")
    _literal("mode", mode, _REPLAY_MODES)
    _literal("tail_policy", tail_policy, _TAIL_POLICIES)
    return ReplayReport(
        mode=cast(ReplayMode, mode),
        tail_policy=cast(TailPolicy, tail_policy),
        tail=_string(record, "tail"),
        header=_decode_optional_log_header(record.get("header")),
        last_sequence=_optional_uint(record.get("last_sequence"), "last_sequence"),
        valid_end_offset=_uint(record.get("valid_end_offset"), "valid_end_offset", U64_MAX),
        records_scanned=_uint(record.get("records_scanned"), "records_scanned", U64_MAX),
        records_replayed=_uint(record.get("records_replayed"), "records_replayed", U64_MAX),
        committed_count=_uint(record.get("committed"), "committed", U64_MAX),
        rejected_count=_uint(record.get("rejected"), "rejected", U64_MAX),
        used_valid_prefix=_boolean(record.get("used_valid_prefix"), "used_valid_prefix"),
        final_state_digest=_optional_digest(record.get("final_state_digest"), "final_state_digest"),
        warning=_decode_optional_error_details(record.get("warning")),
        error=_decode_optional_error_details(record.get("error")),
        divergence=_decode_optional_replay_divergence(record.get("divergence")),
    )


def _decode_optional_log_header(value: object) -> LogHeader | None:
    if value is None:
        return None
    record = _record(value, "log header")
    config_record = _record(record.get("engine_config"), "log header engine_config")
    return LogHeader(
        format_version=_uint(record.get("format_version"), "header.format_version", U16_MAX),
        semantics_version=_uint(
            record.get("semantics_version"), "header.semantics_version", U16_MAX
        ),
        log_id=_decode_log_id(record.get("log_id"), "header.log_id"),
        first_sequence=_uint(record.get("first_sequence"), "header.first_sequence", U64_MAX),
        engine_config=MultiInstrumentEngineConfig(
            _uint(
                config_record.get("max_total_active_orders"),
                "header.max_total_active_orders",
                U64_MAX,
            )
        ),
        catalog=tuple(
            _decode_instrument_config(item)
            for item in _sequence(record.get("catalog"), "header.catalog")
        ),
    )


def _decode_replay_evidence(value: object, name: str) -> ReplayEvidence:
    record = _record(value, name)
    outcome_raw = _optional_string(record.get("outcome"), f"{name}.outcome")
    if outcome_raw is not None:
        _literal(f"{name}.outcome", outcome_raw, _REPLAY_OUTCOMES)
    rejection_raw = record.get("rejection_reason")
    return ReplayEvidence(
        outcome=cast(Literal["committed", "rejected"] | None, outcome_raw),
        rejection_reason=(
            None
            if rejection_raw is None
            else _enum(RejectReason, rejection_raw, f"{name}.rejection_reason")
        ),
        event_count=_optional_uint(record.get("event_count"), f"{name}.event_count"),
        event_digest=_optional_digest(record.get("event_digest"), f"{name}.event_digest"),
    )


def _decode_optional_replay_divergence(value: object) -> ReplayDivergence | None:
    if value is None:
        return None
    record = _record(value, "replay divergence")
    engine_error_raw = record.get("actual_engine_error")
    return ReplayDivergence(
        record_offset=_uint(record.get("record_offset"), "divergence.record_offset", U64_MAX),
        sequence=_uint(record.get("sequence"), "divergence.sequence", U64_MAX),
        category=_string(record, "category"),
        command=_decode_optional_command(record.get("command")),
        expected=_decode_replay_evidence(record.get("expected"), "divergence.expected"),
        actual=_decode_replay_evidence(record.get("actual"), "divergence.actual"),
        actual_engine_error=(
            None
            if engine_error_raw is None
            else _enum(EngineError, engine_error_raw, "divergence.actual_engine_error")
        ),
        actual_events=tuple(
            _decode_event(item)
            for item in _sequence(record.get("actual_events"), "divergence.actual_events")
        ),
    )


def _decode_optional_command(value: object) -> Command | None:
    if value is None:
        return None
    record = _record(value, "replay command")
    command_type = _string(record, "type")
    if command_type == "new":
        price_raw = record.get("limit_price")
        return NewOrder(
            client_id=_uint(record.get("client_id"), "command.client_id", U32_MAX),
            order_id=_uint(record.get("order_id"), "command.order_id", U64_MAX),
            instrument_id=_uint(record.get("instrument_id"), "command.instrument_id", U32_MAX),
            side=_uint(record.get("side"), "command.side", U8_MAX),
            order_type=_uint(record.get("order_type"), "command.order_type", U8_MAX),
            time_in_force=_uint(record.get("time_in_force"), "command.time_in_force", U8_MAX),
            limit_price=(None if price_raw is None else _int64(price_raw, "command.limit_price")),
            quantity=_uint(record.get("quantity"), "command.quantity", U64_MAX),
        )
    if command_type == "cancel":
        return CancelOrder(
            client_id=_uint(record.get("client_id"), "command.client_id", U32_MAX),
            order_id=_uint(record.get("order_id"), "command.order_id", U64_MAX),
            instrument_id=_uint(record.get("instrument_id"), "command.instrument_id", U32_MAX),
        )
    if command_type == "replace":
        return ReplaceOrder(
            client_id=_uint(record.get("client_id"), "command.client_id", U32_MAX),
            old_order_id=_uint(record.get("old_order_id"), "command.old_order_id", U64_MAX),
            new_order_id=_uint(record.get("new_order_id"), "command.new_order_id", U64_MAX),
            instrument_id=_uint(record.get("instrument_id"), "command.instrument_id", U32_MAX),
            new_limit_price=_int64(record.get("new_limit_price"), "command.new_limit_price"),
            new_quantity=_uint(record.get("new_quantity"), "command.new_quantity", U64_MAX),
        )
    raise RuntimeError(f"native binding replay command has unknown type: {command_type!r}")


def _decode_publication(value: object) -> SnapshotPublication:
    record = _record(value, "snapshot publication")
    return SnapshotPublication(
        path=_decode_path(record.get("path"), "path"),
        covered_sequence=_uint(record.get("covered_sequence"), "covered_sequence", U64_MAX),
        covered_log_byte_offset=_uint(
            record.get("covered_log_byte_offset"),
            "covered_log_byte_offset",
            U64_MAX,
        ),
        encoded_bytes=_uint(record.get("encoded_bytes"), "encoded_bytes", U64_MAX),
    )


def _decode_publication_report(value: object) -> SnapshotPublicationReport:
    record = _record(value, "snapshot publication report")
    path = record.get("path")
    return SnapshotPublicationReport(
        path=None if path is None else _decode_path(path, "path"),
        covered_sequence=_uint(record.get("covered_sequence"), "covered_sequence", U64_MAX),
        covered_log_byte_offset=_uint(
            record.get("covered_log_byte_offset"),
            "covered_log_byte_offset",
            U64_MAX,
        ),
        encoded_bytes=_uint(record.get("encoded_bytes"), "encoded_bytes", U64_MAX),
        final_file_visible=_boolean(record.get("final_file_visible"), "final_file_visible"),
        error=_decode_optional_error_details(record.get("error")),
    )


def _raise_mapped_native_error(
    exc: Exception,
    *,
    output: OutputMode = "objects",
) -> None:
    message, record = _native_exception(exc)
    details = _decode_error_details(record)
    if isinstance(exc, _native.NativePersistenceError):
        prefix_raw = record.get("prefix_batch")
        prefix = None if prefix_raw is None else _decode_batch_result(prefix_raw, output)
        report_raw = record.get("recovery_report")
        report = None if report_raw is None else _decode_optional_recovery_report(report_raw)
        raise PersistenceError(
            message,
            details=details,
            session_poisoned=_optional_boolean(
                record.get("session_poisoned"), "session_poisoned", default=False
            ),
            prefix_result=prefix,
            report=report,
        ) from exc
    if isinstance(exc, _native.NativeRecoveryError):
        report_raw = record.get("recovery_report")
        report = None if report_raw is None else _decode_optional_recovery_report(report_raw)
        raise RecoveryError(message, details=details, report=report) from exc
    if isinstance(exc, _native.NativeSnapshotError):
        publication_raw = record.get("publication_report")
        publication = (
            None if publication_raw is None else _decode_publication_report(publication_raw)
        )
        raise SnapshotError(message, details=details, publication=publication) from exc
    raise ReadOnlyRecoveryError(message or _READ_ONLY_MESSAGE, details=details) from exc


def _native_exception(exc: Exception) -> tuple[str, Mapping[str, object]]:
    arguments = cast(tuple[object, ...], exc.args)
    message = str(arguments[0]) if arguments else str(exc)
    record = _record(arguments[1], "native exception details") if len(arguments) >= 2 else {}
    return message, record


def _decode_optional_error_details(value: object) -> OperationErrorDetails | None:
    if value is None:
        return None
    record = _record(value, "error details")
    category = record.get("category")
    if category is None or category == "none":
        return None
    return _decode_error_details(record)


def _decode_error_details(value: object) -> OperationErrorDetails:
    record = _record(value, "error details")
    return OperationErrorDetails(
        category=_string(record, "category"),
        byte_offset=_optional_uint(record.get("byte_offset"), "byte_offset") or 0,
        system_error_value=_optional_int(record.get("system_error_value"), "system_error_value")
        or 0,
        system_error_message=_optional_string(
            record.get("system_error_message"), "system_error_message"
        )
        or "",
    )


def _record(value: object, name: str) -> Mapping[str, object]:
    if not isinstance(value, dict) or not all(isinstance(key, str) for key in value):
        raise RuntimeError(f"native binding {name} is not a string-keyed dictionary")
    return cast(Mapping[str, object], value)


def _sequence(value: object, name: str) -> tuple[object, ...]:
    if not isinstance(value, (list, tuple)):
        raise RuntimeError(f"native binding {name} is not an owned sequence")
    return tuple(cast(Iterable[object], value))


def _string(record: Mapping[str, object], name: str) -> str:
    return _plain_string(record.get(name), name)


def _plain_string(value: object, name: str) -> str:
    if type(value) is not str:
        raise RuntimeError(f"native binding {name} is not a string")
    return value


def _optional_string(value: object, name: str) -> str | None:
    return None if value is None else _plain_string(value, name)


def _decode_path(value: object, name: str) -> Path:
    if isinstance(value, bytes):
        raise RuntimeError(f"native binding {name} returned a bytes path")
    try:
        normalized = os.fspath(cast(str | os.PathLike[str], value))
    except TypeError as exc:
        raise RuntimeError(f"native binding {name} is not a Unicode path") from exc
    if not isinstance(normalized, str):
        raise RuntimeError(f"native binding {name} returned a bytes path")
    return Path(normalized)


def _boolean(value: object, name: str) -> bool:
    if type(value) is not bool:
        raise RuntimeError(f"native binding {name} is not a boolean")
    return value


def _optional_boolean(value: object, name: str, *, default: bool) -> bool:
    return default if value is None else _boolean(value, name)


def _uint(value: object, name: str, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
        raise RuntimeError(f"native binding {name} is outside its unsigned representation")
    return value


def _optional_uint(value: object, name: str) -> int | None:
    return None if value is None else _uint(value, name, U64_MAX)


def _int64(value: object, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not I64_MIN <= value <= I64_MAX:
        raise RuntimeError(f"native binding {name} is outside its signed 64-bit representation")
    return value


def _optional_int(value: object, name: str) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise RuntimeError(f"native binding {name} is not an integer")
    return value


_EnumT = TypeVar("_EnumT", bound=IntEnum)


def _enum(enum_type: type[_EnumT], value: object, name: str) -> _EnumT:
    raw = _uint(value, name, U16_MAX)
    try:
        return enum_type(raw)
    except ValueError as exc:
        raise RuntimeError(f"native binding {name} has an unknown enum code: {raw}") from exc


def _decode_digest(value: object, name: str) -> str:
    digest = _plain_string(value, name)
    if _DIGEST.fullmatch(digest) is None:
        raise RuntimeError(f"native binding {name} is not a lowercase 256-bit digest")
    return digest


def _decode_log_id(value: object, name: str) -> str:
    log_id = _plain_string(value, name)
    if _LOG_ID.fullmatch(log_id) is None:
        raise RuntimeError(f"native binding {name} is not a lowercase 128-bit log ID")
    return log_id


def _optional_digest(value: object, name: str) -> str | None:
    return None if value is None else _decode_digest(value, name)


_COLUMN_TYPECODES: dict[str, str] = {
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
_REQUIRED_COLUMNS = frozenset(_COLUMN_TYPECODES)


def _verify_array_item_sizes() -> None:
    expected = {"B": 1, "H": 2, "I": 4, "Q": 8, "q": 8}
    actual = {typecode: array(typecode).itemsize for typecode in expected}
    if actual != expected:
        raise ImportError(
            "AtlasLOB column arrays require CPython fixed-width item sizes "
            f"{expected}, got {actual}"
        )


_verify_array_item_sizes()


__all__ = [
    "BatchResult",
    "ColumnBatch",
    "Durability",
    "Engine",
    "EngineResult",
    "LogHeader",
    "ObjectBatch",
    "OperationErrorDetails",
    "OutputMode",
    "PersistenceError",
    "ReadOnlyRecoveryError",
    "RecoveryError",
    "RecoveryReport",
    "ReplayDivergence",
    "ReplayEvidence",
    "ReplayMode",
    "ReplayReport",
    "SkippedSnapshot",
    "SnapshotError",
    "SnapshotPublication",
    "SnapshotPublicationReport",
    "SummaryBatch",
    "TailPolicy",
]
