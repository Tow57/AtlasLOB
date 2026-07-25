"""Deterministic semantic reduction of divergent command sequences."""

from __future__ import annotations

import hashlib
import math
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass, replace
from typing import Generic, TypeVar

from atlaslob.domain import (
    I64_MAX,
    U32_MAX,
    U64_MAX,
    CancelOrder,
    Command,
    NewOrder,
    OrderType,
    ReplaceOrder,
    Side,
    TimeInForce,
)
from atlaslob.native import encode_command

SignatureT = TypeVar("SignatureT")


@dataclass(frozen=True, slots=True)
class ShrinkBudget:
    """Hard limits for one reducer invocation."""

    max_evaluations: int = 10_000
    timeout_seconds: float | None = None

    def __post_init__(self) -> None:
        if (
            isinstance(self.max_evaluations, bool)
            or not isinstance(self.max_evaluations, int)
            or self.max_evaluations < 1
        ):
            raise ValueError("max_evaluations must be a positive integer")
        if self.timeout_seconds is not None and (
            isinstance(self.timeout_seconds, bool)
            or not isinstance(self.timeout_seconds, (int, float))
            or not math.isfinite(self.timeout_seconds)
            or self.timeout_seconds <= 0
        ):
            raise ValueError("timeout_seconds must be finite and positive when present")


_DEFAULT_BUDGET = ShrinkBudget()


@dataclass(frozen=True, slots=True)
class ShrinkContext:
    """Domain bounds used by deterministic field simplification."""

    routed_instrument: int
    tick_increment: int = 1
    max_quantity: int = U64_MAX

    def __post_init__(self) -> None:
        if not 1 <= self.routed_instrument <= U32_MAX:
            raise ValueError("routed_instrument must be a nonzero u32")
        if not 1 <= self.tick_increment <= I64_MAX:
            raise ValueError("tick_increment must be a positive i64")
        if not 1 <= self.max_quantity <= U64_MAX:
            raise ValueError("max_quantity must be a nonzero u64")


@dataclass(frozen=True, slots=True)
class ShrinkResult(Generic[SignatureT]):
    """A minimized reproducer plus deterministic reducer accounting."""

    original_count: int
    commands: tuple[Command, ...]
    signature: SignatureT
    evaluations: int
    cache_hits: int
    max_evaluations: int
    timeout_seconds: float | None
    elapsed_seconds: float
    budget_exhausted: bool
    completed_stages: tuple[str, ...]


class _BudgetExhausted(RuntimeError):
    pass


class _CandidateEvaluator(Generic[SignatureT]):
    __slots__ = (
        "_budget",
        "_cache",
        "_cache_hits",
        "_deadline",
        "_evaluations",
        "_evaluator",
        "_signature",
    )

    def __init__(
        self,
        evaluator: Callable[[tuple[Command, ...], float | None], SignatureT | None],
        signature: SignatureT,
        budget: ShrinkBudget,
    ) -> None:
        self._evaluator = evaluator
        self._signature = signature
        self._budget = budget
        self._evaluations = 0
        self._cache_hits = 0
        self._cache: dict[bytes, SignatureT | None] = {}
        self._deadline = (
            None if budget.timeout_seconds is None else time.monotonic() + budget.timeout_seconds
        )

    @property
    def evaluations(self) -> int:
        return self._evaluations

    @property
    def cache_hits(self) -> int:
        return self._cache_hits

    def preserves(self, commands: tuple[Command, ...]) -> bool:
        candidate_key = _candidate_key(commands, deadline=self._deadline)
        if candidate_key in self._cache:
            self._cache_hits += 1
            return self._cache[candidate_key] == self._signature
        if self._evaluations >= self._budget.max_evaluations:
            raise _BudgetExhausted
        remaining = _remaining(self._deadline)
        result = self._evaluator(commands, remaining)
        self._evaluations += 1
        if self._deadline is not None and time.monotonic() >= self._deadline:
            raise _BudgetExhausted
        self._cache[candidate_key] = result
        return result == self._signature


