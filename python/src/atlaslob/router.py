"""Independent multi-instrument coordinator for the Python correctness oracle.

The router deliberately owns the concerns that cannot live in an individual
book: global sequencing, configured-instrument routing, active-order identity,
and engine-wide capacity.  Each routed book remains the simple
``ReferenceEngine`` from Phase 3.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import overload

from atlaslob.canonical import engine_state_digest
from atlaslob.domain import (
    ATLASLOB_SEMANTICS_VERSION,
    U64_MAX,
    BookTop,
    CancelOrder,
    Command,
    EngineError,
    EngineSnapshot,
    EventBatch,
    EventHeader,
    InstrumentConfig,
    InstrumentSnapshot,
    MultiInstrumentEngineConfig,
    NewOrder,
    OrderType,
    ReferenceResult,
    RejectedEvent,
    RejectReason,
    ReplaceOrder,
    TimeInForce,
    command_type,
)
from atlaslob.reference import (
    ReferenceEngine,
    _Order,
    _Rejection,
    _relevant,
    _validate_cancel_shape,
    _validate_new_shape,
    _validate_replace_shape,
)


@dataclass(frozen=True, slots=True)
class _ActiveIdentity:
    instrument_id: int
    client_id: int


class ReferenceRouter:
    """Deterministic, eager multi-instrument reference engine."""

    def __init__(
        self,
        catalog: tuple[InstrumentConfig, ...],
        config: MultiInstrumentEngineConfig | None = None,
        *,
        first_sequence: int = 1,
    ) -> None:
        if not isinstance(catalog, tuple):
            raise TypeError("catalog must be a tuple of InstrumentConfig values")
        if not catalog:
            raise ValueError("catalog must contain at least one instrument")
        if any(not isinstance(entry, InstrumentConfig) for entry in catalog):
            raise TypeError("catalog must contain only InstrumentConfig values")
        instrument_ids = tuple(entry.instrument_id for entry in catalog)
        if len(instrument_ids) != len(set(instrument_ids)):
            raise ValueError("catalog instrument IDs must be unique")
        if (
            isinstance(first_sequence, bool)
            or not isinstance(first_sequence, int)
            or not 1 <= first_sequence <= U64_MAX
        ):
            raise ValueError("first_sequence must be a nonzero u64")

        self._catalog = tuple(sorted(catalog, key=lambda entry: entry.instrument_id))
        self._config = config if config is not None else MultiInstrumentEngineConfig()
        if not isinstance(self._config, MultiInstrumentEngineConfig):
            raise TypeError("config must be a MultiInstrumentEngineConfig")
        self._books = {
            entry.instrument_id: ReferenceEngine(entry.instrument_id, entry.matching)
            for entry in self._catalog
        }
        self._active: dict[int, _ActiveIdentity] = {}
        self._next_sequence = first_sequence
        self._last_sequence = first_sequence - 1
        self._sequence_exhausted = False
        self._poisoned = False
        self.assert_invariants()

    @property
    def catalog(self) -> tuple[InstrumentConfig, ...]:
        self._ensure_usable()
        return self._catalog

    @property
    def active_order_count(self) -> int:
        self._ensure_usable()
        return len(self._active)

    @property
    def empty(self) -> bool:
        self._ensure_usable()
        return not self._active

    @property
    def next_sequence(self) -> int:
        self._ensure_usable()
        return self._next_sequence

    @property
    def sequence_exhausted(self) -> bool:
        self._ensure_usable()
        return self._sequence_exhausted

    def contains_instrument(self, instrument_id: int) -> bool:
        self._ensure_usable()
        return instrument_id in self._books

    def execute(self, command: Command) -> ReferenceResult:
        """Submit one representable command under the global sequence policy."""

        self._ensure_usable()
        ReferenceEngine._require_representable(command)
        sequence = self._issue_sequence()
        if sequence is None:
            return ReferenceResult(error=EngineError.SEQUENCE_EXHAUSTED)

        try:
            result = self._execute_sequenced(command, sequence)
            self.assert_invariants()
            return result
        except BaseException:
            self._poisoned = True
            raise

    def top(self, instrument_id: int) -> BookTop | None:
        self._ensure_usable()
        book = self._books.get(instrument_id)
        return None if book is None else book.top()

    @overload
    def snapshot(self) -> EngineSnapshot: ...

    @overload
    def snapshot(self, instrument_id: int) -> InstrumentSnapshot | None: ...

    def snapshot(
        self,
        instrument_id: int | None = None,
    ) -> EngineSnapshot | InstrumentSnapshot | None:
        self._ensure_usable()
        self.assert_invariants()
        if instrument_id is not None:
            book = self._books.get(instrument_id)
            return None if book is None else self._instrument_snapshot(book)
        instruments = tuple(
            self._instrument_snapshot(self._books[entry.instrument_id]) for entry in self._catalog
        )
        return EngineSnapshot(
            semantics_version=ATLASLOB_SEMANTICS_VERSION,
            last_sequence=self._last_sequence,
            sequence_exhausted=self._sequence_exhausted,
            engine_config=self._config,
            active_order_count=len(self._active),
            catalog=self._catalog,
            instruments=instruments,
        )

    def state_digest(self) -> str:
        return engine_state_digest(self.snapshot())

    def validate_invariants(self) -> bool:
        return not self.invariant_errors()

    def invariant_errors(self) -> tuple[str, ...]:
        errors: list[str] = []
        if self._poisoned:
            errors.append("reference router is poisoned by an internal exception")
        if self._sequence_exhausted:
            if self._next_sequence != 0 or self._last_sequence != U64_MAX:
                errors.append("exhausted global sequence state is inconsistent")
        elif not 1 <= self._next_sequence <= U64_MAX:
            errors.append("next global sequence is not a nonzero u64")
        elif self._last_sequence != self._next_sequence - 1:
            errors.append("last and next global sequences are inconsistent")

        catalog_ids = tuple(entry.instrument_id for entry in self._catalog)
        if catalog_ids != tuple(sorted(catalog_ids)):
            errors.append("catalog is not sorted by instrument ID")
        if len(catalog_ids) != len(set(catalog_ids)):
            errors.append("catalog contains a duplicate instrument ID")
        if set(catalog_ids) != set(self._books):
            errors.append("configured books do not exactly match the catalog")

        expected: dict[int, _ActiveIdentity] = {}
        priorities: set[int] = set()
        for entry in self._catalog:
            book = self._books.get(entry.instrument_id)
            if book is None:
                continue
            for error in book.invariant_errors():
                errors.append(f"instrument {entry.instrument_id}: {error}")
            if book._config != entry.matching:
                errors.append(f"instrument {entry.instrument_id} config differs from catalog")
            if book._last_sequence > self._last_sequence:
                errors.append(f"instrument {entry.instrument_id} is ahead of global sequence")
            for order in book._orders.values():
                if order.order_id in expected:
                    errors.append(f"active order ID {order.order_id} is globally duplicated")
                else:
                    expected[order.order_id] = _ActiveIdentity(
                        instrument_id=order.instrument_id,
                        client_id=order.client_id,
                    )
                if order.priority_sequence in priorities:
                    errors.append(
                        f"priority sequence {order.priority_sequence} is globally duplicated"
                    )
                priorities.add(order.priority_sequence)
                if not 1 <= order.priority_sequence <= self._last_sequence:
                    errors.append(f"order {order.order_id} has invalid global priority")

        if expected != self._active:
            errors.append("global active-order directory differs from routed books")
        if len(self._active) > self._config.max_total_active_orders:
            errors.append("active order count exceeds engine-wide capacity")
        return tuple(errors)

    def assert_invariants(self) -> None:
        errors = self.invariant_errors()
        if errors:
            raise RuntimeError("reference router invariant failure: " + "; ".join(errors))

    def _execute_sequenced(self, command: Command, sequence: int) -> ReferenceResult:
        shape = self._shape_rejection(command)
        if shape is not None:
            return self._reject(sequence, command, shape)

        book = self._books.get(command.instrument_id)
        if book is None:
            relevant_id = (
                command.order_id
                if isinstance(command, (NewOrder, CancelOrder))
                else command.old_order_id
            )
            return self._reject(
                sequence,
                command,
                _Rejection(RejectReason.UNKNOWN_INSTRUMENT, relevant_id),
            )

        if isinstance(command, NewOrder):
            rejection = self._validate_new(book, command)
            if rejection is not None:
                return self._reject(sequence, command, rejection)
            if not self._capacity_allows(book, command, removes_old=None):
                return self._reject(
                    sequence,
                    command,
                    _Rejection(RejectReason.CAPACITY_EXCEEDED, command.order_id),
                )
        elif isinstance(command, CancelOrder):
            rejection = self._validate_cancel(command)
            if rejection is not None:
                return self._reject(sequence, command, rejection)
        else:
            rejection, old = self._validate_replace(book, command)
            if rejection is not None:
                return self._reject(sequence, command, rejection)
            if old is None:
                raise RuntimeError("validated replacement is missing its old order")
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
            if not self._capacity_allows(book, replacement, removes_old=old):
                return self._reject(
                    sequence,
                    command,
                    _Rejection(RejectReason.CAPACITY_EXCEEDED, command.new_order_id),
                )

        return self._delegate(book, command, sequence)

    @staticmethod
    def _shape_rejection(command: Command) -> _Rejection | None:
        if isinstance(command, NewOrder):
            reason = _validate_new_shape(command)
            return (
                None
                if reason == RejectReason.NONE
                else _Rejection(reason, _relevant(command.order_id))
            )
        if isinstance(command, CancelOrder):
            reason = _validate_cancel_shape(command)
            return (
                None
                if reason == RejectReason.NONE
                else _Rejection(reason, _relevant(command.order_id))
            )
        reason = _validate_replace_shape(command)
        if reason == RejectReason.NONE:
            return None
        relevant_id = command.old_order_id
        if reason == RejectReason.INVALID_ORDER_ID and command.old_order_id != 0:
            relevant_id = command.new_order_id
        elif reason in (
            RejectReason.INVALID_REPLACEMENT_ID,
            RejectReason.INVALID_QUANTITY,
            RejectReason.INVALID_PRICE,
        ):
            relevant_id = command.new_order_id
        return _Rejection(reason, _relevant(relevant_id))

    def _validate_new(
        self,
        book: ReferenceEngine,
        command: NewOrder,
    ) -> _Rejection | None:
        if command.quantity > book._config.max_order_quantity:
            return _Rejection(RejectReason.QUANTITY_OUT_OF_RANGE, command.order_id)
        if (
            command.order_type == OrderType.LIMIT
            and command.limit_price is not None
            and command.limit_price % book._config.tick_increment != 0
        ):
            return _Rejection(RejectReason.INVALID_TICK, command.order_id)
        if command.order_id in self._active:
            return _Rejection(RejectReason.DUPLICATE_ORDER_ID, command.order_id)
        return None

    def _validate_cancel(self, command: CancelOrder) -> _Rejection | None:
        existing = self._active.get(command.order_id)
        if existing is None:
            return _Rejection(RejectReason.UNKNOWN_ORDER_ID, command.order_id)
        if existing.client_id != command.client_id:
            return _Rejection(RejectReason.OWNERSHIP_MISMATCH, command.order_id)
        if existing.instrument_id != command.instrument_id:
            return _Rejection(RejectReason.INSTRUMENT_MISMATCH, command.order_id)
        return None

    def _validate_replace(
        self,
        book: ReferenceEngine,
        command: ReplaceOrder,
    ) -> tuple[_Rejection | None, _Order | None]:
        if command.new_quantity > book._config.max_order_quantity:
            return (
                _Rejection(RejectReason.QUANTITY_OUT_OF_RANGE, command.new_order_id),
                None,
            )
        if command.new_limit_price % book._config.tick_increment != 0:
            return _Rejection(RejectReason.INVALID_TICK, command.new_order_id), None
        identity = self._active.get(command.old_order_id)
        if identity is None:
            return _Rejection(RejectReason.UNKNOWN_ORDER_ID, command.old_order_id), None
        if identity.client_id != command.client_id:
            return _Rejection(RejectReason.OWNERSHIP_MISMATCH, command.old_order_id), None
        if identity.instrument_id != command.instrument_id:
            return _Rejection(RejectReason.INSTRUMENT_MISMATCH, command.old_order_id), None
        if command.new_order_id in self._active:
            return _Rejection(RejectReason.INVALID_REPLACEMENT_ID, command.new_order_id), None
        old = book._orders.get(command.old_order_id)
        if old is None:
            raise RuntimeError("global directory order is missing from its routed book")
        return None, old

    def _capacity_allows(
        self,
        book: ReferenceEngine,
        replacement: NewOrder,
        *,
        removes_old: _Order | None,
    ) -> bool:
        if not book._capacity_allows(replacement, removes_old):
            return False
        projection = book._project_fills(replacement)
        final_count = (
            len(self._active) - projection.terminal_passive_count - int(removes_old is not None)
        )
        if book._residual_rests(replacement, projection.remaining_quantity):
            final_count += 1
        return final_count <= self._config.max_total_active_orders

    def _delegate(
        self,
        book: ReferenceEngine,
        command: Command,
        sequence: int,
    ) -> ReferenceResult:
        before = set(book._orders)
        result = book._execute_at_sequence(command, sequence)
        if not result.committed:
            raise RuntimeError("prevalidated routed command was unexpectedly rejected")
        after = set(book._orders)
        for order_id in before - after:
            del self._active[order_id]
        for order_id in after - before:
            order = book._orders[order_id]
            self._active[order_id] = _ActiveIdentity(
                instrument_id=order.instrument_id,
                client_id=order.client_id,
            )
        return result

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

    @staticmethod
    def _instrument_snapshot(book: ReferenceEngine) -> InstrumentSnapshot:
        snapshot = book.snapshot()
        return InstrumentSnapshot(
            instrument_id=snapshot.instrument_id,
            active_order_count=snapshot.active_order_count,
            bids=snapshot.bids,
            asks=snapshot.asks,
        )

    @staticmethod
    def _reject(
        sequence: int,
        command: Command,
        rejection: _Rejection,
    ) -> ReferenceResult:
        event = RejectedEvent(
            EventHeader(sequence, 0, command.instrument_id),
            command_type(command),
            rejection.reason,
            rejection.order_id,
        )
        return ReferenceResult(batch=EventBatch((event,)))

    def _ensure_usable(self) -> None:
        if self._poisoned:
            raise RuntimeError("reference router is poisoned by an earlier internal exception")
