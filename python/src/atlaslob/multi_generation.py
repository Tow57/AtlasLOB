"""Deterministic V2 generation for multi-instrument evidence campaigns."""

from __future__ import annotations

from collections.abc import Iterator, Mapping
from dataclasses import dataclass
from typing import Final

from atlaslob.domain import (
    U32_MAX,
    U64_MAX,
    CancelOrder,
    Command,
    InstrumentConfig,
    MultiInstrumentEngineConfig,
    NewOrder,
    ReplaceOrder,
)
from atlaslob.generation import (
    GeneratedCommand,
    GenerationStats,
    SplitMix64,
    WorkloadGenerator,
    WorkloadSpec,
    spec_from_dict,
    spec_to_dict,
)

MULTI_GENERATOR_VERSION: Final = 2
MULTI_WORKLOAD_SPEC_SCHEMA: Final = "atlas_workload_spec_v2"


@dataclass(frozen=True, slots=True)
class MultiWorkloadSpec:
    """Resolved V2 workload made from independent V1 instrument streams."""

    streams: tuple[WorkloadSpec, ...]
    engine: MultiInstrumentEngineConfig = MultiInstrumentEngineConfig()

    def __post_init__(self) -> None:
        if not isinstance(self.streams, tuple) or not self.streams:
            raise ValueError("streams must be a nonempty tuple")
        if any(not isinstance(stream, WorkloadSpec) for stream in self.streams):
            raise TypeError("streams must contain only WorkloadSpec values")
        instrument_ids = tuple(stream.instrument_id for stream in self.streams)
        if instrument_ids != tuple(sorted(instrument_ids)):
            raise ValueError("streams must be sorted by instrument ID")
        if len(instrument_ids) != len(set(instrument_ids)):
            raise ValueError("stream instrument IDs must be unique")
        if not isinstance(self.engine, MultiInstrumentEngineConfig):
            raise TypeError("engine must be a MultiInstrumentEngineConfig")
        if self.command_count > U64_MAX:
            raise ValueError("total command count must fit in a u64")

    @property
    def command_count(self) -> int:
        return sum(stream.command_count for stream in self.streams)

    @property
    def catalog(self) -> tuple[InstrumentConfig, ...]:
        return tuple(
            InstrumentConfig(stream.instrument_id, stream.engine) for stream in self.streams
        )


@dataclass(frozen=True, slots=True)
class GeneratedMultiCommand:
    """One globally remapped command plus generator-only provenance."""

    command: Command
    source_instrument_id: int
    intended_valid: bool
    intent: str


class _IdentifierRemapper:
    __slots__ = ("_next_order_id", "_order_ids")

    def __init__(self) -> None:
        self._next_order_id = 1
        self._order_ids: dict[tuple[int, int], int] = {}

    def order_id(self, stream_index: int, value: int) -> int:
        if value == 0:
            return 0
        key = (stream_index, value)
        mapped = self._order_ids.get(key)
        if mapped is not None:
            return mapped
        if self._next_order_id > U64_MAX:
            raise OverflowError("multi-instrument order ID namespace is exhausted")
        mapped = self._next_order_id
        self._next_order_id += 1
        self._order_ids[key] = mapped
        return mapped

    def reserve(self) -> int:
        if self._next_order_id > U64_MAX:
            raise OverflowError("multi-instrument order ID namespace is exhausted")
        mapped = self._next_order_id
        self._next_order_id += 1
        return mapped