def shrink_failure(
    commands: Sequence[Command],
    evaluator: Callable[[tuple[Command, ...], float | None], SignatureT | None],
    signature: SignatureT,
    *,
    context: ShrinkContext,
    budget: ShrinkBudget = _DEFAULT_BUDGET,
) -> ShrinkResult[SignatureT]:
    """Minimize a sequence while preserving one semantic failure signature.

    The evaluator is responsible for constructing fresh reference/native engines
    for every uncached candidate.
    """

    started = time.monotonic()
    original = tuple(commands)
    if not original:
        raise ValueError("cannot shrink an empty command sequence")
    checked = _CandidateEvaluator(evaluator, signature, budget)
    try:
        original_preserves = checked.preserves(original)
    except _BudgetExhausted:
        return ShrinkResult(
            original_count=len(original),
            commands=original,
            signature=signature,
            evaluations=checked.evaluations,
            cache_hits=checked.cache_hits,
            max_evaluations=budget.max_evaluations,
            timeout_seconds=budget.timeout_seconds,
            elapsed_seconds=time.monotonic() - started,
            budget_exhausted=True,
            completed_stages=(),
        )
    if not original_preserves:
        raise ValueError("the original command sequence does not reproduce the signature")

    current = original
    completed: list[str] = []
    exhausted = False
    try:
        current = _delete_chunks(current, checked)
        completed.append("chunk_deletion")
        current = _delete_individual(current, checked)
        completed.append("individual_deletion")

        remapped = _remap_identifiers(current, context.routed_instrument)
        if remapped != current and checked.preserves(remapped):
            current = remapped
        completed.append("identifier_remap")

        current = _simplify_prices(current, checked, context)
        completed.append("price_reduction")
        current = _simplify_quantities(current, checked, context)
        completed.append("quantity_reduction")
        current = _simplify_commands(current, checked, context)
        completed.append("command_simplification")
        current = _normalize_side_and_tif(current, checked, context)
        completed.append("side_tif_normalization")
        current = _simplify_prices(current, checked, context)
        current = _simplify_quantities(current, checked, context)
        completed.append("final_field_reduction")
        current = _delete_individual(current, checked)
        completed.append("final_deletion")
    except _BudgetExhausted:
        exhausted = True

    return ShrinkResult(
        original_count=len(original),
        commands=current,
        signature=signature,
        evaluations=checked.evaluations,
        cache_hits=checked.cache_hits,
        max_evaluations=budget.max_evaluations,
        timeout_seconds=budget.timeout_seconds,
        elapsed_seconds=time.monotonic() - started,
        budget_exhausted=exhausted,
        completed_stages=tuple(completed),
    )


def _candidate_key(
    commands: tuple[Command, ...],
    *,
    deadline: float | None,
) -> bytes:
    digest = hashlib.sha256()
    for command in commands:
        _remaining(deadline)
        digest.update(encode_command(command).encode("ascii"))
        digest.update(b"\n")
    return digest.digest()


def _remaining(deadline: float | None) -> float | None:
    if deadline is None:
        return None
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise _BudgetExhausted
    return remaining


def _delete_chunks(
    commands: tuple[Command, ...],
    evaluator: _CandidateEvaluator[SignatureT],
) -> tuple[Command, ...]:
    current = commands
    granularity = 2
    while len(current) >= 2:
        chunk_size = (len(current) + granularity - 1) // granularity
        reduced = False
        for start in range(0, len(current), chunk_size):
            candidate = current[:start] + current[start + chunk_size :]
            if candidate and evaluator.preserves(candidate):
                current = candidate
                granularity = max(2, granularity - 1)
                reduced = True
                break
        if reduced:
            continue
        if granularity >= len(current):
            break
        granularity = min(len(current), granularity * 2)
    return current


