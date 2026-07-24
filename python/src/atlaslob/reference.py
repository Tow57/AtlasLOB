"""Straightforward, independent reference matching engine.

The oracle deliberately favors inspectability over speed.  Price levels are
plain dictionaries of FIFO deques, active identity is a separate dictionary,
and best-price traversal sorts integer prices on demand.  It does not import
the native AtlasLOB library or mirror its private storage and planning types.
"""

from __future__ import annotations

from collections import deque
from collections.abc import Iterable
from dataclasses import dataclass

from atlaslob.canonical import state_digest as canonical_state_digest
from atlaslob.domain import (
    ATLASLOB_SEMANTICS_VERSION,
    I64_MAX,
    I64_MIN,
    U8_MAX,
    U32_MAX,
    U64_MAX,
    AcceptedEvent,
    BookChangedEvent,
    BookSnapshot,
    CanceledEvent,
    CancelOrder,
    Command,
    CommandType,
    DoneEvent,
    DoneReason,
    EngineError,
    Event,
    EventBatch,
    EventHeader,
    MatchingConfig,
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


@dataclass(frozen=True, slots=True)
class BookTop:
    """Value-only best bid and ask view."""

    best_bid: TopOfBookLevel | None
    best_ask: TopOfBookLevel | None


@dataclass(slots=True)
class _Order:
    order_id: int
    client_id: int
    instrument_id: int
    side: Side
    price: int
    remaining_quantity: int
    priority_sequence: int


@dataclass(frozen=True, slots=True)
class _Rejection:
    reason: RejectReason
    order_id: int | None


@dataclass(frozen=True, slots=True)
class _FillProjection:
    remaining_quantity: int
    terminal_passive_count: int


_Levels = dict[int, deque[_Order]]


def _is_valid_side(value: int) -> bool:
    return value in (Side.BUY, Side.SELL)


def _is_valid_order_type(value: int) -> bool:
    return value in (OrderType.LIMIT, OrderType.MARKET)


def _is_valid_time_in_force(value: int) -> bool:
    return value in (TimeInForce.GTC, TimeInForce.IOC, TimeInForce.FOK)


def _relevant(order_id: int) -> int | None:
    return order_id if order_id != 0 else None


def _header(events: list[Event], sequence: int, instrument_id: int) -> EventHeader:
    return EventHeader(
        command_sequence=sequence,
        event_index=len(events),
        instrument_id=instrument_id,
    )


def _validate_new_shape(order: NewOrder) -> RejectReason:
    if order.client_id == 0:
        return RejectReason.INVALID_CLIENT_ID
    if order.order_id == 0:
        return RejectReason.INVALID_ORDER_ID
    if order.instrument_id == 0:
        return RejectReason.INVALID_INSTRUMENT_ID
    if order.quantity == 0:
        return RejectReason.INVALID_QUANTITY
    if not _is_valid_side(order.side):
        return RejectReason.INVALID_SIDE
    if not _is_valid_order_type(order.order_type):
        return RejectReason.INVALID_ORDER_TYPE
    if not _is_valid_time_in_force(order.time_in_force):
        return RejectReason.INVALID_TIME_IN_FORCE
    if order.time_in_force == TimeInForce.FOK:
        return RejectReason.UNSUPPORTED_TIME_IN_FORCE
    if order.order_type == OrderType.LIMIT:
        if order.limit_price is None:
            return RejectReason.MISSING_LIMIT_PRICE
        if order.limit_price <= 0:
            return RejectReason.INVALID_PRICE
        return RejectReason.NONE
    if order.limit_price is not None:
        return RejectReason.UNEXPECTED_LIMIT_PRICE
    if order.time_in_force != TimeInForce.IOC:
        return RejectReason.INVALID_ORDER_TYPE_TIME_IN_FORCE
    return RejectReason.NONE


def _validate_cancel_shape(order: CancelOrder) -> RejectReason:
    if order.client_id == 0:
        return RejectReason.INVALID_CLIENT_ID
    if order.order_id == 0:
        return RejectReason.INVALID_ORDER_ID
    if order.instrument_id == 0:
        return RejectReason.INVALID_INSTRUMENT_ID
    return RejectReason.NONE


def _validate_replace_shape(order: ReplaceOrder) -> RejectReason:
    if order.client_id == 0:
        return RejectReason.INVALID_CLIENT_ID
    if order.old_order_id == 0 or order.new_order_id == 0:
        return RejectReason.INVALID_ORDER_ID
    if order.old_order_id == order.new_order_id:
        return RejectReason.INVALID_REPLACEMENT_ID
    if order.instrument_id == 0:
        return RejectReason.INVALID_INSTRUMENT_ID
    if order.new_quantity == 0:
        return RejectReason.INVALID_QUANTITY
    if order.new_limit_price <= 0:
        return RejectReason.INVALID_PRICE
    return RejectReason.NONE


class ReferenceEngine:
    """Independent price-time matching oracle for one routed instrument."""

    def __init__(
        self,
        instrument_id: int,
        config: MatchingConfig | None = None,
        *,
        first_sequence: int = 1,
    ) -> None:
        if (
            isinstance(instrument_id, bool)
            or not isinstance(instrument_id, int)
            or not 1 <= instrument_id <= U32_MAX
        ):
            raise ValueError("instrument_id must be a nonzero u32")
        if (
            isinstance(first_sequence, bool)
            or not isinstance(first_sequence, int)
            or not 1 <= first_sequence <= U64_MAX
        ):
            raise ValueError("first_sequence must be a nonzero u64")

        self._instrument_id = instrument_id
        self._config = config if config is not None else MatchingConfig()
        self._bids: _Levels = {}
        self._asks: _Levels = {}
        self._orders: dict[int, _Order] = {}
        self._next_sequence = first_sequence
        self._last_sequence = first_sequence - 1
        self._sequence_exhausted = False
        self._poisoned = False
        self.assert_invariants()

    @property
    def instrument_id(self) -> int:
        self._ensure_usable()
        return self._instrument_id

    @property
    def active_order_count(self) -> int:
        self._ensure_usable()
        return len(self._orders)

    @property
    def empty(self) -> bool:
        self._ensure_usable()
        return not self._orders

    @property
    def next_sequence(self) -> int:
        self._ensure_usable()
        return self._next_sequence

    @property
    def sequence_exhausted(self) -> bool:
        self._ensure_usable()
        return self._sequence_exhausted

    def execute(self, command: Command) -> ReferenceResult:
        """Submit one already-parsed domain command.

        Values outside their fixed-width domain representation are adapter
        errors and raise ``ValueError`` before sequencing.  All representable
        commands, including domain and state rejects, consume one sequence.
        """

        self._ensure_usable()
        self._require_representable(command)
        sequence = self._issue_sequence()
        if sequence is None:
            return ReferenceResult(error=EngineError.SEQUENCE_EXHAUSTED)

        try:
            if isinstance(command, NewOrder):
                result = self._execute_new(command, sequence)
            elif isinstance(command, CancelOrder):
                result = self._execute_cancel(command, sequence)
            elif isinstance(command, ReplaceOrder):
                result = self._execute_replace(command, sequence)
            else:
                raise TypeError(f"unsupported command type: {type(command)!r}")

            self.assert_invariants()
            return result
        except BaseException:
            # Python is the correctness oracle, not the availability boundary. A MemoryError or
            # unexpected internal exception may occur after ordinary container mutation, so the
            # instance is deliberately fatal rather than pretending it can continue atomically.
            self._poisoned = True
            raise

    def top(self) -> BookTop:
        self._ensure_usable()
        return BookTop(
            best_bid=self._top_level(self._bids, descending=True),
            best_ask=self._top_level(self._asks, descending=False),
        )

    def snapshot(self) -> BookSnapshot:
        self._ensure_usable()
        self.assert_invariants()
        return BookSnapshot(
            semantics_version=ATLASLOB_SEMANTICS_VERSION,
            instrument_id=self._instrument_id,
            last_sequence=self._last_sequence,
            sequence_exhausted=self._sequence_exhausted,
            active_order_count=len(self._orders),
            bids=self._snapshot_levels(self._bids, descending=True),
            asks=self._snapshot_levels(self._asks, descending=False),
        )

    def state_digest(self) -> str:
        return canonical_state_digest(self.snapshot())

    def validate_invariants(self) -> bool:
        return not self.invariant_errors()

    def invariant_errors(self) -> tuple[str, ...]:
        errors: list[str] = []
        seen: dict[int, _Order] = {}
        priorities: set[int] = set()

        if self._poisoned:
            errors.append("reference engine is poisoned by an internal exception")
        if not 1 <= self._instrument_id <= U32_MAX:
            errors.append("routed instrument is not a nonzero u32")
        if self._sequence_exhausted:
            if self._next_sequence != 0 or self._last_sequence != U64_MAX:
                errors.append("exhausted sequence state is inconsistent")
        elif not 1 <= self._next_sequence <= U64_MAX:
            errors.append("next sequence is not a nonzero u64")
        elif self._last_sequence != self._next_sequence - 1:
            errors.append("last and next sequence are inconsistent")

        self._append_side_invariant_errors(
            self._bids,
            Side.BUY,
            seen,
            priorities,
            errors,
        )
        self._append_side_invariant_errors(
            self._asks,
            Side.SELL,
            seen,
            priorities,
            errors,
        )

        if set(seen) != set(self._orders):
            errors.append("active index IDs do not match queued IDs")
        else:
            for order_id, queued in seen.items():
                if self._orders[order_id] is not queued:
                    errors.append(f"active index identity differs for order {order_id}")
        if len(self._orders) > self._config.max_active_orders:
            errors.append("active order count exceeds configured capacity")

        best_bid = max(self._bids, default=None)
        best_ask = min(self._asks, default=None)
        if best_bid is not None and best_ask is not None and best_bid >= best_ask:
            errors.append("resting book is crossed")
        return tuple(errors)

    def assert_invariants(self) -> None:
        errors = self.invariant_errors()
        if errors:
            raise RuntimeError("reference engine invariant failure: " + "; ".join(errors))

    def _ensure_usable(self) -> None:
        if self._poisoned:
            raise RuntimeError("reference engine is poisoned by an earlier internal exception")

    def _issue_sequence(self) -> int | None:
        if self._sequence_exhausted:
            return None
        sequence = self._next_sequence
        self._last_sequence = sequence
        if sequence == U64_MAX:
            self._next_sequence = 0
            self._sequence_exhausted = True
        else:
            self._next_sequence = sequence + 1
        return sequence

    def _execute_new(self, order: NewOrder, sequence: int) -> ReferenceResult:
        rejection = self._validate_new(order)
        if rejection is not None:
            return self._reject(
                sequence,
                order.instrument_id,
                CommandType.NEW,
                rejection,
            )
        if not self._capacity_allows(order, removes_old=None):
            return self._reject(
                sequence,
                order.instrument_id,
                CommandType.NEW,
                _Rejection(RejectReason.CAPACITY_EXCEEDED, order.order_id),
            )

        before = self.top()
        events: list[Event] = [
            AcceptedEvent(
                _header([], sequence, order.instrument_id),
                CommandType.NEW,
            )
        ]
        remaining = self._match(order, sequence, events)
        if self._residual_rests(order, remaining):
            self._rest(order, remaining, sequence)
        self._append_aggressor_terminal(events, order, sequence, remaining)
        self._append_book_changed(events, sequence, before, self.top())
        return ReferenceResult(batch=EventBatch(tuple(events)))

    def _execute_cancel(self, order: CancelOrder, sequence: int) -> ReferenceResult:
        rejection = self._validate_cancel(order)
        if rejection is not None:
            return self._reject(
                sequence,
                order.instrument_id,
                CommandType.CANCEL,
                rejection,
            )

        before = self.top()
        canceled = self._orders[order.order_id]
        events: list[Event] = []
        events.append(
            AcceptedEvent(
                _header(events, sequence, order.instrument_id),
                CommandType.CANCEL,
            )
        )
        events.append(
            CanceledEvent(
                _header(events, sequence, order.instrument_id),
                order.order_id,
                canceled.remaining_quantity,
            )
        )
        events.append(
            DoneEvent(
                _header(events, sequence, order.instrument_id),
                order.order_id,
                DoneReason.CANCELED,
                canceled.remaining_quantity,
            )
        )
        self._erase(canceled)
        self._append_book_changed(events, sequence, before, self.top())
        return ReferenceResult(batch=EventBatch(tuple(events)))

    def _execute_replace(self, command: ReplaceOrder, sequence: int) -> ReferenceResult:
        rejection = self._validate_replace(command)
        if rejection is not None:
            return self._reject(
                sequence,
                command.instrument_id,
                CommandType.REPLACE,
                rejection,
            )

        old = self._orders[command.old_order_id]
        replacement = NewOrder(
            client_id=old.client_id,
            order_id=command.new_order_id,
            instrument_id=old.instrument_id,
            side=old.side,
            order_type=OrderType.LIMIT,
            time_in_force=TimeInForce.GTC,
            limit_price=command.new_limit_price,
            quantity=command.new_quantity,
        )
        if not self._capacity_allows(replacement, removes_old=old):
            return self._reject(
                sequence,
                command.instrument_id,
                CommandType.REPLACE,
                _Rejection(RejectReason.CAPACITY_EXCEEDED, command.new_order_id),
            )

        before = self.top()
        events: list[Event] = []
        events.append(
            AcceptedEvent(
                _header(events, sequence, command.instrument_id),
                CommandType.REPLACE,
            )
        )
        events.append(
            ReplacedEvent(
                _header(events, sequence, command.instrument_id),
                command.old_order_id,
                command.new_order_id,
            )
        )
        events.append(
            CanceledEvent(
                _header(events, sequence, command.instrument_id),
                command.old_order_id,
                old.remaining_quantity,
            )
        )
        events.append(
            DoneEvent(
                _header(events, sequence, command.instrument_id),
                command.old_order_id,
                DoneReason.REPLACED,
                old.remaining_quantity,
            )
        )

        self._erase(old)
        remaining = self._match(replacement, sequence, events)
        if self._residual_rests(replacement, remaining):
            self._rest(replacement, remaining, sequence)
        self._append_aggressor_terminal(events, replacement, sequence, remaining)
        self._append_book_changed(events, sequence, before, self.top())
        return ReferenceResult(batch=EventBatch(tuple(events)))

    def _validate_new(self, order: NewOrder) -> _Rejection | None:
        shape = _validate_new_shape(order)
        if shape != RejectReason.NONE:
            return _Rejection(shape, _relevant(order.order_id))
        if order.instrument_id != self._instrument_id:
            return _Rejection(RejectReason.UNKNOWN_INSTRUMENT, order.order_id)
        if order.quantity > self._config.max_order_quantity:
            return _Rejection(RejectReason.QUANTITY_OUT_OF_RANGE, order.order_id)
        if (
            order.order_type == OrderType.LIMIT
            and order.limit_price is not None
            and order.limit_price % self._config.tick_increment != 0
        ):
            return _Rejection(RejectReason.INVALID_TICK, order.order_id)
        if order.order_id in self._orders:
            return _Rejection(RejectReason.DUPLICATE_ORDER_ID, order.order_id)
        return None

    def _validate_cancel(self, order: CancelOrder) -> _Rejection | None:
        shape = _validate_cancel_shape(order)
        if shape != RejectReason.NONE:
            return _Rejection(shape, _relevant(order.order_id))
        if order.instrument_id != self._instrument_id:
            return _Rejection(RejectReason.UNKNOWN_INSTRUMENT, order.order_id)
        existing = self._orders.get(order.order_id)
        if existing is None:
            return _Rejection(RejectReason.UNKNOWN_ORDER_ID, order.order_id)
        if existing.client_id != order.client_id:
            return _Rejection(RejectReason.OWNERSHIP_MISMATCH, order.order_id)
        if existing.instrument_id != order.instrument_id:
            return _Rejection(RejectReason.INSTRUMENT_MISMATCH, order.order_id)
        return None

    def _validate_replace(self, order: ReplaceOrder) -> _Rejection | None:
        shape = _validate_replace_shape(order)
        if shape != RejectReason.NONE:
            relevant_id = order.old_order_id
            if shape == RejectReason.INVALID_ORDER_ID and order.old_order_id != 0:
                relevant_id = order.new_order_id
            elif shape in (
                RejectReason.INVALID_REPLACEMENT_ID,
                RejectReason.INVALID_QUANTITY,
                RejectReason.INVALID_PRICE,
            ):
                relevant_id = order.new_order_id
            return _Rejection(shape, _relevant(relevant_id))
        if order.instrument_id != self._instrument_id:
            return _Rejection(RejectReason.UNKNOWN_INSTRUMENT, order.old_order_id)
        if order.new_quantity > self._config.max_order_quantity:
            return _Rejection(RejectReason.QUANTITY_OUT_OF_RANGE, order.new_order_id)
        if order.new_limit_price % self._config.tick_increment != 0:
            return _Rejection(RejectReason.INVALID_TICK, order.new_order_id)
        existing = self._orders.get(order.old_order_id)
        if existing is None:
            return _Rejection(RejectReason.UNKNOWN_ORDER_ID, order.old_order_id)
        if existing.client_id != order.client_id:
            return _Rejection(RejectReason.OWNERSHIP_MISMATCH, order.old_order_id)
        if existing.instrument_id != order.instrument_id:
            return _Rejection(RejectReason.INSTRUMENT_MISMATCH, order.old_order_id)
        if order.new_order_id in self._orders:
            return _Rejection(RejectReason.INVALID_REPLACEMENT_ID, order.new_order_id)
        return None

    def _capacity_allows(self, order: NewOrder, removes_old: _Order | None) -> bool:
        projection = self._project_fills(order)
        final_count = (
            len(self._orders) - projection.terminal_passive_count - int(removes_old is not None)
        )
        if self._residual_rests(order, projection.remaining_quantity):
            final_count += 1
        if final_count > self._config.max_active_orders:
            return False
        if not self._residual_rests(order, projection.remaining_quantity):
            return True

        price = order.limit_price
        if price is None:
            raise RuntimeError("resting projection is missing a price")
        levels = self._bids if order.side == Side.BUY else self._asks
        projected_aggregate = sum(queued.remaining_quantity for queued in levels.get(price, ()))
        if (
            removes_old is not None
            and removes_old.side == order.side
            and removes_old.price == price
        ):
            projected_aggregate -= removes_old.remaining_quantity
        return projected_aggregate <= U64_MAX - projection.remaining_quantity

    def _project_fills(self, aggressor: NewOrder) -> _FillProjection:
        opposite = self._asks if aggressor.side == Side.BUY else self._bids
        remaining = aggressor.quantity
        terminal_count = 0
        for price in self._ordered_prices(
            opposite,
            descending=aggressor.side == Side.SELL,
        ):
            if remaining == 0 or not self._crosses(aggressor, price):
                break
            for passive in opposite[price]:
                if remaining == 0:
                    break
                if remaining >= passive.remaining_quantity:
                    remaining -= passive.remaining_quantity
                    terminal_count += 1
                else:
                    remaining = 0
        return _FillProjection(remaining, terminal_count)

    def _match(
        self,
        aggressor: NewOrder,
        sequence: int,
        events: list[Event],
    ) -> int:
        opposite = self._asks if aggressor.side == Side.BUY else self._bids
        remaining = aggressor.quantity
        prices = self._ordered_prices(
            opposite,
            descending=aggressor.side == Side.SELL,
        )
        for price in prices:
            if remaining == 0 or not self._crosses(aggressor, price):
                break
            queue = opposite[price]
            while queue and remaining:
                passive = queue[0]
                execution_quantity = min(remaining, passive.remaining_quantity)
                remaining -= execution_quantity
                passive.remaining_quantity -= execution_quantity
                events.append(
                    TradeEvent(
                        _header(events, sequence, aggressor.instrument_id),
                        aggressor.order_id,
                        passive.order_id,
                        aggressor.client_id,
                        passive.client_id,
                        Side(aggressor.side),
                        passive.price,
                        execution_quantity,
                        remaining,
                        passive.remaining_quantity,
                    )
                )
                if passive.remaining_quantity == 0:
                    queue.popleft()
                    del self._orders[passive.order_id]
            if not queue:
                del opposite[price]
        return remaining

    def _rest(self, order: NewOrder, remaining: int, sequence: int) -> None:
        if order.limit_price is None:
            raise RuntimeError("resting order is missing a price")
        rested = _Order(
            order_id=order.order_id,
            client_id=order.client_id,
            instrument_id=order.instrument_id,
            side=Side(order.side),
            price=order.limit_price,
            remaining_quantity=remaining,
            priority_sequence=sequence,
        )
        levels = self._bids if rested.side == Side.BUY else self._asks
        levels.setdefault(rested.price, deque()).append(rested)
        self._orders[rested.order_id] = rested

    def _erase(self, order: _Order) -> None:
        levels = self._bids if order.side == Side.BUY else self._asks
        queue = levels[order.price]
        for index, queued in enumerate(queue):
            if queued is order:
                del queue[index]
                break
        else:
            raise RuntimeError(f"active order {order.order_id} is not queued")
        if not queue:
            del levels[order.price]
        del self._orders[order.order_id]

    @staticmethod
    def _crosses(aggressor: NewOrder, resting_price: int) -> bool:
        if aggressor.order_type == OrderType.MARKET:
            return True
        if aggressor.limit_price is None:
            raise RuntimeError("limit aggressor is missing a price")
        if aggressor.side == Side.BUY:
            return aggressor.limit_price >= resting_price
        return aggressor.limit_price <= resting_price

    @staticmethod
    def _residual_rests(order: NewOrder, remaining: int) -> bool:
        return (
            remaining != 0
            and order.order_type == OrderType.LIMIT
            and order.time_in_force == TimeInForce.GTC
        )

    def _append_aggressor_terminal(
        self,
        events: list[Event],
        order: NewOrder,
        sequence: int,
        remaining: int,
    ) -> None:
        if self._residual_rests(order, remaining):
            if order.limit_price is None:
                raise RuntimeError("rested event is missing a price")
            events.append(
                RestedEvent(
                    _header(events, sequence, order.instrument_id),
                    order.order_id,
                    order.client_id,
                    Side(order.side),
                    order.limit_price,
                    remaining,
                )
            )
            return
        if remaining == 0:
            reason = DoneReason.FILLED
        elif order.order_type == OrderType.MARKET:
            reason = DoneReason.MARKET_EXHAUSTED
        else:
            reason = DoneReason.IOC_RESIDUAL_CANCELED
        events.append(
            DoneEvent(
                _header(events, sequence, order.instrument_id),
                order.order_id,
                reason,
                remaining,
            )
        )

    def _append_book_changed(
        self,
        events: list[Event],
        sequence: int,
        before: BookTop,
        after: BookTop,
    ) -> None:
        if before == after:
            return
        events.append(
            BookChangedEvent(
                _header(events, sequence, self._instrument_id),
                after.best_bid,
                after.best_ask,
            )
        )

    @staticmethod
    def _reject(
        sequence: int,
        instrument_id: int,
        command_type: CommandType,
        rejection: _Rejection,
    ) -> ReferenceResult:
        event = RejectedEvent(
            EventHeader(sequence, 0, instrument_id),
            command_type,
            rejection.reason,
            rejection.order_id,
        )
        return ReferenceResult(batch=EventBatch((event,)))

    @staticmethod
    def _ordered_prices(levels: _Levels, *, descending: bool) -> list[int]:
        return sorted(levels, reverse=descending)

    @staticmethod
    def _top_level(levels: _Levels, *, descending: bool) -> TopOfBookLevel | None:
        if not levels:
            return None
        price = max(levels) if descending else min(levels)
        aggregate = sum(order.remaining_quantity for order in levels[price])
        return TopOfBookLevel(price, aggregate)

    @classmethod
    def _snapshot_levels(
        cls,
        levels: _Levels,
        *,
        descending: bool,
    ) -> tuple[PriceLevelSnapshot, ...]:
        snapshots: list[PriceLevelSnapshot] = []
        for price in cls._ordered_prices(levels, descending=descending):
            queue = levels[price]
            orders = tuple(
                OrderSnapshot(
                    order_id=order.order_id,
                    client_id=order.client_id,
                    instrument_id=order.instrument_id,
                    side=order.side,
                    price=order.price,
                    remaining_quantity=order.remaining_quantity,
                    priority_sequence=order.priority_sequence,
                )
                for order in queue
            )
            snapshots.append(
                PriceLevelSnapshot(
                    price=price,
                    aggregate_quantity=sum(order.remaining_quantity for order in queue),
                    orders=orders,
                )
            )
        return tuple(snapshots)

    def _append_side_invariant_errors(
        self,
        levels: _Levels,
        expected_side: Side,
        seen: dict[int, _Order],
        priorities: set[int],
        errors: list[str],
    ) -> None:
        for price, queue in levels.items():
            if not queue:
                errors.append(f"price level {price} is empty")
                continue
            if not 1 <= price <= I64_MAX:
                errors.append(f"price level {price} is not positive i64")
            aggregate = 0
            previous_priority = 0
            for order in queue:
                if order.order_id in seen:
                    errors.append(f"order ID {order.order_id} is queued more than once")
                else:
                    seen[order.order_id] = order
                if order.priority_sequence in priorities:
                    errors.append(f"priority sequence {order.priority_sequence} is reused")
                priorities.add(order.priority_sequence)
                if order.side != expected_side:
                    errors.append(f"order {order.order_id} has the wrong side")
                if order.instrument_id != self._instrument_id:
                    errors.append(f"order {order.order_id} has the wrong instrument")
                if order.price != price:
                    errors.append(f"order {order.order_id} has the wrong level price")
                if not 1 <= order.remaining_quantity <= U64_MAX:
                    errors.append(f"order {order.order_id} has invalid remaining quantity")
                if not 1 <= order.priority_sequence <= self._last_sequence:
                    errors.append(f"order {order.order_id} has invalid priority")
                if order.priority_sequence <= previous_priority:
                    errors.append(f"price level {price} is not FIFO by priority")
                previous_priority = order.priority_sequence
                aggregate += order.remaining_quantity
            if aggregate > U64_MAX:
                errors.append(f"price level {price} aggregate overflows u64")

    @staticmethod
    def _require_representable(command: Command) -> None:
        if isinstance(command, NewOrder):
            ReferenceEngine._require_uint("client_id", command.client_id, U32_MAX)
            ReferenceEngine._require_uint("order_id", command.order_id, U64_MAX)
            ReferenceEngine._require_uint(
                "instrument_id",
                command.instrument_id,
                U32_MAX,
            )
            ReferenceEngine._require_uint("side", command.side, U8_MAX)
            ReferenceEngine._require_uint("order_type", command.order_type, U8_MAX)
            ReferenceEngine._require_uint(
                "time_in_force",
                command.time_in_force,
                U8_MAX,
            )
            if command.limit_price is not None:
                ReferenceEngine._require_i64("limit_price", command.limit_price)
            ReferenceEngine._require_uint("quantity", command.quantity, U64_MAX)
            return
        if isinstance(command, CancelOrder):
            ReferenceEngine._require_uint("client_id", command.client_id, U32_MAX)
            ReferenceEngine._require_uint("order_id", command.order_id, U64_MAX)
            ReferenceEngine._require_uint(
                "instrument_id",
                command.instrument_id,
                U32_MAX,
            )
            return
        if isinstance(command, ReplaceOrder):
            ReferenceEngine._require_uint("client_id", command.client_id, U32_MAX)
            ReferenceEngine._require_uint(
                "old_order_id",
                command.old_order_id,
                U64_MAX,
            )
            ReferenceEngine._require_uint(
                "new_order_id",
                command.new_order_id,
                U64_MAX,
            )
            ReferenceEngine._require_uint(
                "instrument_id",
                command.instrument_id,
                U32_MAX,
            )
            ReferenceEngine._require_i64(
                "new_limit_price",
                command.new_limit_price,
            )
            ReferenceEngine._require_uint(
                "new_quantity",
                command.new_quantity,
                U64_MAX,
            )
            return
        raise TypeError(f"unsupported command type: {type(command)!r}")

    @staticmethod
    def _require_uint(name: str, value: int, maximum: int) -> None:
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
            raise ValueError(f"{name} is outside its unsigned domain representation")

    @staticmethod
    def _require_i64(name: str, value: int) -> None:
        if isinstance(value, bool) or not isinstance(value, int) or not I64_MIN <= value <= I64_MAX:
            raise ValueError(f"{name} is outside its i64 domain representation")


def batches(results: Iterable[ReferenceResult]) -> tuple[EventBatch, ...]:
    """Extract successful domain batches, rejecting engine failures."""

    output: list[EventBatch] = []
    for result in results:
        if result.batch is None:
            raise ValueError(f"result has engine error: {result.error!r}")
        output.append(result.batch)
    return tuple(output)