class MultiWorkloadGenerator(Iterator[GeneratedMultiCommand]):
    """Stable interleaving of independent V1 streams under Generator V2."""

    def __init__(self, spec: MultiWorkloadSpec, seed: int) -> None:
        if not isinstance(spec, MultiWorkloadSpec):
            raise TypeError("spec must be a MultiWorkloadSpec")
        if isinstance(seed, bool) or not isinstance(seed, int) or not 0 <= seed <= U64_MAX:
            raise ValueError("seed must be a u64")
        self._spec = spec
        self._rng = SplitMix64(seed)
        self._streams = tuple(
            WorkloadGenerator(stream, self._rng.next_u64()) for stream in spec.streams
        )
        self._remaining = [stream.command_count for stream in spec.streams]
        self._remapper = _IdentifierRemapper()
        self._unknown_instrument = _unknown_instrument(
            {stream.instrument_id for stream in spec.streams}
        )
        self._special_commands = self._build_special_commands()
        self._special_index = 0
        self._consume_stream_budget(len(self._special_commands))
        self._count = 0
        self._intended_valid = 0
        self._intended_invalid = 0
        self._new_orders = 0
        self._cancels = 0
        self._replaces = 0
        self._intents: dict[str, int] = {}

    def __iter__(self) -> MultiWorkloadGenerator:
        return self

    def __next__(self) -> GeneratedMultiCommand:
        if self._special_index < len(self._special_commands):
            special_output = self._special_commands[self._special_index]
            self._special_index += 1
            self._record(special_output)
            return special_output

        available = tuple(index for index, remaining in enumerate(self._remaining) if remaining)
        if not available:
            raise StopIteration
        stream_index = available[self._rng.randbelow(len(available))]
        base_generated = next(self._streams[stream_index])
        self._remaining[stream_index] -= 1
        source = self._spec.streams[stream_index].instrument_id
        command = self._remap_command(stream_index, source, base_generated)
        intent = f"instrument_{source}/{base_generated.intent}"
        output = GeneratedMultiCommand(
            command=command,
            source_instrument_id=source,
            intended_valid=base_generated.intended_valid,
            intent=intent,
        )
        self._record(output)
        return output

    @property
    def stats(self) -> GenerationStats:
        return GenerationStats(
            command_count=self._count,
            intended_valid=self._intended_valid,
            intended_invalid=self._intended_invalid,
            new_orders=self._new_orders,
            cancels=self._cancels,
            replaces=self._replaces,
            intents=tuple(sorted(self._intents.items())),
        )

    def _remap_command(
        self,
        stream_index: int,
        source_instrument: int,
        generated: GeneratedCommand,
    ) -> Command:
        command = generated.command
        instrument_id = (
            command.instrument_id
            if command.instrument_id in (0, source_instrument)
            else self._unknown_instrument
        )
        if isinstance(command, NewOrder):
            return NewOrder(
                client_id=command.client_id,
                order_id=self._remapper.order_id(stream_index, command.order_id),
                instrument_id=instrument_id,
                side=command.side,
                order_type=command.order_type,
                time_in_force=command.time_in_force,
                limit_price=command.limit_price,
                quantity=command.quantity,
            )
        if isinstance(command, CancelOrder):
            return CancelOrder(
                client_id=command.client_id,
                order_id=self._remapper.order_id(stream_index, command.order_id),
                instrument_id=instrument_id,
            )
        return ReplaceOrder(
            client_id=command.client_id,
            old_order_id=self._remapper.order_id(stream_index, command.old_order_id),
            new_order_id=self._remapper.order_id(stream_index, command.new_order_id),
            instrument_id=instrument_id,
            new_limit_price=command.new_limit_price,
            new_quantity=command.new_quantity,
        )

    def _build_special_commands(self) -> tuple[GeneratedMultiCommand, ...]:
        if len(self._spec.streams) < 2:
            return ()
        first = self._spec.streams[0]
        second = self._spec.streams[1]
        if (
            first.engine.max_active_orders < 1
            or second.engine.max_active_orders < 1
            or self._spec.engine.max_total_active_orders < 1
        ):
            return self._capacity_zero_special(first)

        client_id = 1
        wrong_client_id = 2
        shared_id = self._remapper.reserve()
        replacement_id = self._remapper.reserve()
        first_price = first.engine.tick_increment
        second_price = second.engine.tick_increment
        cross_identity = (
            self._special(
                NewOrder(
                    client_id,
                    shared_id,
                    first.instrument_id,
                    1,
                    1,
                    1,
                    first_price,
                    1,
                ),
                first.instrument_id,
                True,
                "multi/cross_book_identity_seed",
            ),
            self._special(
                NewOrder(
                    client_id,
                    shared_id,
                    second.instrument_id,
                    1,
                    1,
                    1,
                    second_price,
                    1,
                ),
                second.instrument_id,
                False,
                "multi/cross_book_duplicate_id",
            ),
            self._special(
                CancelOrder(wrong_client_id, shared_id, second.instrument_id),
                second.instrument_id,
                False,
                "multi/routed_cancel_ownership_precedence",
            ),
            self._special(
                CancelOrder(client_id, shared_id, second.instrument_id),
                second.instrument_id,
                False,
                "multi/routed_cancel_instrument_mismatch",
            ),
            self._special(
                ReplaceOrder(
                    wrong_client_id,
                    shared_id,
                    replacement_id,
                    second.instrument_id,
                    second_price,
                    1,
                ),
                second.instrument_id,
                False,
                "multi/routed_replace_ownership_precedence",
            ),
            self._special(
                ReplaceOrder(
                    client_id,
                    shared_id,
                    replacement_id,
                    second.instrument_id,
                    second_price,
                    1,
                ),
                second.instrument_id,
                False,
                "multi/routed_replace_instrument_mismatch",
            ),
            self._special(
                CancelOrder(client_id, shared_id, first.instrument_id),
                first.instrument_id,
                True,
                "multi/terminal_before_cross_instrument_reuse",
            ),
            self._special(
                NewOrder(
                    client_id,
                    shared_id,
                    second.instrument_id,
                    1,
                    1,
                    1,
                    second_price,
                    1,
                ),
                second.instrument_id,
                True,
                "multi/cross_instrument_terminal_id_reuse",
            ),
            self._special(
                CancelOrder(client_id, shared_id, second.instrument_id),
                second.instrument_id,
                True,
                "multi/terminal_reuse_cleanup",
            ),
        )
        if len(cross_identity) > self._spec.command_count:
            return ()

        capacity = self._binding_capacity_specials(first, second, client_id)
        if len(cross_identity) + len(capacity) > self._spec.command_count:
            capacity = ()
        return cross_identity + capacity

    def _capacity_zero_special(
        self,
        first: WorkloadSpec,
    ) -> tuple[GeneratedMultiCommand, ...]:
        if self._spec.engine.max_total_active_orders != 0 or self._spec.command_count < 1:
            return ()
        order_id = self._remapper.reserve()
        return (
            self._special(
                NewOrder(
                    1,
                    order_id,
                    first.instrument_id,
                    1,
                    1,
                    1,
                    first.engine.tick_increment,
                    1,
                ),
                first.instrument_id,
                False,
                "multi/global_capacity_exceeded",
            ),
        )

    def _binding_capacity_specials(
        self,
        first: WorkloadSpec,
        second: WorkloadSpec,
        client_id: int,
    ) -> tuple[GeneratedMultiCommand, ...]:
        capacity = self._spec.engine.max_total_active_orders
        if not 1 <= capacity <= 8:
            return ()
        streams = (first, second)
        counts = [0, 0]
        fills: list[tuple[int, int]] = []
        commands: list[GeneratedMultiCommand] = []
        for _ in range(capacity):
            available = tuple(
                index
                for index, stream in enumerate(streams)
                if counts[index] < stream.engine.max_active_orders
            )
            if not available:
                return ()
            stream_index = available[len(fills) % len(available)]
            stream = streams[stream_index]
            order_id = self._remapper.reserve()
            counts[stream_index] += 1
            fills.append((stream_index, order_id))
            commands.append(
                self._special(
                    NewOrder(
                        client_id,
                        order_id,
                        stream.instrument_id,
                        1,
                        1,
                        1,
                        stream.engine.tick_increment,
                        1,
                    ),
                    stream.instrument_id,
                    True,
                    "multi/global_capacity_fill",
                )
            )

        overflow_stream_index = next(
            (
                index
                for index, stream in enumerate(streams)
                if counts[index] < stream.engine.max_active_orders
            ),
            None,
        )
        if overflow_stream_index is None:
            return ()
        overflow_stream = streams[overflow_stream_index]
        overflow_id = self._remapper.reserve()
        commands.append(
            self._special(
                NewOrder(
                    client_id,
                    overflow_id,
                    overflow_stream.instrument_id,
                    1,
                    1,
                    1,
                    overflow_stream.engine.tick_increment,
                    1,
                ),
                overflow_stream.instrument_id,
                False,
                "multi/global_capacity_exceeded",
            )
        )
        for stream_index, order_id in fills:
            stream = streams[stream_index]
            commands.append(
                self._special(
                    CancelOrder(client_id, order_id, stream.instrument_id),
                    stream.instrument_id,
                    True,
                    "multi/global_capacity_cleanup",
                )
            )
        return tuple(commands)

    @staticmethod
    def _special(
        command: Command,
        source_instrument_id: int,
        intended_valid: bool,
        intent: str,
    ) -> GeneratedMultiCommand:
        return GeneratedMultiCommand(
            command=command,
            source_instrument_id=source_instrument_id,
            intended_valid=intended_valid,
            intent=intent,
        )

    def _consume_stream_budget(self, count: int) -> None:
        index = 0
        remaining = count
        while remaining:
            stream_index = index % len(self._remaining)
            if self._remaining[stream_index]:
                self._remaining[stream_index] -= 1
                remaining -= 1
            index += 1

    def _record(self, generated: GeneratedMultiCommand) -> None:
        self._count += 1
        if generated.intended_valid:
            self._intended_valid += 1
        else:
            self._intended_invalid += 1
        self._intents[generated.intent] = self._intents.get(generated.intent, 0) + 1
        if isinstance(generated.command, NewOrder):
            self._new_orders += 1
        elif isinstance(generated.command, CancelOrder):
            self._cancels += 1
        else:
            self._replaces += 1


