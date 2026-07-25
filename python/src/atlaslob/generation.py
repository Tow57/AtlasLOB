"""Deterministic, versioned workload generation for differential campaigns.

The generation shadow deliberately models only enough lifecycle state to choose
meaningful commands.  It never emits expected events, snapshots, or digests and
is therefore not a correctness oracle.
"""

from __future__ import annotations

from collections import deque
from collections.abc import Iterator, Mapping, Sequence
from dataclasses import dataclass
from enum import StrEnum
from typing import Final

from atlaslob.domain import (
    I64_MAX,
    U32_MAX,
    U64_MAX,
    CancelOrder,
    Command,
    MatchingConfig,
    NewOrder,
    OrderType,
    ReplaceOrder,
    Side,
    TimeInForce,
)

GENERATOR_VERSION: Final = 1
WORKLOAD_SPEC_SCHEMA: Final = "atlas_workload_spec_v1"
_U64_MODULUS: Final = 1 << 64
_U64_MASK: Final = U64_MAX
_BASIS_POINTS: Final = 10_000
_DEFAULT_ENGINE: Final = MatchingConfig(
    max_order_quantity=1_000,
    tick_increment=1,
    max_active_orders=128,
)


def _require_int(name: str, value: int, *, minimum: int, maximum: int) -> None:
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise ValueError(f"{name} must be an integer in [{minimum}, {maximum}]")


class WorkloadProfile(StrEnum):
    """Named workload families required by the Phase 3 campaign contract."""

    UNIFORM_SYNTHETIC = "uniform_synthetic"
    CLUSTERED_MID = "clustered_mid"
    HOT_LEVEL_CONTENTION = "hot_level_contention"
    SPARSE_WIDE = "sparse_wide"
    CANCEL_HEAVY = "cancel_heavy"
    SWEEP_HEAVY = "sweep_heavy"
    REPLACE_HEAVY = "replace_heavy"
    INVALID_MIX = "invalid_mix"
    TRACE_DRIVEN_SYNTHETIC = "trace_driven_synthetic"
    ADVERSARIAL_BOUNDARY = "adversarial_boundary"


class PriceModel(StrEnum):
    """Resolved price-distribution algorithm."""

    UNIFORM = "uniform"
    CLUSTERED = "clustered"
    HOT_LEVEL = "hot_level"
    SPARSE_WIDE = "sparse_wide"
    TREND_MEAN_REVERT = "trend_mean_revert"
    ADVERSARIAL_LEGAL = "adversarial_legal"


@dataclass(frozen=True, slots=True)
class OperationWeights:
    """Integer-only command-selection weights."""

    new: int
    cancel: int
    replace: int

    def __post_init__(self) -> None:
        for name, value in (("new", self.new), ("cancel", self.cancel), ("replace", self.replace)):
            _require_int(name, value, minimum=0, maximum=U32_MAX)
        if self.total == 0:
            raise ValueError("at least one operation weight must be positive")

    @property
    def total(self) -> int:
        return self.new + self.cancel + self.replace


@dataclass(frozen=True, slots=True)
class WorkloadSpec:
    """Fully resolved parameters for one deterministic command stream."""

    command_count: int
    instrument_id: int
    engine: MatchingConfig
    profile: WorkloadProfile
    price_model: PriceModel
    operation_weights: OperationWeights
    invalid_basis_points: int
    aggressive_basis_points: int
    market_basis_points: int
    boundary_quantity_basis_points: int
    mid_price: int
    price_span_ticks: int
    active_order_target: int
    client_count: int
    snapshot_interval: int

    def __post_init__(self) -> None:
        _require_int("command_count", self.command_count, minimum=1, maximum=U64_MAX)
        _require_int("instrument_id", self.instrument_id, minimum=1, maximum=U32_MAX)
        if not isinstance(self.profile, WorkloadProfile):
            raise TypeError("profile must be a WorkloadProfile")
        if not isinstance(self.price_model, PriceModel):
            raise TypeError("price_model must be a PriceModel")
        if not isinstance(self.operation_weights, OperationWeights):
            raise TypeError("operation_weights must be OperationWeights")
        for name, value in (
            ("invalid_basis_points", self.invalid_basis_points),
            ("aggressive_basis_points", self.aggressive_basis_points),
            ("market_basis_points", self.market_basis_points),
            ("boundary_quantity_basis_points", self.boundary_quantity_basis_points),
        ):
            _require_int(name, value, minimum=0, maximum=_BASIS_POINTS)
        _require_int("mid_price", self.mid_price, minimum=1, maximum=I64_MAX)
        if self.mid_price % self.engine.tick_increment != 0:
            raise ValueError("mid_price must be aligned to tick_increment")
        _require_int("price_span_ticks", self.price_span_ticks, minimum=1, maximum=U32_MAX)
        _require_int(
            "active_order_target",
            self.active_order_target,
            minimum=0,
            maximum=self.engine.max_active_orders,
        )
        _require_int("client_count", self.client_count, minimum=1, maximum=U32_MAX)
        _require_int("snapshot_interval", self.snapshot_interval, minimum=1, maximum=U64_MAX)