def _delete_individual(
    commands: tuple[Command, ...],
    evaluator: _CandidateEvaluator[SignatureT],
) -> tuple[Command, ...]:
    current = commands
    index = 0
    while index < len(current):
        candidate = current[:index] + current[index + 1 :]
        if candidate and evaluator.preserves(candidate):
            current = candidate
        else:
            index += 1
    return current


def _remap_identifiers(
    commands: tuple[Command, ...],
    routed_instrument: int,
) -> tuple[Command, ...]:
    order_ids: dict[int, int] = {}
    client_ids: dict[int, int] = {}

    def order_id(value: int) -> int:
        if value == 0:
            return 0
        if value not in order_ids:
            order_ids[value] = len(order_ids) + 1
        return order_ids[value]

    def client_id(value: int) -> int:
        if value == 0:
            return 0
        if value not in client_ids:
            client_ids[value] = len(client_ids) + 1
        return client_ids[value]

    def instrument_id(value: int) -> int:
        return routed_instrument if value != 0 else 0

    output: list[Command] = []
    for command in commands:
        if isinstance(command, NewOrder):
            output.append(
                replace(
                    command,
                    client_id=client_id(command.client_id),
                    order_id=order_id(command.order_id),
                    instrument_id=instrument_id(command.instrument_id),
                )
            )
        elif isinstance(command, CancelOrder):
            output.append(
                replace(
                    command,
                    client_id=client_id(command.client_id),
                    order_id=order_id(command.order_id),
                    instrument_id=instrument_id(command.instrument_id),
                )
            )
        else:
            output.append(
                replace(
                    command,
                    client_id=client_id(command.client_id),
                    old_order_id=order_id(command.old_order_id),
                    new_order_id=order_id(command.new_order_id),
                    instrument_id=instrument_id(command.instrument_id),
                )
            )
    return tuple(output)


