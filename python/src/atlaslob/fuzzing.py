"""Bounded byte-driven command fuzzing helpers."""

from __future__ import annotations

from dataclasses import replace

from atlaslob.domain import (
    U32_MAX,
    CancelOrder,
    Command,
    MatchingConfig,
    NewOrder,
    OrderType,
    ReplaceOrder,
    Side,
    TimeInForce,
)

_DEFAULT_FUZZ_CONFIG = MatchingConfig(
    max_order_quantity=1_000,
    tick_increment=1,
    max_active_orders=64,
)


class _Bytes:
    __slots__ = ("_data", "_index")

    def __init__(self, data: bytes) -> None:
        self._data = data or b"\x00"
        self._index = 0

    def take(self) -> int:
        value = self._data[self._index % len(self._data)]
        self._index += 1
        return value

    def u16(self) -> int:
        return (self.take() << 8) | self.take()


def commands_from_bytes(
    data: bytes,
    *,
    instrument_id: int = 7,
    config: MatchingConfig = _DEFAULT_FUZZ_CONFIG,
    max_commands: int = 64,
) -> tuple[Command, ...]:
    """Interpret arbitrary bytes as a bounded, fully representable command sequence."""

    if not 1 <= instrument_id <= U32_MAX:
        raise ValueError("instrument_id must be a nonzero u32")
    if max_commands < 1:
        raise ValueError("max_commands must be positive")
    source = _Bytes(data)
    count = min(max_commands, 1 + source.take() % max_commands)
    commands: list[Command] = []
    for _ in range(count):
        kind = source.take() % 3
        flags = source.take()
        client_id = 1 + source.take() % 16
        order_id = 1 + source.u16()
        command_instrument = instrument_id
        if flags & 0x01:
            client_id = 0
        if flags & 0x02:
            order_id = 0
        if flags & 0x04:
            command_instrument = 0 if flags & 0x08 else _other_instrument(instrument_id)

        if kind == 0:
            side: int = Side.BUY if source.take() % 2 == 0 else Side.SELL
            order_type: int = OrderType.LIMIT if source.take() % 3 != 0 else OrderType.MARKET
            time_in_force: int = (
                TimeInForce.GTC
                if order_type == OrderType.LIMIT and source.take() % 2 == 0
                else TimeInForce.IOC
            )
            price = (1 + source.u16() % 512) * config.tick_increment
            limit_price: int | None = price if order_type == OrderType.LIMIT else None
            quantity = 1 + source.u16() % min(config.max_order_quantity, 1_000)
            invalid_selector = source.take() % 12 if flags & 0x80 else -1
            if invalid_selector == 0:
                side = 0
            elif invalid_selector == 1:
                order_type = 0
            elif invalid_selector == 2:
                time_in_force = 0
            elif invalid_selector == 3:
                time_in_force = TimeInForce.FOK
            elif invalid_selector == 4:
                limit_price = None
                order_type = OrderType.LIMIT
            elif invalid_selector == 5:
                limit_price = price
                order_type = OrderType.MARKET
            elif invalid_selector == 6:
                limit_price = 0
                order_type = OrderType.LIMIT
            elif invalid_selector == 7:
                quantity = 0
            elif invalid_selector == 8 and config.max_order_quantity < (1 << 64) - 1:
                quantity = config.max_order_quantity + 1
            commands.append(
                NewOrder(
                    client_id=client_id,
                    order_id=order_id,
                    instrument_id=command_instrument,
                    side=side,
                    order_type=order_type,
                    time_in_force=time_in_force,
                    limit_price=limit_price,
                    quantity=quantity,
                )
            )
        elif kind == 1:
            commands.append(CancelOrder(client_id, order_id, command_instrument))
        else:
            new_order_id = 1 + source.u16()
            if flags & 0x10:
                new_order_id = order_id
            price = (1 + source.u16() % 512) * config.tick_increment
            quantity = 1 + source.u16() % min(config.max_order_quantity, 1_000)
            if flags & 0x20:
                price = 0
            if flags & 0x40:
                quantity = 0
            commands.append(
                ReplaceOrder(
                    client_id=client_id,
                    old_order_id=order_id,
                    new_order_id=new_order_id,
                    instrument_id=command_instrument,
                    new_limit_price=price,
                    new_quantity=quantity,
                )
            )
    return tuple(commands)


def mutate_one_command(
    commands: tuple[Command, ...],
    data: bytes,
    *,
    routed_instrument: int = 7,
) -> tuple[Command, ...]:
    """Apply one bounded field mutation to a pre-existing command sequence."""

    if not commands:
        raise ValueError("cannot mutate an empty command sequence")
    source = _Bytes(data)
    index = source.take() % len(commands)
    command = commands[index]
    selector = source.take()
    variants: tuple[Command, ...]
    if isinstance(command, NewOrder):
        variants = (
            replace(command, client_id=0),
            replace(command, order_id=0),
            replace(command, instrument_id=_other_instrument(routed_instrument)),
            replace(
                command,
                side=Side.SELL if command.side == Side.BUY else Side.BUY,
            ),
            replace(command, side=0),
            replace(command, time_in_force=TimeInForce.FOK),
            replace(command, quantity=1),
            replace(command, quantity=0),
            replace(
                command,
                limit_price=(None if command.limit_price is not None else 100),
            ),
        )
    elif isinstance(command, CancelOrder):
        variants = (
            replace(command, client_id=0),
            replace(command, order_id=0),
            replace(command, order_id=command.order_id + 1),
            replace(command, instrument_id=_other_instrument(routed_instrument)),
        )
    else:
        variants = (
            replace(command, client_id=0),
            replace(command, old_order_id=0),
            replace(command, new_order_id=0),
            replace(command, new_order_id=command.old_order_id),
            replace(command, instrument_id=_other_instrument(routed_instrument)),
            replace(command, new_limit_price=0),
            replace(command, new_quantity=0),
            replace(command, new_quantity=1),
        )
    replacement = variants[selector % len(variants)]
    return commands[:index] + (replacement,) + commands[index + 1 :]


def _other_instrument(instrument_id: int) -> int:
    return 1 if instrument_id == U32_MAX else instrument_id + 1
