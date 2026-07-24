"""Value-only domain model used by the independent Python oracle."""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum
from typing import TypeAlias

ATLASLOB_SEMANTICS_VERSION = 6
U8_MAX = (1 << 8) - 1
U16_MAX = (1 << 16) - 1
U32_MAX = (1 << 32) - 1
U64_MAX = (1 << 64) - 1
I64_MIN = -(1 << 63)
I64_MAX = (1 << 63) - 1


class Side(IntEnum):
    BUY = 1
    SELL = 2


class OrderType(IntEnum):
    LIMIT = 1
    MARKET = 2


class TimeInForce(IntEnum):
    GTC = 1
    IOC = 2
    FOK = 3


class CommandType(IntEnum):
    NEW = 1
    CANCEL = 2
    REPLACE = 3


class EventType(IntEnum):
    ACCEPTED = 1
    REJECTED = 2
    TRADE = 3
    RESTED = 4
    CANCELED = 5
    REPLACED = 6
    DONE = 7
    BOOK_CHANGED = 8


class DoneReason(IntEnum):
    FILLED = 1
    IOC_RESIDUAL_CANCELED = 2
    MARKET_EXHAUSTED = 3
    CANCELED = 4
    REPLACED = 5
    FOK_UNAVAILABLE = 6


class RejectReason(IntEnum):
    NONE = 0
    INVALID_ORDER_ID = 1
    INVALID_INSTRUMENT_ID = 2
    INVALID_QUANTITY = 3
    INVALID_SIDE = 4
    INVALID_ORDER_TYPE = 5
    INVALID_TIME_IN_FORCE = 6
    MISSING_LIMIT_PRICE = 7
    UNEXPECTED_LIMIT_PRICE = 8
    INVALID_PRICE = 9
    INVALID_ORDER_TYPE_TIME_IN_FORCE = 10
    UNSUPPORTED_TIME_IN_FORCE = 11
    INVALID_CLIENT_ID = 12
    UNKNOWN_INSTRUMENT = 13
    QUANTITY_OUT_OF_RANGE = 14
    INVALID_TICK = 15
    DUPLICATE_ORDER_ID = 16
    UNKNOWN_ORDER_ID = 17
    INVALID_REPLACEMENT_ID = 18
    OWNERSHIP_MISMATCH = 19
    INSTRUMENT_MISMATCH = 20
    CAPACITY_EXCEEDED = 21


class EngineError(IntEnum):
    SEQUENCE_EXHAUSTED = 1
    INTERNAL_FAILURE = 2


@dataclass(frozen=True, slots=True)
class MatchingConfig:
    max_order_quantity: int = U64_MAX
    tick_increment: int = 1
    max_active_orders: int = U64_MAX

    def __post_init__(self) -> None:
        _require_u64("max_order_quantity", self.max_order_quantity, nonzero=True)
        _require_i64("tick_increment", self.tick_increment)
        if self.tick_increment <= 0:
            raise ValueError("tick_increment must be positive")
        _require_u64("max_active_orders", self.max_active_orders)


@dataclass(frozen=True, slots=True)
class NewOrder:
    client_id: int
    order_id: int
    instrument_id: int
    side: int
    order_type: int
    time_in_force: int
    limit_price: int | None
    quantity: int


@dataclass(frozen=True, slots=True)
class CancelOrder:
    client_id: int
    order_id: int
    instrument_id: int


@dataclass(frozen=True, slots=True)
class ReplaceOrder:
    client_id: int
    old_order_id: int
    new_order_id: int
    instrument_id: int
    new_limit_price: int
    new_quantity: int


Command: TypeAlias = NewOrder | CancelOrder | ReplaceOrder


def command_type(command: Command) -> CommandType:
    if isinstance(command, NewOrder):
        return CommandType.NEW
    if isinstance(command, CancelOrder):
        return CommandType.CANCEL
    return CommandType.REPLACE


@dataclass(frozen=True, slots=True)
class EventHeader:
    command_sequence: int
    event_index: int
    instrument_id: int


@dataclass(frozen=True, slots=True)
class AcceptedEvent:
    header: EventHeader
    command_type: CommandType


@dataclass(frozen=True, slots=True)
class RejectedEvent:
    header: EventHeader
    command_type: CommandType
    reason: RejectReason
    order_id: int | None


@dataclass(frozen=True, slots=True)
class TradeEvent:
    header: EventHeader
    aggressor_order_id: int
    resting_order_id: int
    aggressor_client_id: int
    resting_client_id: int
    aggressor_side: Side
    execution_price: int
    execution_quantity: int
    aggressor_remaining: int
    resting_remaining: int