def _simplify_prices(
    commands: tuple[Command, ...],
    evaluator: _CandidateEvaluator[SignatureT],
    context: ShrinkContext,
) -> tuple[Command, ...]:
    tick = context.tick_increment
    observed = sorted(
        {
            price
            for command in commands
            for price in _command_prices(command)
            if price > 0 and price % tick == 0
        }
    )
    central = observed[len(observed) // 2] if observed else 100 * tick
    candidates = _unique(
        (
            tick,
            min(I64_MAX - (I64_MAX % tick), 2 * tick),
            min(I64_MAX - (I64_MAX % tick), 100 * tick),
            central,
        )
    )
    current = commands
    for candidate_price in candidates:
        candidate = tuple(_with_price(command, candidate_price) for command in current)
        if (
            candidate != current
            and _price_complexity(candidate) < _price_complexity(current)
            and evaluator.preserves(candidate)
        ):
            current = candidate
            break
    for index in range(len(current)):
        for candidate_price in candidates:
            candidate_command = _with_price(current[index], candidate_price)
            if candidate_command == current[index]:
                continue
            candidate = _replace_at(current, index, candidate_command)
            if _price_complexity(candidate) < _price_complexity(current) and evaluator.preserves(
                candidate
            ):
                current = candidate
                break
    return current


def _simplify_quantities(
    commands: tuple[Command, ...],
    evaluator: _CandidateEvaluator[SignatureT],
    context: ShrinkContext,
) -> tuple[Command, ...]:
    observed = sorted(
        {
            quantity
            for command in commands
            for quantity in _command_quantities(command)
            if 1 <= quantity <= context.max_quantity
        }
    )
    candidates = [1]
    if context.max_quantity >= 2:
        candidates.append(2)
    candidates.extend(observed)
    candidates.extend(quantity - 1 for quantity in observed if 1 < quantity <= context.max_quantity)
    ordered_candidates = _unique(tuple(candidates))
    current = commands
    for index in range(len(current)):
        for quantity in ordered_candidates:
            candidate_command = _with_quantity(current[index], quantity)
            if candidate_command == current[index]:
                continue
            candidate = _replace_at(current, index, candidate_command)
            if _quantity_complexity(candidate) < _quantity_complexity(
                current
            ) and evaluator.preserves(candidate):
                current = candidate
                break
    return current


def _simplify_commands(
    commands: tuple[Command, ...],
    evaluator: _CandidateEvaluator[SignatureT],
    context: ShrinkContext,
) -> tuple[Command, ...]:
    current = commands
    for index, command in enumerate(tuple(current)):
        alternatives: list[Command] = []
        if isinstance(command, ReplaceOrder):
            alternatives.append(
                CancelOrder(
                    client_id=command.client_id,
                    order_id=command.old_order_id,
                    instrument_id=command.instrument_id,
                )
            )
        elif isinstance(command, NewOrder) and command.order_type == OrderType.LIMIT:
            alternatives.append(
                replace(
                    command,
                    order_type=OrderType.MARKET,
                    time_in_force=TimeInForce.IOC,
                    limit_price=None,
                )
            )
        for alternative in alternatives:
            candidate = _replace_at(current, index, alternative)
            if evaluator.preserves(candidate):
                current = candidate
                break
    return current


def _normalize_side_and_tif(
    commands: tuple[Command, ...],
    evaluator: _CandidateEvaluator[SignatureT],
    context: ShrinkContext,
) -> tuple[Command, ...]:
    del context
    current = commands
    for index in range(len(current)):
        command = current[index]
        if not isinstance(command, NewOrder):
            continue
        alternatives: list[NewOrder] = []
        if command.side != Side.BUY:
            alternatives.append(replace(command, side=Side.BUY))
        if command.order_type == OrderType.LIMIT and command.time_in_force != TimeInForce.GTC:
            alternatives.append(replace(command, time_in_force=TimeInForce.GTC))
        for alternative in alternatives:
            candidate = _replace_at(current, index, alternative)
            if evaluator.preserves(candidate):
                current = candidate
                command = alternative
    return current


def _command_prices(command: Command) -> tuple[int, ...]:
    if isinstance(command, NewOrder):
        return () if command.limit_price is None else (command.limit_price,)
    if isinstance(command, ReplaceOrder):
        return (command.new_limit_price,)
    return ()


def _command_quantities(command: Command) -> tuple[int, ...]:
    if isinstance(command, NewOrder):
        return (command.quantity,)
    if isinstance(command, ReplaceOrder):
        return (command.new_quantity,)
    return ()


def _with_price(command: Command, price: int) -> Command:
    if isinstance(command, NewOrder) and command.limit_price is not None:
        return replace(command, limit_price=price)
    if isinstance(command, ReplaceOrder):
        return replace(command, new_limit_price=price)
    return command


def _with_quantity(command: Command, quantity: int) -> Command:
    if isinstance(command, NewOrder):
        return replace(command, quantity=quantity)
    if isinstance(command, ReplaceOrder):
        return replace(command, new_quantity=quantity)
    return command


def _replace_at(
    commands: tuple[Command, ...],
    index: int,
    command: Command,
) -> tuple[Command, ...]:
    return commands[:index] + (command,) + commands[index + 1 :]


def _unique(values: tuple[int, ...]) -> tuple[int, ...]:
    return tuple(dict.fromkeys(values))


def _price_complexity(commands: tuple[Command, ...]) -> tuple[int, tuple[int, ...]]:
    prices = tuple(price for command in commands for price in _command_prices(command))
    return sum(abs(price) for price in prices), tuple(abs(price) for price in prices)


def _quantity_complexity(commands: tuple[Command, ...]) -> tuple[int, tuple[int, ...]]:
    quantities = tuple(
        quantity for command in commands for quantity in _command_quantities(command)
    )
    return sum(quantities), quantities
