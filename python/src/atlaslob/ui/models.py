"""Versioned HTTP values. Engine integers cross JSON as decimal strings."""

from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field

Decimal = Annotated[str, Field(pattern=r"^(0|[1-9][0-9]*)$", max_length=20)]
SignedDecimal = Annotated[str, Field(pattern=r"^-?(0|[1-9][0-9]*)$", max_length=20)]
AggregateDecimal = Annotated[str, Field(pattern=r"^(0|[1-9][0-9]*)$")]
AggregateSignedDecimal = Annotated[str, Field(pattern=r"^-?(0|[1-9][0-9]*)$")]
ChartKind = Literal["price", "depth"]
ChartRange = Literal["100", "500", "all"]
Mode = Literal["manual", "scenario"]


class Model(BaseModel):
    model_config = ConfigDict(
        extra="forbid", frozen=True, json_schema_serialization_defaults_required=True
    )


class Guard(Model):
    activation: str | None


class NewCommand(Guard):
    kind: Literal["new"]
    instrument_id: Decimal
    client_id: Decimal
    side: Literal["buy", "sell"]
    order_type: Literal["limit_gtc", "limit_ioc", "market_ioc"]
    quantity: Decimal
    price: SignedDecimal | None = None


class CancelCommand(Guard):
    kind: Literal["cancel"]
    instrument_id: Decimal
    order_id: Decimal


class ReplaceCommand(Guard):
    kind: Literal["replace"]
    instrument_id: Decimal
    order_id: Decimal
    price: SignedDecimal
    quantity: Decimal


CommandRequest = Annotated[NewCommand | CancelCommand | ReplaceCommand, Field(discriminator="kind")]


class CreateSession(Guard):
    name: str = Field(min_length=1, max_length=80)
    mode: Mode
    scenario_id: str | None = None
    restart: bool = False


class RenameSession(Model):
    name: str = Field(min_length=1, max_length=80)


class PlaybackRequest(Guard):
    action: Literal["step", "play", "pause", "speed"]
    speed: Literal[1, 2, 5, 10] | None = None


class SessionInfo(Model):
    id: str
    name: str
    mode: Mode
    created_at: str
    updated_at: str
    scenario_id: str | None = None
    error: str | None = None


class SessionList(Model):
    sessions: list[SessionInfo]
    last_opened: str | None


class ScenarioInfo(Model):
    id: str
    name: str
    description: str
    step_count: int


class OrderView(Model):
    order_id: Decimal
    client_id: Decimal
    instrument_id: Decimal
    side: Literal["buy", "sell"]
    price: SignedDecimal
    quantity: Decimal
    priority: Decimal


class LevelView(Model):
    price: SignedDecimal
    quantity: AggregateDecimal
    count: int


class InstrumentSummary(Model):
    instrument_id: Decimal
    name: str
    last_price: SignedDecimal | None = None
    first_price: SignedDecimal | None = None
    change: AggregateSignedDecimal | None = None
    high: SignedDecimal | None = None
    low: SignedDecimal | None = None
    volume: AggregateDecimal = "0"
    active_count: int = 0


class Candle(Model):
    start_sequence: Decimal
    end_sequence: Decimal
    open: SignedDecimal | None = None
    high: SignedDecimal | None = None
    low: SignedDecimal | None = None
    close: SignedDecimal | None = None
    volume: AggregateDecimal = "0"


class DepthPoint(Model):
    price: SignedDecimal
    quantity: AggregateDecimal
    cumulative: AggregateDecimal


class ChartView(Model):
    instance_id: str
    activation: str | None
    revision: int
    sequence: Decimal
    instrument_id: Decimal
    kind: ChartKind
    range: ChartRange
    bucket_size: int = 1
    start_sequence: Decimal = "0"
    end_sequence: Decimal = "0"
    candles: list[Candle] = Field(default_factory=list)
    bids: list[DepthPoint] = Field(default_factory=list)
    asks: list[DepthPoint] = Field(default_factory=list)


class EventView(Model):
    id: str
    sequence: Decimal
    instrument_id: Decimal
    kind: str
    description: str
    details: dict[str, str]
    command: dict[str, str]


class TradeView(Model):
    id: str
    sequence: Decimal
    instrument_id: Decimal
    side: Literal["buy", "sell"]
    price: SignedDecimal
    quantity: Decimal
    aggressor_id: Decimal
    resting_id: Decimal


class PlaybackView(Model):
    playing: bool = False
    speed: int = 2
    cursor: int = 0
    total: int = 0
    next_description: str | None = None
    scenario_name: str | None = None


class View(Model):
    instance_id: str
    activation: str | None
    revision: int
    chart_revision: int = 0
    instruments: list[InstrumentSummary] = Field(default_factory=list)
    session: SessionInfo | None
    instrument_id: Decimal
    sequence: Decimal = "0"
    saved_sequence: Decimal = "0"
    active_count: int = 0
    total_active_count: int = 0
    bids: list[LevelView] = Field(default_factory=list)
    asks: list[LevelView] = Field(default_factory=list)
    bid_levels: int = 0
    ask_levels: int = 0
    spread: AggregateSignedDecimal | None = None
    orders: list[OrderView] = Field(default_factory=list)
    orders_page: int = 1
    orders_pages: int = 1
    events: list[EventView] = Field(default_factory=list)
    trades: list[TradeView] = Field(default_factory=list)
    activity_marker: str = "Activity since this session was opened"
    playback: PlaybackView = Field(default_factory=PlaybackView)
    fault: str | None = None
    command_limit_reached: bool = False


class ActionResult(Model):
    message: str
    outcome: Literal["ok", "committed", "rejected", "fault"] = "ok"
    assigned_order_id: Decimal | None = None
    view: View


class Status(Model):
    ready: bool
    version: str
    semantics_version: int
    active_session: str | None
    activation: str | None
    error: str | None


class ErrorResponse(Model):
    detail: str