@dataclass(frozen=True, slots=True)
class GeneratedCommand:
    """One command plus generator-only intent metadata."""

    command: Command
    intended_valid: bool
    intent: str


@dataclass(frozen=True, slots=True)
class GenerationStats:
    """Intent counts collected while a stream is generated."""

    command_count: int
    intended_valid: int
    intended_invalid: int
    new_orders: int
    cancels: int
    replaces: int
    intents: tuple[tuple[str, int], ...]


class SplitMix64:
    """Small, frozen integer PRNG with cross-version deterministic output."""

    __slots__ = ("_state",)

    def __init__(self, seed: int) -> None:
        _require_int("seed", seed, minimum=0, maximum=U64_MAX)
        self._state = seed

    def next_u64(self) -> int:
        self._state = (self._state + 0x9E3779B97F4A7C15) & _U64_MASK
        value = self._state
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & _U64_MASK
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & _U64_MASK
        return (value ^ (value >> 31)) & _U64_MASK

    def randbelow(self, bound: int) -> int:
        _require_int("bound", bound, minimum=1, maximum=_U64_MODULUS)
        limit = _U64_MODULUS - (_U64_MODULUS % bound)
        while True:
            value = self.next_u64()
            if value < limit:
                return value % bound

    def chance(self, basis_points: int) -> bool:
        _require_int(
            "basis_points",
            basis_points,
            minimum=0,
            maximum=_BASIS_POINTS,
        )
        return self.randbelow(_BASIS_POINTS) < basis_points

    def choose(self, values: Sequence[int]) -> int:
        if not values:
            raise ValueError("cannot choose from an empty sequence")
        return values[self.randbelow(len(values))]


@dataclass(slots=True)
class _ShadowOrder:
    order_id: int
    client_id: int
    instrument_id: int
    side: Side
    price: int
    remaining_quantity: int


class _GenerationShadow:
    """Non-oracle state used only to target valid identity and lifecycle commands."""

    __slots__ = ("active", "asks", "bids", "inactive")

    def __init__(self) -> None:
        self.active: dict[int, _ShadowOrder] = {}
        self.bids: dict[int, deque[int]] = {}
        self.asks: dict[int, deque[int]] = {}
        self.inactive: deque[int] = deque(maxlen=256)

    def ordered_active_ids(self) -> tuple[int, ...]:
        return tuple(sorted(self.active))

    def best_bid(self) -> int | None:
        return max(self.bids, default=None)

    def best_ask(self) -> int | None:
        return min(self.asks, default=None)

    def side_total_at_best(self, side: Side) -> int:
        levels = self.bids if side == Side.BUY else self.asks
        price = self.best_bid() if side == Side.BUY else self.best_ask()
        if price is None:
            return 0
        return sum(self.active[order_id].remaining_quantity for order_id in levels[price])

    def side_total(self, side: Side) -> int:
        levels = self.bids if side == Side.BUY else self.asks
        return sum(
            self.active[order_id].remaining_quantity
            for queue in levels.values()
            for order_id in queue
        )

    def apply(self, command: Command) -> None:
        if isinstance(command, CancelOrder):
            self._remove(command.order_id)
            return
        if isinstance(command, ReplaceOrder):
            old = self.active[command.old_order_id]
            side = old.side
            client_id = old.client_id
            instrument_id = old.instrument_id
            self._remove(command.old_order_id)
            self._apply_new(
                NewOrder(
                    client_id=client_id,
                    order_id=command.new_order_id,
                    instrument_id=instrument_id,
                    side=side,
                    order_type=OrderType.LIMIT,
                    time_in_force=TimeInForce.GTC,
                    limit_price=command.new_limit_price,
                    quantity=command.new_quantity,
                )
            )
            return
        self._apply_new(command)

    def _apply_new(self, command: NewOrder) -> None:
        side = Side(command.side)
        remaining = command.quantity
        opposite = self.asks if side == Side.BUY else self.bids
        while remaining > 0 and opposite:
            price = min(opposite) if side == Side.BUY else max(opposite)
            if not self._crosses(side, command.limit_price, price):
                break
            queue = opposite[price]
            resting = self.active[queue[0]]
            executed = min(remaining, resting.remaining_quantity)
            remaining -= executed
            resting.remaining_quantity -= executed
            if resting.remaining_quantity == 0:
                self._remove(resting.order_id)
        if (
            remaining > 0
            and command.order_type == OrderType.LIMIT
            and command.time_in_force == TimeInForce.GTC
        ):
            if command.limit_price is None:
                raise AssertionError("generated GTC limit unexpectedly lacks a price")
            order = _ShadowOrder(
                order_id=command.order_id,
                client_id=command.client_id,
                instrument_id=command.instrument_id,
                side=side,
                price=command.limit_price,
                remaining_quantity=remaining,
            )
            self.active[order.order_id] = order
            levels = self.bids if side == Side.BUY else self.asks
            levels.setdefault(order.price, deque()).append(order.order_id)
        else:
            self.inactive.append(command.order_id)

    def _remove(self, order_id: int) -> None:
        order = self.active.pop(order_id)
        levels = self.bids if order.side == Side.BUY else self.asks
        queue = levels[order.price]
        queue.remove(order_id)
        if not queue:
            del levels[order.price]
        self.inactive.append(order_id)

    @staticmethod
    def _crosses(side: Side, limit_price: int | None, resting_price: int) -> bool:
        if limit_price is None:
            return True
        if side == Side.BUY:
            return resting_price <= limit_price
        return resting_price >= limit_price


