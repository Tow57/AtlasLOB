"""Exact, bounded projections of native executions; never derived from browser polls."""

from __future__ import annotations

from collections.abc import Iterable, Sequence
from dataclasses import dataclass

from atlaslob.domain import Event, PriceLevelSnapshot, TradeEvent
from atlaslob.ui.models import Candle, ChartRange, DepthPoint, InstrumentSummary


@dataclass(frozen=True, slots=True)
class Bar:
    open: int
    high: int
    low: int
    close: int
    volume: int

    def merge(self, other: Bar) -> Bar:
        return Bar(
            self.open,
            max(self.high, other.high),
            min(self.low, other.low),
            other.close,
            self.volume + other.volume,
        )


class Projection:
    """One immutable Bar per trading command/instrument, plus session aggregates."""

    def __init__(self) -> None:
        self.sequence = 0
        self.commands: dict[int, dict[int, Bar]] = {7: {}, 42: {}}
        self.totals: dict[int, Bar] = {}

    def apply_command(self, sequence: int, events: Iterable[Event]) -> None:
        if sequence != self.sequence + 1:
            raise ValueError("Chart command sequence must advance exactly once.")
        pending: dict[int, Bar] = {}
        previous_index = -1
        for event in events:
            if not isinstance(event, TradeEvent):
                continue
            header = event.header
            if (
                header.command_sequence != sequence
                or header.event_index <= previous_index
                or header.instrument_id not in self.commands
                or event.execution_quantity <= 0
            ):
                raise ValueError("Invalid or unordered native trade while building chart history.")
            previous_index = header.event_index
            price = event.execution_price
            bar = Bar(price, price, price, price, event.execution_quantity)
            prior = pending.get(header.instrument_id)
            pending[header.instrument_id] = prior.merge(bar) if prior else bar
        # Commit only after every event validates; a failed command projection publishes nothing.
        totals = {
            instrument: self.totals[instrument].merge(bar) if instrument in self.totals else bar
            for instrument, bar in pending.items()
        }
        for instrument, bar in pending.items():
            self.commands[instrument][sequence] = bar
        self.totals.update(totals)
        self.sequence = sequence

    @classmethod
    def recover(cls, sequence: int, trades: Iterable[TradeEvent]) -> Projection:
        result = cls()
        grouped: dict[int, list[TradeEvent]] = {}
        previous = (0, -1)
        for trade in trades:
            key = (trade.header.command_sequence, trade.header.event_index)
            if not 1 <= key[0] <= sequence or key <= previous:
                raise ValueError("Recovered trades are outside the verified prefix or unordered.")
            grouped.setdefault(key[0], []).append(trade)
            previous = key
        for command_sequence in range(1, sequence + 1):
            result.apply_command(command_sequence, grouped.get(command_sequence, ()))
        return result

    def summary(self, instrument: int, name: str, active_count: int) -> InstrumentSummary:
        bar = self.totals.get(instrument)
        return InstrumentSummary(
            instrument_id=str(instrument),
            name=name,
            last_price=str(bar.close) if bar else None,
            first_price=str(bar.open) if bar else None,
            change=str(bar.close - bar.open) if bar else None,
            high=str(bar.high) if bar else None,
            low=str(bar.low) if bar else None,
            volume=str(bar.volume) if bar else "0",
            active_count=active_count,
        )

    def price(self, instrument: int, visible_range: ChartRange) -> tuple[int, int, list[Candle]]:
        if self.sequence == 0:
            return 1, 0, []
        start = 1 if visible_range == "all" else max(1, self.sequence - int(visible_range) + 1)
        bucket_size = next(
            size
            for size in (1, 5, 20, 100)
            if (self.sequence - 1) // size - (start - 1) // size + 1 <= 500
        )
        aligned_start = (start - 1) // bucket_size * bucket_size + 1
        candles = []
        commands = self.commands[instrument]
        for first in range(aligned_start, self.sequence + 1, bucket_size):
            last = min(first + bucket_size - 1, self.sequence)
            aggregate: Bar | None = None
            for sequence in range(first, last + 1):
                bar = commands.get(sequence)
                if bar:
                    aggregate = aggregate.merge(bar) if aggregate else bar
            candles.append(
                Candle(
                    start_sequence=str(first),
                    end_sequence=str(last),
                    open=str(aggregate.open) if aggregate else None,
                    high=str(aggregate.high) if aggregate else None,
                    low=str(aggregate.low) if aggregate else None,
                    close=str(aggregate.close) if aggregate else None,
                    volume=str(aggregate.volume) if aggregate else "0",
                )
            )
        return bucket_size, aligned_start, candles


def depth_points(levels: Sequence[PriceLevelSnapshot]) -> list[DepthPoint]:
    """Snapshot levels arrive best-first; every level contributes exactly once."""
    cumulative = 0
    result = []
    for level in levels:
        cumulative += level.aggregate_quantity
        result.append(
            DepthPoint(
                price=str(level.price),
                quantity=str(level.aggregate_quantity),
                cumulative=str(cumulative),
            )
        )
    return result
