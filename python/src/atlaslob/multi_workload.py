"""Canonical workload and manifest schemas for multi-instrument evidence V2."""

from __future__ import annotations

import hashlib
import json
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from typing import Final

from atlaslob.domain import (
    I64_MAX,
    I64_MIN,
    U32_MAX,
    U64_MAX,
    CancelOrder,
    Command,
    NewOrder,
    ReplaceOrder,
)
from atlaslob.generation import GenerationStats
from atlaslob.multi_generation import (
    MULTI_GENERATOR_VERSION,
    MultiWorkloadSpec,
    multi_spec_from_dict,
    multi_spec_to_dict,
)

MULTI_WORKLOAD_STREAM_SCHEMA: Final = "atlas_workload_stream_v2"
MULTI_WORKLOAD_COMMAND_SCHEMA: Final = "atlas_workload_command_v2"
MULTI_WORKLOAD_MANIFEST_SCHEMA: Final = "atlas_workload_manifest_v2"


@dataclass(frozen=True, slots=True)
class MultiWorkloadManifest:
    generator_version: int
    seed: int
    spec: MultiWorkloadSpec
    command_count: int
    command_stream_sha256: str
    stats: GenerationStats

    def __post_init__(self) -> None:
        if self.generator_version != MULTI_GENERATOR_VERSION:
            raise ValueError("unsupported multi workload generator version")
        _require_u64("seed", self.seed)
        _require_u64("command_count", self.command_count)
        if self.command_count != self.spec.command_count:
            raise ValueError("manifest command count differs from its spec")
        if (
            len(self.command_stream_sha256) != 64
            or self.command_stream_sha256.lower() != self.command_stream_sha256
            or any(character not in "0123456789abcdef" for character in self.command_stream_sha256)
        ):
            raise ValueError("command_stream_sha256 must be lowercase SHA-256 hex")
        if self.stats.command_count != self.command_count:
            raise ValueError("generation stats do not cover the workload")
        if self.stats.intended_valid + self.stats.intended_invalid != self.command_count:
            raise ValueError("generation validity intents do not cover the workload")
        if self.stats.new_orders + self.stats.cancels + self.stats.replaces != self.command_count:
            raise ValueError("generation operation counts do not cover the workload")
        if sum(count for _, count in self.stats.intents) != self.command_count:
            raise ValueError("generation intent histogram does not cover the workload")
        intent_names = [name for name, _ in self.stats.intents]
        if (
            intent_names != sorted(intent_names)
            or len(intent_names) != len(set(intent_names))
            or any(not name or count < 1 for name, count in self.stats.intents)
        ):
            raise ValueError("generation intent histogram is not canonical")


def command_to_dict(command: Command) -> dict[str, object]:
    if isinstance(command, NewOrder):
        return {
            "type": "new",
            "client_id": str(command.client_id),
            "order_id": str(command.order_id),
            "instrument_id": str(command.instrument_id),
            "side": command.side,
            "order_type": command.order_type,
            "time_in_force": command.time_in_force,
            "limit_price": None if command.limit_price is None else str(command.limit_price),
            "quantity": str(command.quantity),
        }
    if isinstance(command, CancelOrder):
        return {
            "type": "cancel",
            "client_id": str(command.client_id),
            "order_id": str(command.order_id),
            "instrument_id": str(command.instrument_id),
        }
    if isinstance(command, ReplaceOrder):
        return {
            "type": "replace",
            "client_id": str(command.client_id),
            "old_order_id": str(command.old_order_id),
            "new_order_id": str(command.new_order_id),
            "instrument_id": str(command.instrument_id),
            "new_limit_price": str(command.new_limit_price),
            "new_quantity": str(command.new_quantity),
        }
    raise TypeError(f"unsupported command type: {type(command)!r}")