class WorkloadGenerator(Iterator[GeneratedCommand]):
    """Generate one replayable stream from a resolved specification and seed."""

    __slots__ = (
        "_count",
        "_intended_invalid",
        "_intended_valid",
        "_intents",
        "_new_orders",
        "_next_order_id",
        "_replaces",
        "_rng",
        "_seed",
        "_shadow",
        "_spec",
        "_cancels",
    )

    def __init__(self, spec: WorkloadSpec, seed: int) -> None:
        if not isinstance(spec, WorkloadSpec):
            raise TypeError("spec must be a WorkloadSpec")
        _require_int("seed", seed, minimum=0, maximum=U64_MAX)
        self._spec = spec
        self._seed = seed
        self._rng = SplitMix64(seed)
        self._shadow = _GenerationShadow()
        self._next_order_id = 1
        self._count = 0
        self._intended_valid = 0
        self._intended_invalid = 0
        self._new_orders = 0
        self._cancels = 0
        self._replaces = 0
        self._intents: dict[str, int] = {}

    def __iter__(self) -> WorkloadGenerator:
        return self

    def __next__(self) -> GeneratedCommand:
        if self._count >= self._spec.command_count:
            raise StopIteration

        intended_invalid = self._rng.chance(self._spec.invalid_basis_points)
        if intended_invalid:
            generated = self._invalid_command()
            self._intended_invalid += 1
        else:
            command, intent = self._valid_command()
            self._shadow.apply(command)
            generated = GeneratedCommand(command, True, intent)
            self._intended_valid += 1

        self._count += 1
        self._intents[generated.intent] = self._intents.get(generated.intent, 0) + 1
        if isinstance(generated.command, NewOrder):
            self._new_orders += 1
        elif isinstance(generated.command, CancelOrder):
            self._cancels += 1
        else:
            self._replaces += 1
        return generated

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

    def _valid_command(self) -> tuple[Command, str]:
        operation = self._choose_operation()
        if operation == "cancel":
            return self._valid_cancel(), "valid_cancel"
        if operation == "replace":
            return self._valid_replace(), "valid_replace"
        command = self._build_valid_new()
        if command.order_type == OrderType.MARKET:
            return command, "valid_market_sweep"
        if command.time_in_force == TimeInForce.IOC:
            return command, "valid_limit_ioc"
        return command, "valid_limit_gtc"

    def _choose_operation(self) -> str:
        active_count = len(self._shadow.active)
        if active_count == 0:
            return "new"
        if active_count >= self._spec.engine.max_active_orders or (
            self._spec.active_order_target != 0
            and active_count > self._spec.active_order_target
            and self._rng.chance(7_500)
        ):
            return "cancel" if self._rng.chance(6_000) else "replace"

        weights = self._spec.operation_weights
        selected = self._rng.randbelow(weights.total)
        if selected < weights.new:
            return "new"
        if selected < weights.new + weights.cancel:
            return "cancel"
        return "replace"

    def _build_valid_new(self) -> NewOrder:
        side = Side.BUY if self._rng.chance(5_000) else Side.SELL
        must_be_terminal = self._spec.engine.max_active_orders == 0
        aggressive = must_be_terminal or self._rng.chance(self._spec.aggressive_basis_points)
        order_id = self._allocate_order_id()
        quantity = self._quantity(side)
        client_id = 1 + self._rng.randbelow(self._spec.client_count)

        if aggressive and self._rng.chance(self._spec.market_basis_points):
            return NewOrder(
                client_id=client_id,
                order_id=order_id,
                instrument_id=self._spec.instrument_id,
                side=side,
                order_type=OrderType.MARKET,
                time_in_force=TimeInForce.IOC,
                limit_price=None,
                quantity=quantity,
            )

        return NewOrder(
            client_id=client_id,
            order_id=order_id,
            instrument_id=self._spec.instrument_id,
            side=side,
            order_type=OrderType.LIMIT,
            time_in_force=TimeInForce.IOC if aggressive else TimeInForce.GTC,
            limit_price=self._price(side, aggressive=aggressive),
            quantity=quantity,
        )

    def _valid_cancel(self) -> CancelOrder:
        order = self._active_choice()
        return CancelOrder(order.client_id, order.order_id, order.instrument_id)

    def _valid_replace(self) -> ReplaceOrder:
        order = self._active_choice()
        return ReplaceOrder(
            client_id=order.client_id,
            old_order_id=order.order_id,
            new_order_id=self._allocate_order_id(),
            instrument_id=order.instrument_id,
            new_limit_price=self._price(order.side, aggressive=self._rng.chance(3_500)),
            new_quantity=self._quantity(order.side),
        )

    def _invalid_command(self) -> GeneratedCommand:
        variants = 30
        variant = self._rng.randbelow(variants)
        command: Command
        if variant < 14:
            command, intent = self._invalid_new(variant)
        elif variant < 20:
            command, intent = self._invalid_cancel(variant - 14)
        else:
            command, intent = self._invalid_replace(variant - 20)
        return GeneratedCommand(command, False, intent)

    def _invalid_new(self, variant: int) -> tuple[NewOrder, str]:
        base = self._build_valid_new()
        if variant == 0:
            return _replace_new(base, client_id=0), "invalid_new_client_zero"
        if variant == 1:
            return _replace_new(base, order_id=0), "invalid_new_order_zero"
        if variant == 2:
            return _replace_new(base, instrument_id=0), "invalid_new_instrument_zero"
        if variant == 3:
            return _replace_new(base, quantity=0), "invalid_new_quantity_zero"
        if variant == 4:
            return _replace_new(base, side=0), "invalid_new_side"
        if variant == 5:
            return _replace_new(base, order_type=0), "invalid_new_order_type"
        if variant == 6:
            return _replace_new(base, time_in_force=0), "invalid_new_time_in_force"
        if variant == 7:
            return (
                _replace_new(
                    base,
                    order_type=OrderType.LIMIT,
                    time_in_force=TimeInForce.GTC,
                    limit_price=None,
                ),
                "invalid_new_missing_limit_price",
            )
        if variant == 8:
            return (
                _replace_new(
                    base,
                    order_type=OrderType.MARKET,
                    time_in_force=TimeInForce.IOC,
                    limit_price=self._spec.mid_price,
                ),
                "invalid_new_unexpected_market_price",
            )
        if variant == 9:
            return (
                _replace_new(
                    base,
                    order_type=OrderType.LIMIT,
                    time_in_force=TimeInForce.GTC,
                    limit_price=0,
                ),
                "invalid_new_price_zero",
            )
        if variant == 10:
            return (
                _replace_new(base, time_in_force=TimeInForce.FOK),
                "invalid_new_unsupported_fok",
            )
        if variant == 11 and self._shadow.active:
            duplicate = self._active_choice()
            return _replace_new(base, order_id=duplicate.order_id), "invalid_new_duplicate_id"
        if variant in (11, 12):
            invalid_quantity = (
                self._spec.engine.max_order_quantity + 1
                if self._spec.engine.max_order_quantity < U64_MAX
                else 0
            )
            return _replace_new(base, quantity=invalid_quantity), "invalid_new_quantity_range"
        if self._spec.engine.tick_increment > 1:
            return (
                _replace_new(
                    base,
                    order_type=OrderType.LIMIT,
                    time_in_force=TimeInForce.GTC,
                    limit_price=self._spec.mid_price + 1,
                ),
                "invalid_new_tick",
            )
        return (
            _replace_new(
                base,
                order_type=OrderType.MARKET,
                time_in_force=TimeInForce.GTC,
                limit_price=None,
            ),
            "invalid_new_type_tif",
        )

    def _invalid_cancel(self, variant: int) -> tuple[CancelOrder, str]:
        if variant == 0:
            return CancelOrder(0, 1, self._spec.instrument_id), "invalid_cancel_client_zero"
        if variant == 1:
            return CancelOrder(1, 0, self._spec.instrument_id), "invalid_cancel_order_zero"
        if variant == 2:
            return CancelOrder(1, 1, 0), "invalid_cancel_instrument_zero"
        if not self._shadow.active:
            unknown = self._allocate_order_id()
            return (
                CancelOrder(1, unknown, self._spec.instrument_id),
                "invalid_cancel_unknown_id",
            )
        order = self._active_choice()
        if variant == 3:
            return (
                CancelOrder(order.client_id, self._allocate_order_id(), order.instrument_id),
                "invalid_cancel_unknown_id",
            )
        if variant == 4:
            wrong_client = 1 if order.client_id != 1 else 2
            return (
                CancelOrder(wrong_client, order.order_id, order.instrument_id),
                "invalid_cancel_ownership",
            )
        return (
            CancelOrder(order.client_id, order.order_id, self._other_instrument()),
            "invalid_cancel_instrument_mismatch",
        )

    def _invalid_replace(self, variant: int) -> tuple[ReplaceOrder, str]:
        if not self._shadow.active:
            return (
                ReplaceOrder(
                    client_id=1,
                    old_order_id=self._allocate_order_id(),
                    new_order_id=self._allocate_order_id(),
                    instrument_id=self._spec.instrument_id,
                    new_limit_price=self._spec.mid_price,
                    new_quantity=1,
                ),
                "invalid_replace_unknown_id",
            )
        order = self._active_choice()
        new_id = self._allocate_order_id()
        price = self._price(order.side, aggressive=False)
        quantity = self._quantity(order.side)
        if variant == 0:
            return (
                ReplaceOrder(0, order.order_id, new_id, order.instrument_id, price, quantity),
                "invalid_replace_client_zero",
            )
        if variant == 1:
            return (
                ReplaceOrder(
                    order.client_id,
                    order.order_id,
                    order.order_id,
                    order.instrument_id,
                    price,
                    quantity,
                ),
                "invalid_replace_same_id",
            )
        if variant == 2:
            return (
                ReplaceOrder(
                    order.client_id,
                    self._allocate_order_id(),
                    new_id,
                    order.instrument_id,
                    price,
                    quantity,
                ),
                "invalid_replace_unknown_id",
            )
        if variant == 3 and len(self._shadow.active) > 1:
            duplicate = self._active_choice(excluding=order.order_id)
            return (
                ReplaceOrder(
                    order.client_id,
                    order.order_id,
                    duplicate.order_id,
                    order.instrument_id,
                    price,
                    quantity,
                ),
                "invalid_replace_duplicate_new_id",
            )
        if variant == 4:
            wrong_client = 1 if order.client_id != 1 else 2
            return (
                ReplaceOrder(
                    wrong_client,
                    order.order_id,
                    new_id,
                    order.instrument_id,
                    price,
                    quantity,
                ),
                "invalid_replace_ownership",
            )
        if variant == 5:
            return (
                ReplaceOrder(
                    order.client_id,
                    order.order_id,
                    new_id,
                    self._other_instrument(),
                    price,
                    quantity,
                ),
                "invalid_replace_instrument_mismatch",
            )
        if variant == 6:
            return (
                ReplaceOrder(
                    order.client_id,
                    0,
                    new_id,
                    order.instrument_id,
                    price,
                    quantity,
                ),
                "invalid_replace_old_zero",
            )
        if variant == 7:
            return (
                ReplaceOrder(
                    order.client_id,
                    order.order_id,
                    0,
                    order.instrument_id,
                    price,
                    quantity,
                ),
                "invalid_replace_new_zero",
            )
        if variant == 8:
            return (
                ReplaceOrder(
                    order.client_id,
                    order.order_id,
                    new_id,
                    order.instrument_id,
                    price,
                    0,
                ),
                "invalid_replace_quantity_zero",
            )
        invalid_price = self._spec.mid_price + 1 if self._spec.engine.tick_increment > 1 else 0
        return (
            ReplaceOrder(
                order.client_id,
                order.order_id,
                new_id,
                order.instrument_id,
                invalid_price,
                quantity,
            ),
            "invalid_replace_price",
        )

    def _active_choice(self, *, excluding: int | None = None) -> _ShadowOrder:
        order_ids = tuple(
            order_id for order_id in self._shadow.ordered_active_ids() if order_id != excluding
        )
        if not order_ids:
            raise AssertionError("active-order choice requires at least one candidate")
        return self._shadow.active[self._rng.choose(order_ids)]

    def _allocate_order_id(self) -> int:
        if self._spec.profile == WorkloadProfile.ADVERSARIAL_BOUNDARY and self._rng.chance(1_000):
            candidate = U64_MAX - self._rng.randbelow(1_024)
            if candidate != 0 and candidate not in self._shadow.active:
                return candidate
        while self._next_order_id in self._shadow.active:
            self._next_order_id += 1
        if self._next_order_id > U64_MAX:
            raise OverflowError("workload exhausted the order-ID domain")
        value = self._next_order_id
        self._next_order_id += 1
        return value

    def _quantity(self, side: Side) -> int:
        maximum = self._spec.engine.max_order_quantity
        if self._rng.chance(self._spec.boundary_quantity_basis_points):
            candidates = [1, maximum]
            if maximum > 1:
                candidates.extend((2, maximum - 1))
            best_total = self._shadow.side_total_at_best(
                Side.SELL if side == Side.BUY else Side.BUY
            )
            side_total = self._shadow.side_total(Side.SELL if side == Side.BUY else Side.BUY)
            for value in (best_total, side_total):
                if 1 <= value <= maximum:
                    candidates.append(value)
                if 1 <= value + 1 <= maximum:
                    candidates.append(value + 1)
            return self._rng.choose(tuple(candidates))
        return 1 + self._rng.randbelow(maximum)

    def _price(self, side: Side, *, aggressive: bool) -> int:
        tick = self._spec.engine.tick_increment
        base = self._base_price()
        best_opposite = self._shadow.best_ask() if side == Side.BUY else self._shadow.best_bid()
        if aggressive and best_opposite is not None:
            sweep_ticks = 1 + self._rng.randbelow(max(1, min(self._spec.price_span_ticks, 16)))
            delta = sweep_ticks * tick
            if side == Side.BUY:
                return _aligned_clamp(best_opposite + delta, tick)
            return _aligned_clamp(best_opposite - delta, tick)

        if not aggressive and best_opposite is not None:
            if side == Side.BUY and base >= best_opposite:
                base = best_opposite - tick
            elif side == Side.SELL and base <= best_opposite:
                base = best_opposite + tick
        return _aligned_clamp(base, tick)

    def _base_price(self) -> int:
        tick = self._spec.engine.tick_increment
        span = self._spec.price_span_ticks
        model = self._spec.price_model
        if model == PriceModel.HOT_LEVEL:
            offset = self._rng.choose((-1, 0, 0, 0, 1))
        elif model == PriceModel.CLUSTERED:
            offset = int(self._rng.randbelow(9)) - 4
        elif model == PriceModel.SPARSE_WIDE:
            sign = -1 if self._rng.chance(5_000) else 1
            offset = sign * (1 + self._rng.randbelow(span)) * 8
        elif model == PriceModel.TREND_MEAN_REVERT:
            phase = (self._count // 64) % 4
            within = self._count % 64
            trend = within if phase in (0, 3) else 63 - within
            direction = 1 if phase < 2 else -1
            noise = int(self._rng.randbelow(7)) - 3
            offset = direction * trend + noise
        elif model == PriceModel.ADVERSARIAL_LEGAL:
            max_aligned = I64_MAX - (I64_MAX % tick)
            return self._rng.choose(
                (
                    tick,
                    min(max_aligned, tick * 2),
                    self._spec.mid_price,
                    max_aligned,
                )
            )
        else:
            offset = int(self._rng.randbelow(2 * span + 1)) - span
        return _aligned_clamp(self._spec.mid_price + offset * tick, tick)

    def _other_instrument(self) -> int:
        return 1 if self._spec.instrument_id == U32_MAX else self._spec.instrument_id + 1


def resolve_workload_spec(
    profile: WorkloadProfile,
    *,
    command_count: int,
    instrument_id: int = 7,
    engine: MatchingConfig = _DEFAULT_ENGINE,
    invalid_basis_points: int | None = None,
    mid_price: int = 10_000,
    price_span_ticks: int = 128,
    active_order_target: int = 64,
    client_count: int = 16,
    snapshot_interval: int = 100,
) -> WorkloadSpec:
    """Resolve every profile default into an explicit, serializable specification."""

    if not isinstance(profile, WorkloadProfile):
        raise TypeError("profile must be a WorkloadProfile")
    defaults = _PROFILE_DEFAULTS[profile]
    resolved_invalid = (
        defaults.invalid_basis_points if invalid_basis_points is None else invalid_basis_points
    )
    return WorkloadSpec(
        command_count=command_count,
        instrument_id=instrument_id,
        engine=engine,
        profile=profile,
        price_model=defaults.price_model,
        operation_weights=defaults.operation_weights,
        invalid_basis_points=resolved_invalid,
        aggressive_basis_points=defaults.aggressive_basis_points,
        market_basis_points=defaults.market_basis_points,
        boundary_quantity_basis_points=defaults.boundary_quantity_basis_points,
        mid_price=mid_price,
        price_span_ticks=price_span_ticks,
        active_order_target=min(active_order_target, engine.max_active_orders),
        client_count=client_count,
        snapshot_interval=snapshot_interval,
    )


def iter_generated(spec: WorkloadSpec, seed: int) -> WorkloadGenerator:
    """Return a stateful iterator that exposes final generation statistics."""

    return WorkloadGenerator(spec, seed)


def iter_commands(spec: WorkloadSpec, seed: int) -> Iterator[Command]:
    """Yield only commands, omitting generator-only intent metadata."""

    for generated in WorkloadGenerator(spec, seed):
        yield generated.command


def spec_to_dict(spec: WorkloadSpec) -> dict[str, object]:
    """Serialize every resolved parameter without implicit profile defaults."""

    return {
        "schema": WORKLOAD_SPEC_SCHEMA,
        "command_count": str(spec.command_count),
        "instrument_id": str(spec.instrument_id),
        "engine": {
            "max_order_quantity": str(spec.engine.max_order_quantity),
            "tick_increment": str(spec.engine.tick_increment),
            "max_active_orders": str(spec.engine.max_active_orders),
        },
        "profile": spec.profile.value,
        "price_model": spec.price_model.value,
        "operation_weights": {
            "new": spec.operation_weights.new,
            "cancel": spec.operation_weights.cancel,
            "replace": spec.operation_weights.replace,
        },
        "invalid_basis_points": spec.invalid_basis_points,
        "aggressive_basis_points": spec.aggressive_basis_points,
        "market_basis_points": spec.market_basis_points,
        "boundary_quantity_basis_points": spec.boundary_quantity_basis_points,
        "mid_price": str(spec.mid_price),
        "price_span_ticks": spec.price_span_ticks,
        "active_order_target": str(spec.active_order_target),
        "client_count": spec.client_count,
        "snapshot_interval": str(spec.snapshot_interval),
    }


def spec_from_dict(value: Mapping[str, object]) -> WorkloadSpec:
    """Strict inverse of :func:`spec_to_dict`."""

    expected = {
        "schema",
        "command_count",
        "instrument_id",
        "engine",
        "profile",
        "price_model",
        "operation_weights",
        "invalid_basis_points",
        "aggressive_basis_points",
        "market_basis_points",
        "boundary_quantity_basis_points",
        "mid_price",
        "price_span_ticks",
        "active_order_target",
        "client_count",
        "snapshot_interval",
    }
    if set(value) != expected:
        raise ValueError("workload spec fields do not match the V1 schema")
    if value["schema"] != WORKLOAD_SPEC_SCHEMA:
        raise ValueError("unsupported workload spec schema")
    engine_value = _mapping(value["engine"], "engine")
    if set(engine_value) != {
        "max_order_quantity",
        "tick_increment",
        "max_active_orders",
    }:
        raise ValueError("engine fields do not match the workload schema")
    weights_value = _mapping(value["operation_weights"], "operation_weights")
    if set(weights_value) != {"new", "cancel", "replace"}:
        raise ValueError("operation weight fields do not match the workload schema")
    return WorkloadSpec(
        command_count=_decimal(value["command_count"], "command_count"),
        instrument_id=_decimal(value["instrument_id"], "instrument_id"),
        engine=MatchingConfig(
            max_order_quantity=_decimal(
                engine_value["max_order_quantity"],
                "max_order_quantity",
            ),
            tick_increment=_signed_decimal(
                engine_value["tick_increment"],
                "tick_increment",
            ),
            max_active_orders=_decimal(
                engine_value["max_active_orders"],
                "max_active_orders",
            ),
        ),
        profile=WorkloadProfile(_string(value["profile"], "profile")),
        price_model=PriceModel(_string(value["price_model"], "price_model")),
        operation_weights=OperationWeights(
            new=_json_int(weights_value["new"], "new"),
            cancel=_json_int(weights_value["cancel"], "cancel"),
            replace=_json_int(weights_value["replace"], "replace"),
        ),
        invalid_basis_points=_json_int(
            value["invalid_basis_points"],
            "invalid_basis_points",
        ),
        aggressive_basis_points=_json_int(
            value["aggressive_basis_points"],
            "aggressive_basis_points",
        ),
        market_basis_points=_json_int(
            value["market_basis_points"],
            "market_basis_points",
        ),
        boundary_quantity_basis_points=_json_int(
            value["boundary_quantity_basis_points"],
            "boundary_quantity_basis_points",
        ),
        mid_price=_signed_decimal(value["mid_price"], "mid_price"),
        price_span_ticks=_json_int(value["price_span_ticks"], "price_span_ticks"),
        active_order_target=_decimal(
            value["active_order_target"],
            "active_order_target",
        ),
        client_count=_json_int(value["client_count"], "client_count"),
        snapshot_interval=_decimal(value["snapshot_interval"], "snapshot_interval"),
    )


@dataclass(frozen=True, slots=True)
class _ProfileDefaults:
    price_model: PriceModel
    operation_weights: OperationWeights
    invalid_basis_points: int
    aggressive_basis_points: int
    market_basis_points: int
    boundary_quantity_basis_points: int


_PROFILE_DEFAULTS: Final = {
    WorkloadProfile.UNIFORM_SYNTHETIC: _ProfileDefaults(
        PriceModel.UNIFORM,
        OperationWeights(60, 20, 20),
        500,
        2_000,
        4_000,
        1_000,
    ),
    WorkloadProfile.CLUSTERED_MID: _ProfileDefaults(
        PriceModel.CLUSTERED,
        OperationWeights(65, 15, 20),
        500,
        2_500,
        3_500,
        1_500,
    ),
    WorkloadProfile.HOT_LEVEL_CONTENTION: _ProfileDefaults(
        PriceModel.HOT_LEVEL,
        OperationWeights(70, 15, 15),
        500,
        3_000,
        3_000,
        2_000,
    ),
    WorkloadProfile.SPARSE_WIDE: _ProfileDefaults(
        PriceModel.SPARSE_WIDE,
        OperationWeights(70, 15, 15),
        500,
        1_500,
        4_000,
        1_500,
    ),
    WorkloadProfile.CANCEL_HEAVY: _ProfileDefaults(
        PriceModel.CLUSTERED,
        OperationWeights(35, 50, 15),
        500,
        1_500,
        3_000,
        1_000,
    ),
    WorkloadProfile.SWEEP_HEAVY: _ProfileDefaults(
        PriceModel.CLUSTERED,
        OperationWeights(75, 10, 15),
        500,
        7_000,
        5_000,
        3_000,
    ),
    WorkloadProfile.REPLACE_HEAVY: _ProfileDefaults(
        PriceModel.CLUSTERED,
        OperationWeights(35, 15, 50),
        500,
        2_500,
        3_000,
        1_500,
    ),
    WorkloadProfile.INVALID_MIX: _ProfileDefaults(
        PriceModel.UNIFORM,
        OperationWeights(50, 25, 25),
        3_000,
        2_000,
        4_000,
        2_000,
    ),
    WorkloadProfile.TRACE_DRIVEN_SYNTHETIC: _ProfileDefaults(
        PriceModel.TREND_MEAN_REVERT,
        OperationWeights(65, 15, 20),
        500,
        3_000,
        3_500,
        1_500,
    ),
    WorkloadProfile.ADVERSARIAL_BOUNDARY: _ProfileDefaults(
        PriceModel.ADVERSARIAL_LEGAL,
        OperationWeights(55, 20, 25),
        1_500,
        4_000,
        4_000,
        8_000,
    ),
}


def _replace_new(
    order: NewOrder,
    *,
    client_id: int | None = None,
    order_id: int | None = None,
    instrument_id: int | None = None,
    side: int | None = None,
    order_type: int | None = None,
    time_in_force: int | None = None,
    limit_price: int | None | object = ...,
    quantity: int | None = None,
) -> NewOrder:
    resolved_price = order.limit_price if limit_price is ... else limit_price
    if resolved_price is not None and not isinstance(resolved_price, int):
        raise TypeError("limit_price replacement must be int or None")
    return NewOrder(
        client_id=order.client_id if client_id is None else client_id,
        order_id=order.order_id if order_id is None else order_id,
        instrument_id=order.instrument_id if instrument_id is None else instrument_id,
        side=order.side if side is None else side,
        order_type=order.order_type if order_type is None else order_type,
        time_in_force=order.time_in_force if time_in_force is None else time_in_force,
        limit_price=resolved_price,
        quantity=order.quantity if quantity is None else quantity,
    )


def _aligned_clamp(value: int, tick: int) -> int:
    maximum = I64_MAX - (I64_MAX % tick)
    clamped = min(max(value, tick), maximum)
    return clamped - (clamped % tick)


def _mapping(value: object, name: str) -> Mapping[str, object]:
    if not isinstance(value, dict) or any(not isinstance(key, str) for key in value):
        raise ValueError(f"{name} must be an object with string keys")
    return value


def _string(value: object, name: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{name} must be a string")
    return value


def _decimal(value: object, name: str) -> int:
    text = _string(value, name)
    if not text or (text != "0" and (text[0] == "0" or not text.isdecimal())):
        raise ValueError(f"{name} must be canonical unsigned decimal")
    return int(text)


def _signed_decimal(value: object, name: str) -> int:
    text = _string(value, name)
    digits = text[1:] if text.startswith("-") else text
    if not digits or (digits != "0" and (digits[0] == "0" or not digits.isdecimal())):
        raise ValueError(f"{name} must be canonical signed decimal")
    if text == "-0":
        raise ValueError(f"{name} must not use negative zero")
    return int(text)


def _json_int(value: object, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{name} must be a JSON integer")
    return value