@dataclass(frozen=True, slots=True)
class RestedEvent:
    header: EventHeader
    order_id: int
    client_id: int
    side: Side
    price: int
    remaining_quantity: int


@dataclass(frozen=True, slots=True)
class CanceledEvent:
    header: EventHeader
    order_id: int
    canceled_quantity: int


@dataclass(frozen=True, slots=True)
class ReplacedEvent:
    header: EventHeader
    old_order_id: int
    new_order_id: int


@dataclass(frozen=True, slots=True)
class DoneEvent:
    header: EventHeader
    order_id: int
    reason: DoneReason
    remaining_quantity: int


@dataclass(frozen=True, slots=True)
class TopOfBookLevel:
    price: int
    aggregate_quantity: int


@dataclass(frozen=True, slots=True)
class BookChangedEvent:
    header: EventHeader
    best_bid: TopOfBookLevel | None
    best_ask: TopOfBookLevel | None


Event: TypeAlias = (
    AcceptedEvent
    | RejectedEvent
    | TradeEvent
    | RestedEvent
    | CanceledEvent
    | ReplacedEvent
    | DoneEvent
    | BookChangedEvent
)


def event_type(event: Event) -> EventType:
    if isinstance(event, AcceptedEvent):
        return EventType.ACCEPTED
    if isinstance(event, RejectedEvent):
        return EventType.REJECTED
    if isinstance(event, TradeEvent):
        return EventType.TRADE
    if isinstance(event, RestedEvent):
        return EventType.RESTED
    if isinstance(event, CanceledEvent):
        return EventType.CANCELED
    if isinstance(event, ReplacedEvent):
        return EventType.REPLACED
    if isinstance(event, DoneEvent):
        return EventType.DONE
    return EventType.BOOK_CHANGED


@dataclass(frozen=True, slots=True)
class EventBatch:
    events: tuple[Event, ...]

    def __post_init__(self) -> None:
        if not self.events:
            raise ValueError("event batch must contain at least one event")
        first = self.events[0].header
        if first.command_sequence == 0:
            raise ValueError("event batch command sequence must be nonzero")
        for index, event in enumerate(self.events):
            header = event.header
            if header.command_sequence != first.command_sequence:
                raise ValueError("event batch command sequences must match")
            if header.instrument_id != first.instrument_id:
                raise ValueError("event batch instrument IDs must match")
            if header.event_index != index:
                raise ValueError("event batch indices must be contiguous from zero")
        if first.instrument_id == 0 and (
            len(self.events) != 1 or not isinstance(self.events[0], RejectedEvent)
        ):
            raise ValueError("zero-instrument batch must contain exactly one rejection")

    @property
    def command_sequence(self) -> int:
        return self.events[0].header.command_sequence

    @property
    def instrument_id(self) -> int:
        return self.events[0].header.instrument_id

    @property
    def committed(self) -> bool:
        return isinstance(self.events[0], AcceptedEvent)

    @property
    def rejected(self) -> bool:
        return isinstance(self.events[0], RejectedEvent)


@dataclass(frozen=True, slots=True)
class ReferenceResult:
    batch: EventBatch | None = None
    error: EngineError | None = None

    def __post_init__(self) -> None:
        if (self.batch is None) == (self.error is None):
            raise ValueError("result must contain exactly one batch or engine error")

    @property
    def committed(self) -> bool:
        return self.batch is not None and self.batch.committed

    @property
    def rejected(self) -> bool:
        return self.batch is not None and self.batch.rejected


@dataclass(frozen=True, slots=True)
class OrderSnapshot:
    order_id: int
    client_id: int
    instrument_id: int
    side: Side
    price: int
    remaining_quantity: int
    priority_sequence: int


@dataclass(frozen=True, slots=True)
class PriceLevelSnapshot:
    price: int
    aggregate_quantity: int
    orders: tuple[OrderSnapshot, ...]


@dataclass(frozen=True, slots=True)
class BookSnapshot:
    semantics_version: int
    instrument_id: int
    last_sequence: int
    sequence_exhausted: bool
    active_order_count: int
    bids: tuple[PriceLevelSnapshot, ...]
    asks: tuple[PriceLevelSnapshot, ...]


def _require_u64(name: str, value: int, *, nonzero: bool = False) -> None:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or not 0 <= value <= U64_MAX
        or (nonzero and value == 0)
    ):
        qualifier = "nonzero " if nonzero else ""
        raise ValueError(f"{name} must be a {qualifier}u64")


def _require_i64(name: str, value: int) -> None:
    if isinstance(value, bool) or not isinstance(value, int) or not I64_MIN <= value <= I64_MAX:
        raise ValueError(f"{name} must be an i64")