def command_from_dict(value: Mapping[str, object]) -> Command:
    command_name = value.get("type")
    if command_name == "new":
        _require_keys(
            value,
            {
                "type",
                "client_id",
                "order_id",
                "instrument_id",
                "side",
                "order_type",
                "time_in_force",
                "limit_price",
                "quantity",
            },
        )
        raw_price = value["limit_price"]
        return NewOrder(
            client_id=_decimal(value["client_id"], "client_id", U32_MAX),
            order_id=_decimal(value["order_id"], "order_id", U64_MAX),
            instrument_id=_decimal(value["instrument_id"], "instrument_id", U32_MAX),
            side=_json_u8(value["side"], "side"),
            order_type=_json_u8(value["order_type"], "order_type"),
            time_in_force=_json_u8(value["time_in_force"], "time_in_force"),
            limit_price=(None if raw_price is None else _signed_decimal(raw_price, "limit_price")),
            quantity=_decimal(value["quantity"], "quantity", U64_MAX),
        )
    if command_name == "cancel":
        _require_keys(value, {"type", "client_id", "order_id", "instrument_id"})
        return CancelOrder(
            client_id=_decimal(value["client_id"], "client_id", U32_MAX),
            order_id=_decimal(value["order_id"], "order_id", U64_MAX),
            instrument_id=_decimal(value["instrument_id"], "instrument_id", U32_MAX),
        )
    if command_name == "replace":
        _require_keys(
            value,
            {
                "type",
                "client_id",
                "old_order_id",
                "new_order_id",
                "instrument_id",
                "new_limit_price",
                "new_quantity",
            },
        )
        return ReplaceOrder(
            client_id=_decimal(value["client_id"], "client_id", U32_MAX),
            old_order_id=_decimal(value["old_order_id"], "old_order_id", U64_MAX),
            new_order_id=_decimal(value["new_order_id"], "new_order_id", U64_MAX),
            instrument_id=_decimal(value["instrument_id"], "instrument_id", U32_MAX),
            new_limit_price=_signed_decimal(value["new_limit_price"], "new_limit_price"),
            new_quantity=_decimal(value["new_quantity"], "new_quantity", U64_MAX),
        )
    raise ValueError("unsupported V2 command type")


def workload_stream_bytes(
    spec: MultiWorkloadSpec,
    commands: Sequence[Command],
) -> bytes:
    if len(commands) != spec.command_count:
        raise ValueError("command stream length differs from its spec")
    header = {
        "schema": MULTI_WORKLOAD_STREAM_SCHEMA,
        "max_total_active_orders": str(spec.engine.max_total_active_orders),
        "catalog": [
            {
                "instrument_id": str(entry.instrument_id),
                "max_order_quantity": str(entry.matching.max_order_quantity),
                "tick_increment": str(entry.matching.tick_increment),
                "max_active_orders": str(entry.matching.max_active_orders),
            }
            for entry in spec.catalog
        ],
    }
    lines = [_canonical_json(header)]
    lines.extend(
        _canonical_json(
            {
                "schema": MULTI_WORKLOAD_COMMAND_SCHEMA,
                "index": str(index),
                "command": command_to_dict(command),
            }
        )
        for index, command in enumerate(commands)
    )
    return ("\n".join(lines) + "\n").encode("ascii")


def build_multi_manifest(
    spec: MultiWorkloadSpec,
    seed: int,
    commands: Sequence[Command],
    stats: GenerationStats,
) -> MultiWorkloadManifest:
    encoded = workload_stream_bytes(spec, commands)
    return MultiWorkloadManifest(
        generator_version=MULTI_GENERATOR_VERSION,
        seed=seed,
        spec=spec,
        command_count=len(commands),
        command_stream_sha256=hashlib.sha256(encoded).hexdigest(),
        stats=stats,
    )


def multi_manifest_to_dict(manifest: MultiWorkloadManifest) -> dict[str, object]:
    return {
        "schema": MULTI_WORKLOAD_MANIFEST_SCHEMA,
        "generator_version": manifest.generator_version,
        "seed": str(manifest.seed),
        "spec": multi_spec_to_dict(manifest.spec),
        "command_count": str(manifest.command_count),
        "command_stream_sha256": manifest.command_stream_sha256,
        "intent_counts": {
            "intended_valid": str(manifest.stats.intended_valid),
            "intended_invalid": str(manifest.stats.intended_invalid),
            "new_orders": str(manifest.stats.new_orders),
            "cancels": str(manifest.stats.cancels),
            "replaces": str(manifest.stats.replaces),
            "intents": {name: str(count) for name, count in manifest.stats.intents},
        },
    }


