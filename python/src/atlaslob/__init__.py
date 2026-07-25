"""Independent correctness evidence for AtlasLOB."""

from __future__ import annotations

import importlib
from typing import TYPE_CHECKING

from atlaslob.canonical import (
    engine_state_bytes,
    engine_state_digest,
    event_digest,
    state_digest,
)
from atlaslob.domain import (
    ATLASLOB_SEMANTICS_VERSION,
    AcceptedEvent,
    BookChangedEvent,
    BookSnapshot,
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
    EventBatch,
    EventHeader,
    EventType,
    InstrumentConfig,
    InstrumentSnapshot,
    MatchingConfig,
    MultiInstrumentEngineConfig,
    NewOrder,
    OrderSnapshot,
    OrderType,
    PriceLevelSnapshot,
    ReferenceResult,
    RejectedEvent,
    RejectReason,
    ReplacedEvent,
    ReplaceOrder,
    RestedEvent,
    Side,
    TimeInForce,
    TopOfBookLevel,
    TradeEvent,
)
from atlaslob.native import (
    NativeInputConfig,
    NativeProtocolError,
    NativeRun,
    NativeTranscript,
    decode_jsonl,
    encode_stream,
    run_native,
)
from atlaslob.reference import ReferenceEngine
from atlaslob.router import ReferenceRouter

if TYPE_CHECKING:
    from atlaslob.engine import (
        BatchResult,
        ColumnBatch,
        Engine,
        EngineResult,
        LogHeader,
        ObjectBatch,
        OperationErrorDetails,
        PersistenceError,
        ReadOnlyRecoveryError,
        RecoveryError,
        RecoveryReport,
        ReplayDivergence,
        ReplayEvidence,
        ReplayReport,
        SkippedSnapshot,
        SnapshotError,
        SnapshotPublication,
        SnapshotPublicationReport,
        SummaryBatch,
    )

_ENGINE_EXPORTS = frozenset(
    {
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
    }
)


def __getattr__(name: str) -> object:
    if name in _ENGINE_EXPORTS:
        module = importlib.import_module("atlaslob.engine")
        value = getattr(module, name)
        globals()[name] = value
        return value
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


def __dir__() -> list[str]:
    return sorted(set(globals()) | _ENGINE_EXPORTS)


__all__ = [
    "ATLASLOB_SEMANTICS_VERSION",
    "AcceptedEvent",
    "BatchResult",
    "BookChangedEvent",
    "BookSnapshot",
    "BookTop",
    "ColumnBatch",
    "CancelOrder",
    "CanceledEvent",
    "Command",
    "CommandType",
    "DoneEvent",
    "DoneReason",
    "EngineSnapshot",
    "Engine",
    "EngineError",
    "EngineResult",
    "Event",
    "EventBatch",
    "EventHeader",
    "EventType",
    "InstrumentConfig",
    "InstrumentSnapshot",
    "LogHeader",
    "MatchingConfig",
    "MultiInstrumentEngineConfig",
    "NewOrder",
    "NativeInputConfig",
    "NativeProtocolError",
    "NativeRun",
    "NativeTranscript",
    "OrderSnapshot",
    "ObjectBatch",
    "OperationErrorDetails",
    "OrderType",
    "PriceLevelSnapshot",
    "PersistenceError",
    "ReadOnlyRecoveryError",
    "ReferenceResult",
    "ReferenceEngine",
    "ReferenceRouter",
    "RecoveryError",
    "RecoveryReport",
    "ReplayDivergence",
    "ReplayEvidence",
    "RejectReason",
    "RejectedEvent",
    "ReplaceOrder",
    "ReplayReport",
    "ReplacedEvent",
    "RestedEvent",
    "Side",
    "SkippedSnapshot",
    "SnapshotError",
    "SnapshotPublication",
    "SnapshotPublicationReport",
    "SummaryBatch",
    "TimeInForce",
    "TopOfBookLevel",
    "TradeEvent",
    "decode_jsonl",
    "engine_state_bytes",
    "engine_state_digest",
    "encode_stream",
    "event_digest",
    "run_native",
    "state_digest",
]