def iter_multi_generated(
    spec: MultiWorkloadSpec,
    seed: int,
) -> MultiWorkloadGenerator:
    return MultiWorkloadGenerator(spec, seed)


def iter_multi_commands(spec: MultiWorkloadSpec, seed: int) -> Iterator[Command]:
    for generated in iter_multi_generated(spec, seed):
        yield generated.command


def multi_spec_to_dict(spec: MultiWorkloadSpec) -> dict[str, object]:
    return {
        "schema": MULTI_WORKLOAD_SPEC_SCHEMA,
        "max_total_active_orders": str(spec.engine.max_total_active_orders),
        "streams": [spec_to_dict(stream) for stream in spec.streams],
    }


def multi_spec_from_dict(value: Mapping[str, object]) -> MultiWorkloadSpec:
    if set(value) != {"schema", "max_total_active_orders", "streams"}:
        raise ValueError("multi workload spec has unexpected fields")
    if value["schema"] != MULTI_WORKLOAD_SPEC_SCHEMA:
        raise ValueError("unsupported multi workload spec schema")
    raw_streams = value["streams"]
    if not isinstance(raw_streams, list):
        raise ValueError("streams must be an array")
    raw_capacity = value["max_total_active_orders"]
    if (
        not isinstance(raw_capacity, str)
        or not raw_capacity.isascii()
        or not raw_capacity.isdecimal()
    ):
        raise ValueError("max_total_active_orders must be a canonical decimal string")
    capacity = int(raw_capacity)
    if not 0 <= capacity <= U64_MAX or str(capacity) != raw_capacity:
        raise ValueError("max_total_active_orders must be a canonical u64")
    streams: list[WorkloadSpec] = []
    for index, raw_stream in enumerate(raw_streams):
        if not isinstance(raw_stream, Mapping):
            raise ValueError(f"streams[{index}] must be an object")
        streams.append(spec_from_dict(raw_stream))
    return MultiWorkloadSpec(
        streams=tuple(streams),
        engine=MultiInstrumentEngineConfig(capacity),
    )


def _unknown_instrument(configured: set[int]) -> int:
    for candidate in range(1, U32_MAX + 1):
        if candidate not in configured:
            return candidate
    raise ValueError("catalog leaves no representable unknown instrument ID")