def multi_manifest_from_dict(value: Mapping[str, object]) -> MultiWorkloadManifest:
    _require_keys(
        value,
        {
            "schema",
            "generator_version",
            "seed",
            "spec",
            "command_count",
            "command_stream_sha256",
            "intent_counts",
        },
    )
    if value["schema"] != MULTI_WORKLOAD_MANIFEST_SCHEMA:
        raise ValueError("unsupported multi workload manifest schema")
    raw_counts = value["intent_counts"]
    if not isinstance(raw_counts, Mapping):
        raise ValueError("intent_counts must be an object")
    _require_keys(
        raw_counts,
        {
            "intended_valid",
            "intended_invalid",
            "new_orders",
            "cancels",
            "replaces",
            "intents",
        },
    )
    raw_intents = raw_counts["intents"]
    if not isinstance(raw_intents, Mapping):
        raise ValueError("intent_counts.intents must be an object")
    if any(not isinstance(name, str) or not name for name in raw_intents):
        raise ValueError("intent names must be nonempty strings")
    command_count = _decimal(value["command_count"], "command_count", U64_MAX)
    stats = GenerationStats(
        command_count=command_count,
        intended_valid=_decimal(raw_counts["intended_valid"], "intended_valid", U64_MAX),
        intended_invalid=_decimal(raw_counts["intended_invalid"], "intended_invalid", U64_MAX),
        new_orders=_decimal(raw_counts["new_orders"], "new_orders", U64_MAX),
        cancels=_decimal(raw_counts["cancels"], "cancels", U64_MAX),
        replaces=_decimal(raw_counts["replaces"], "replaces", U64_MAX),
        intents=tuple(
            sorted(
                (
                    name,
                    _decimal(count, f"intents.{name}", U64_MAX),
                )
                for name, count in raw_intents.items()
            )
        ),
    )
    raw_spec = value["spec"]
    if not isinstance(raw_spec, Mapping):
        raise ValueError("spec must be an object")
    digest = value["command_stream_sha256"]
    if not isinstance(digest, str):
        raise ValueError("command_stream_sha256 must be a string")
    generator_version = value["generator_version"]
    if isinstance(generator_version, bool) or not isinstance(generator_version, int):
        raise ValueError("generator_version must be an integer")
    return MultiWorkloadManifest(
        generator_version=generator_version,
        seed=_decimal(value["seed"], "seed", U64_MAX),
        spec=multi_spec_from_dict(raw_spec),
        command_count=command_count,
        command_stream_sha256=digest,
        stats=stats,
    )


def _canonical_json(value: Mapping[str, object]) -> str:
    return json.dumps(
        value,
        allow_nan=False,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    )


def _require_keys(value: Mapping[str, object], expected: set[str]) -> None:
    if set(value) != expected:
        raise ValueError("V2 mapping has unexpected fields")


def _decimal(value: object, name: str, maximum: int) -> int:
    if (
        not isinstance(value, str)
        or not value.isascii()
        or not value.isdecimal()
        or (len(value) > 1 and value.startswith("0"))
    ):
        raise ValueError(f"{name} must be a canonical decimal string")
    parsed = int(value)
    if not 0 <= parsed <= maximum:
        raise ValueError(f"{name} is outside its representation")
    return parsed


def _signed_decimal(value: object, name: str) -> int:
    if not isinstance(value, str) or not value.isascii():
        raise ValueError(f"{name} must be a canonical signed decimal string")
    if value.startswith("-"):
        digits = value[1:]
        if not digits or not digits.isdecimal() or digits.startswith("0"):
            raise ValueError(f"{name} must be a canonical signed decimal string")
    elif not value.isdecimal() or (len(value) > 1 and value.startswith("0")):
        raise ValueError(f"{name} must be a canonical signed decimal string")
    parsed = int(value)
    if not I64_MIN <= parsed <= I64_MAX:
        raise ValueError(f"{name} is outside i64")
    return parsed


def _json_u8(value: object, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 255:
        raise ValueError(f"{name} must be a JSON u8")
    return value


def _require_u64(name: str, value: int) -> None:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= U64_MAX:
        raise ValueError(f"{name} must be a u64")
