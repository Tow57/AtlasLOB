"""Development-only evidence faults used to prove differential diagnosis.

The injectors alter a copied native evidence mapping after the real engine and
strict native decoder have completed their work.  They never add dormant fault
branches to the matching engine itself.
"""

from __future__ import annotations

import copy
import hashlib
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path

from atlaslob.differential import (
    CampaignResult,
    FailureSignature,
    run_fixture,
)
from atlaslob.domain import Command, NewOrder, ReplaceOrder
from atlaslob.native import NativeInputConfig, encode_command, encode_header


class FaultName(StrEnum):
    """Closed vocabulary of intentional Phase 3 comparison faults."""

    NEWEST_AT_PRICE = "newest_at_price"
    INCOMING_TRADE_PRICE = "incoming_trade_price"
    STALE_PARTIAL_AGGREGATE = "stale_partial_aggregate"


INJECTED_FAULTS = tuple(FaultName)


@dataclass(frozen=True, slots=True)
class FaultInjector:
    """Apply one deterministic fault to the first eligible result record."""

    name: FaultName

    def __call__(
        self,
        command: Command | None,
        record: Mapping[str, object],
    ) -> Mapping[str, object]:
        output = copy.deepcopy(dict(record))
        evidence = _result_evidence(output)
        if evidence is None:
            return output
        if self.name == FaultName.NEWEST_AT_PRICE:
            _inject_newest_at_price(evidence)
        elif self.name == FaultName.INCOMING_TRADE_PRICE:
            _inject_incoming_trade_price(command, evidence)
        elif self.name == FaultName.STALE_PARTIAL_AGGREGATE:
            _inject_stale_partial_aggregate(evidence)
        else:  # pragma: no cover - StrEnum construction closes this branch
            raise ValueError(f"unsupported injected fault: {self.name}")
        return output


def run_injected_fault(
    executable: Path,
    config: NativeInputConfig,
    commands: Sequence[Command],
    fault: FaultName,
    working_directory: Path,
    *,
    timeout: float = 10.0,
) -> CampaignResult:
    """Run one fresh reference/native comparison through a named fault."""

    payload = _fixture_bytes(config, commands)
    fingerprint = hashlib.sha256(payload).hexdigest()
    directory = Path(working_directory) / fingerprint
    directory.mkdir(parents=True, exist_ok=True)
    workload_path = directory / "candidate.atlas"
    _write_fixture(workload_path, payload)
    return run_fixture(
        executable,
        workload_path,
        directory / "reference.jsonl",
        mode="exact",
        case_name=f"injected-{fault.value}",
        timeout=timeout,
        evidence_transformer=FaultInjector(fault),
    )


def injected_fault_signature(
    executable: Path,
    config: NativeInputConfig,
    commands: Sequence[Command],
    fault: FaultName,
    working_directory: Path,
    *,
    timeout: float = 10.0,
) -> FailureSignature | None:
    """Return the stable divergence signature or fail on a harness problem."""

    result = run_injected_fault(
        executable,
        config,
        commands,
        fault,
        working_directory,
        timeout=timeout,
    )
    if result.status == "harness_error":
        raise RuntimeError(result.harness_error or "injected-fault harness failed")
    return result.divergence.signature if result.divergence is not None else None


def _write_fixture(
    path: Path,
    payload: bytes,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.read_bytes() == payload:
        return
    temporary = path.with_name(f".{path.name}.tmp")
    with temporary.open("wb") as output:
        output.write(payload)
        output.flush()
    temporary.replace(path)


def _fixture_bytes(
    config: NativeInputConfig,
    commands: Sequence[Command],
) -> bytes:
    lines = [encode_header(config), *(encode_command(command) for command in commands)]
    return ("\n".join(lines) + "\n").encode("ascii")


def _result_evidence(record: dict[str, object]) -> dict[str, object] | None:
    if record.get("kind") != "result":
        return None
    evidence = record.get("evidence")
    return evidence if isinstance(evidence, dict) else None


def _snapshot(evidence: dict[str, object]) -> dict[str, object] | None:
    value = evidence.get("snapshot")
    return value if isinstance(value, dict) else None


def _events(evidence: dict[str, object]) -> list[object] | None:
    value = evidence.get("events")
    return value if isinstance(value, list) else None


def _inject_newest_at_price(evidence: dict[str, object]) -> bool:
    snapshot = _snapshot(evidence)
    if snapshot is None:
        return False
    for side in ("bids", "asks"):
        levels = snapshot.get(side)
        if not isinstance(levels, list):
            continue
        for level in levels:
            if not isinstance(level, dict):
                continue
            orders = level.get("orders")
            if isinstance(orders, list) and len(orders) >= 2:
                orders[0], orders[-1] = orders[-1], orders[0]
                return True
    return False


def _inject_incoming_trade_price(
    command: Command | None,
    evidence: dict[str, object],
) -> bool:
    if isinstance(command, NewOrder):
        incoming_price = command.limit_price
    elif isinstance(command, ReplaceOrder):
        incoming_price = command.new_limit_price
    else:
        incoming_price = None
    if incoming_price is None:
        return False
    events = _events(evidence)
    if events is None:
        return False
    for event in events:
        if not isinstance(event, dict) or event.get("type") != "trade":
            continue
        current = event.get("execution_price")
        replacement = str(incoming_price)
        if current != replacement:
            event["execution_price"] = replacement
            return True
    return False


def _inject_stale_partial_aggregate(evidence: dict[str, object]) -> bool:
    events = _events(evidence)
    snapshot = _snapshot(evidence)
    if events is None or snapshot is None:
        return False
    for event in events:
        if not isinstance(event, dict) or event.get("type") != "trade":
            continue
        resting_remaining = _decimal(event.get("resting_remaining"))
        execution_quantity = _decimal(event.get("execution_quantity"))
        resting_order_id = event.get("resting_order_id")
        aggressor_side = event.get("aggressor_side")
        if (
            resting_remaining is None
            or resting_remaining == 0
            or execution_quantity is None
            or execution_quantity == 0
            or not isinstance(resting_order_id, str)
            or aggressor_side not in {"buy", "sell"}
        ):
            continue
        passive_side = "asks" if aggressor_side == "buy" else "bids"
        levels = snapshot.get(passive_side)
        if not isinstance(levels, list):
            continue
        for level in levels:
            if not isinstance(level, dict):
                continue
            orders = level.get("orders")
            aggregate = _decimal(level.get("aggregate_quantity"))
            if not isinstance(orders, list) or aggregate is None:
                continue
            if any(
                isinstance(order, dict) and order.get("order_id") == resting_order_id
                for order in orders
            ):
                level["aggregate_quantity"] = str(aggregate + execution_quantity)
                return True
    return False


def _decimal(value: object) -> int | None:
    if (
        not isinstance(value, str)
        or not value
        or (value != "0" and value.startswith("0"))
        or not value.isdecimal()
    ):
        return None
    return int(value)
