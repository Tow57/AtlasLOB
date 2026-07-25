"""Deterministic reducer that preserves V2 instrument routing."""

from __future__ import annotations

import time
from collections.abc import Callable, Sequence
from dataclasses import replace
from typing import TypeVar

from atlaslob.domain import CancelOrder, Command, InstrumentConfig, NewOrder, ReplaceOrder
from atlaslob.shrinking import (
    ShrinkBudget,
    ShrinkResult,
    _BudgetExhausted,
    _CandidateEvaluator,
    _delete_chunks,
    _delete_individual,
)

SignatureT = TypeVar("SignatureT")
_DEFAULT_BUDGET = ShrinkBudget()


def shrink_multi_failure(
    commands: Sequence[Command],
    evaluator: Callable[[tuple[Command, ...], float | None], SignatureT | None],
    signature: SignatureT,
    *,
    catalog: tuple[InstrumentConfig, ...],
    budget: ShrinkBudget = _DEFAULT_BUDGET,
) -> ShrinkResult[SignatureT]:
    """Reduce a routed failure without collapsing configured instruments."""

    started = time.monotonic()
    original = tuple(commands)
    if not original:
        raise ValueError("cannot shrink an empty command sequence")
    configured = {entry.instrument_id: entry for entry in catalog}
    if not configured or len(configured) != len(catalog):
        raise ValueError("catalog must be nonempty with unique instrument IDs")
    checked = _CandidateEvaluator(evaluator, signature, budget)
    try:
        if not checked.preserves(original):
            raise ValueError("the original command sequence does not reproduce the signature")
    except _BudgetExhausted:
        return _result(
            started,
            original,
            original,
            signature,
            checked,
            budget,
            True,
            (),
        )

    current = original
    completed: list[str] = []
    exhausted = False
    try:
        current = _delete_chunks(current, checked)
        completed.append("chunk_deletion")
        current = _delete_individual(current, checked)
        completed.append("individual_deletion")

        remapped = _remap_identifiers(current)
        if remapped != current and checked.preserves(remapped):
            current = remapped
        completed.append("global_identifier_remap")

        current = _simplify_quantities(current, checked)
        completed.append("quantity_reduction")
        current = _simplify_prices(current, checked, configured)
        completed.append("per_instrument_price_reduction")
        current = _delete_individual(current, checked)
        completed.append("final_deletion")
    except _BudgetExhausted:
        exhausted = True
    return _result(
        started,
        original,
        current,
        signature,
        checked,
        budget,
        exhausted,
        tuple(completed),
    )


def _result(
    started: float,
    original: tuple[Command, ...],
    current: tuple[Command, ...],
    signature: SignatureT,
    checked: _CandidateEvaluator[SignatureT],
    budget: ShrinkBudget,
    exhausted: bool,
    completed: tuple[str, ...],
) -> ShrinkResult[SignatureT]:
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
        completed_stages=completed,
    )


def _remap_identifiers(commands: tuple[Command, ...]) -> tuple[Command, ...]:
    order_ids: dict[int, int] = {}
    client_ids: dict[int, int] = {}

    def order_id(value: int) -> int:
        if value == 0:
            return 0
        return order_ids.setdefault(value, len(order_ids) + 1)

    def client_id(value: int) -> int:
        if value == 0:
            return 0
        return client_ids.setdefault(value, len(client_ids) + 1)

    output: list[Command] = []
    for command in commands:
        if isinstance(command, NewOrder):
            output.append(
                replace(
                    command,
                    client_id=client_id(command.client_id),
                    order_id=order_id(command.order_id),
                )
            )
        elif isinstance(command, CancelOrder):
            output.append(
                replace(
                    command,
                    client_id=client_id(command.client_id),
                    order_id=order_id(command.order_id),
                )
            )
        else:
            output.append(
                replace(
                    command,
                    client_id=client_id(command.client_id),
                    old_order_id=order_id(command.old_order_id),
                    new_order_id=order_id(command.new_order_id),
                )
            )
    return tuple(output)


def _simplify_quantities(
    commands: tuple[Command, ...],
    checked: _CandidateEvaluator[SignatureT],
) -> tuple[Command, ...]:
    current = commands
    for index, command in enumerate(tuple(current)):
        if isinstance(command, NewOrder):
            quantity = command.quantity
            candidate_command: Command = replace(command, quantity=1)
        elif isinstance(command, ReplaceOrder):
            quantity = command.new_quantity
            candidate_command = replace(command, new_quantity=1)
        else:
            continue
        if quantity in (0, 1):
            continue
        candidate = current[:index] + (candidate_command,) + current[index + 1 :]
        if checked.preserves(candidate):
            current = candidate
    return current


def _simplify_prices(
    commands: tuple[Command, ...],
    checked: _CandidateEvaluator[SignatureT],
    configured: dict[int, InstrumentConfig],
) -> tuple[Command, ...]:
    current = commands
    for index, command in enumerate(tuple(current)):
        entry = configured.get(command.instrument_id)
        tick = 1 if entry is None else entry.matching.tick_increment
        if isinstance(command, NewOrder):
            if command.limit_price is None or command.limit_price in (0, tick):
                continue
            candidate_command: Command = replace(command, limit_price=tick)
        elif isinstance(command, ReplaceOrder):
            if command.new_limit_price in (0, tick):
                continue
            candidate_command = replace(command, new_limit_price=tick)
        else:
            continue
        candidate = current[:index] + (candidate_command,) + current[index + 1 :]
        if checked.preserves(candidate):
            current = candidate
    return current
