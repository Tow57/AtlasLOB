"""Serialized native engine ownership and cached, bounded presentation state."""

from __future__ import annotations

import asyncio
import copy
import time
import uuid
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from dataclasses import fields
from datetime import UTC, datetime
from enum import IntEnum
from importlib.metadata import version
from pathlib import Path
from typing import Any, Literal

from atlaslob.domain import (
    ATLASLOB_SEMANTICS_VERSION,
    I64_MAX,
    I64_MIN,
    U64_MAX,
    CancelOrder,
    Command,
    InstrumentConfig,
    MatchingConfig,
    NewOrder,
    RejectedEvent,
    ReplaceOrder,
    TradeEvent,
)
from atlaslob.ui.charts import Projection, depth_points
from atlaslob.ui.models import (
    ActionResult,
    CancelCommand,
    ChartKind,
    ChartRange,
    ChartView,
    CreateSession,
    EventView,
    InstrumentSummary,
    LevelView,
    NewCommand,
    OrderView,
    PlaybackRequest,
    PlaybackView,
    ReplaceCommand,
    SessionInfo,
    SessionList,
    Status,
    TradeView,
    View,
)
from atlaslob.ui.scenarios import SCENARIOS, decode, digest, encode, first_demo
from atlaslob.ui.store import Store

COMMAND_LIMIT = 10_000
Outcome = Literal["committed", "rejected", "fault"]
CATALOG = tuple(InstrumentConfig(i, MatchingConfig(max_active_orders=1_000)) for i in (7, 42))


class ServiceError(Exception):
    def __init__(self, status: int, detail: str):
        self.status = status
        self.detail = detail
        super().__init__(detail)


def now() -> str:
    return datetime.now(UTC).isoformat()


def bounded(value: str, name: str, low: int = 0, high: int = U64_MAX) -> int:
    number = int(value)
    if not low <= number <= high:
        raise ServiceError(422, f"{name} must be between {low} and {high}.")
    return number


def readable(value: Any) -> str:
    if isinstance(value, IntEnum):
        return value.name.replace("_", " ").lower()
    return str(value)


class Service:
    """Construct and call ONLY on Controller's single executor thread."""

    def __init__(self, data_dir: Path):
        from atlaslob import Engine

        self.engine_type = Engine
        self.instance_id = uuid.uuid4().hex
        self.store = Store(data_dir)
        self.engine: Any = None
        self.snapshot: Any = None
        self.meta: dict[str, Any] | None = None
        self.activation: str | None = None
        self.revision = 0
        self.chart_revision = 0
        self.projection = Projection()
        self.chart_fault: str | None = None
        self.chart_cache: dict[tuple[int, ChartKind, ChartRange], ChartView] = {}
        self.cached_chart_revision = -1
        self.saved_sequence = 0
        self.next_id = 1
        self.fault: str | None = None
        self.errors: dict[str, str] = {}
        self.playing = False
        self.speed = 2
        self.next_due = 0.0
        self.events: dict[int, deque[EventView]] = {i: deque(maxlen=200) for i in (7, 42)}
        self.trades: dict[int, deque[TradeView]] = {i: deque(maxlen=100) for i in (7, 42)}
        self.marker = "Activity since this session was opened"
        self.view_cache: dict[tuple[int, int, int], View] = {}
        self.cached_revision = -1
        if not self.store.ids() and not (self.store.root / "library.json").exists():
            try:
                self.create(CreateSession(activation=None, name="First demo", mode="manual"))
                for command in first_demo():
                    if self.fault:
                        break
                    self._execute(command)
            except Exception:
                self.close()
                raise

    def _guard(self, activation: str | None) -> None:
        if activation != self.activation:
            raise ServiceError(
                409, "This session activation has changed. Refresh before continuing."
            )

    def _mutable(self) -> None:
        if self.engine is None or self.meta is None:
            raise ServiceError(409, "Open or create a session first.")
        if self.fault:
            raise ServiceError(409, "Mutations are stopped. Reopen through strict recovery.")
        if self.snapshot.last_sequence >= COMMAND_LIMIT:
            raise ServiceError(409, "The 10,000-command limit is reached. Start a new session.")

    def _info(self, meta: dict[str, Any]) -> SessionInfo:
        return SessionInfo.model_validate(
            {
                **{
                    k: meta.get(k)
                    for k in ("id", "name", "mode", "created_at", "updated_at", "scenario_id")
                },
                "error": self.errors.get(meta["id"]),
            }
        )

    def sessions(self) -> SessionList:
        entries = []
        for sid in self.store.ids():
            try:
                entries.append(self._info(self.store.read(sid)))
            except (ValueError, OSError, KeyError) as exc:
                entries.append(
                    SessionInfo(
                        id=sid,
                        name="Unavailable session",
                        mode="manual",
                        created_at="",
                        updated_at="",
                        error=f"Metadata cannot be read: {exc}",
                    )
                )
        entries.sort(key=lambda e: e.updated_at, reverse=True)
        return SessionList(sessions=entries, last_opened=self.store.last_opened())

    def _release(self) -> None:
        self.playing = False
        self.engine = None  # Native owner destruction closes its log on this thread.
        self.snapshot = None
        self.meta = None
        self.activation = None
        self.projection = Projection()
        self.chart_fault = None
        self.chart_revision += 1
        self.revision += 1

    def _activate(self, meta: dict[str, Any], *, reopened: bool) -> None:
        self.meta = meta
        self.snapshot = self.engine.snapshot()
        if (
            self.snapshot.catalog != CATALOG
            or self.snapshot.engine_config.max_total_active_orders != 2_000
        ):
            raise ValueError("Session has an incompatible instrument catalog or capacity.")
        if self.snapshot.last_sequence > COMMAND_LIMIT:
            raise ValueError("Session exceeds the supported command limit.")
        if meta["mode"] == "scenario" and self.snapshot.last_sequence > len(
            meta["scenario"]["payload"]["steps"]
        ):
            raise ValueError("Recovered sequence exceeds the frozen scenario length.")
        # The native API only publishes collected trades after complete strict verification.
        # Build the entire projection before making this activation visible.
        self.projection = Projection.recover(
            self.snapshot.last_sequence, self.engine.recovered_trades if reopened else ()
        )
        self._instrument_summaries()  # Validate display metadata before publishing activation.
        self.chart_fault = None
        self.chart_revision += 1
        self.activation = uuid.uuid4().hex
        self.fault = None
        self.playing = False
        self.speed = 2
        self.saved_sequence = self.snapshot.last_sequence
        self.next_id = (
            max(
                [self.saved_sequence]
                + [
                    o.order_id
                    for b in self.snapshot.instruments
                    for level in (*b.bids, *b.asks)
                    for o in level.orders
                ]
            )
            + 1
        )
        for buffer in self.events.values():
            buffer.clear()
        for trades in self.trades.values():
            trades.clear()
        self.marker = (
            "Session reopened · Activity since this session was opened"
            if reopened
            else "Activity since this session was opened"
        )
        self.revision += 1
        self.store.mark_opened(meta["id"])

    def create(self, request: CreateSession) -> ActionResult:
        self._guard(request.activation)
        name = request.name.strip()
        if not name:
            raise ServiceError(422, "Session name cannot be blank.")
        frozen = None
        if request.mode == "scenario":
            if request.restart:
                if not self.meta or self.meta["mode"] != "scenario":
                    raise ServiceError(409, "Only an active scenario can be restarted.")
                frozen = copy.deepcopy(self.meta["scenario"])
            else:
                scenario = next((s for s in SCENARIOS if s.id == request.scenario_id), None)
                if scenario is None:
                    raise ServiceError(422, "Choose a built-in scenario.")
                frozen = scenario.freeze()
        elif request.restart or request.scenario_id is not None:
            raise ServiceError(422, "Manual sessions cannot specify a scenario or restart.")
        sid = uuid.uuid4().hex
        meta = {
            "schema": 1,
            "id": sid,
            "name": name,
            "mode": request.mode,
            "created_at": now(),
            "updated_at": now(),
            "instruments": [
                {"id": "7", "name": "DEMO-A", "tick": "1"},
                {"id": "42", "name": "DEMO-B", "tick": "1"},
            ],
            "scenario_id": frozen["payload"]["id"] if frozen else None,
            "scenario": frozen,
        }
        self._release()
        try:
            directory = self.store.directory(sid)
            directory.mkdir()
            self.store.write(meta)
            self.engine = self.engine_type.create_logged(
                directory / "commands.atlaslog",
                CATALOG,
                max_total_active_orders=2_000,
                durability="sync_each_record",
            )
            self._activate(meta, reopened=False)
        except Exception as exc:
            self._release()
            self.fault = f"Session creation failed: {exc}"
            self.errors[sid] = self.fault
            raise ServiceError(503, self.fault) from exc
        return ActionResult(message=f"Created {name}.", view=self.view())

    def open(self, sid: str, activation: str | None) -> ActionResult:
        self._guard(activation)
        self._release()
        try:
            meta = self.store.read(sid)
            self._info(meta)
            log = self.store.directory(sid) / "commands.atlaslog"
            if log.is_symlink() or not log.is_file():
                raise ValueError("Native command log is missing or is a symbolic link.")
            if meta["mode"] == "scenario":
                frozen = meta["scenario"]
                payload = frozen["payload"]
                if payload["version"] != 1 or digest(payload) != frozen["sha256"]:
                    raise ValueError("Frozen scenario content hash/version is invalid.")
                if not 1 <= len(payload["steps"]) <= COMMAND_LIMIT:
                    raise ValueError("Frozen scenario has an invalid step count.")
                for step in payload["steps"]:
                    decode(step["command"])
            self.engine = self.engine_type.recover(
                log,
                mode="verify",
                tail_policy="strict",
                durability="sync_each_record",
                collect_trades=True,
                max_recovered_trades=10_000,
            )
            self._activate(meta, reopened=True)
            self.errors.pop(sid, None)
        except Exception as exc:
            self._release()
            self.fault = f"Strict recovery failed; files were preserved: {exc}"
            self.errors[sid] = self.fault
            raise ServiceError(409, self.fault) from exc
        return ActionResult(message="Session reopened. Activity starts here.", view=self.view())

    def rename(self, sid: str, name: str) -> SessionInfo:
        if not name.strip():
            raise ServiceError(422, "Session name cannot be blank.")
        try:
            meta = self.store.read(sid)
            meta["name"] = name.strip()
            self.store.write(meta)
        except (ValueError, OSError) as exc:
            raise ServiceError(409, f"Could not rename session: {exc}") from exc
        if self.meta and self.meta["id"] == sid:
            self.meta = meta
            self.revision += 1
        return self._info(meta)

    def _save_metadata(self) -> None:
        assert self.meta is not None
        self.meta["updated_at"] = now()
        self.store.write(self.meta)

    def _execute(self, command: Command) -> tuple[Outcome, str]:
        self._mutable()
        try:
            result = self.engine.submit(command)
            if result.error is not None:
                raise RuntimeError(readable(result.error))
            self.saved_sequence = result.command_sequence
            self.snapshot = self.engine.snapshot()
            try:
                self.projection.apply_command(result.command_sequence, result.events)
            except Exception as exc:
                self.chart_fault = (
                    f"Chart projection failed: {exc}. Reopen through strict recovery."
                )
                self.chart_revision += 1
                raise
            self.chart_revision += 1
            self.next_id = max(
                self.next_id,
                self.snapshot.last_sequence + 1,
                getattr(command, "order_id", 0) + 1,
                getattr(command, "new_order_id", 0) + 1,
            )
            command_details = {
                k: "none" if v is None else str(v) for k, v in encode(command).items()
            }
            reason = ""
            for event in result.events:
                header = event.header
                eid = f"{header.command_sequence}:{header.event_index}"
                kind = type(event).__name__.removesuffix("Event")
                detail = {
                    f.name: readable(getattr(event, f.name))
                    for f in fields(event)
                    if f.name != "header"
                }
                description = ", ".join(f"{k.replace('_', ' ')}: {v}" for k, v in detail.items())
                if isinstance(event, RejectedEvent):
                    reason = readable(event.reason)
                    description = f"Rejected: {reason}. This command consumed a sequence."
                if isinstance(event, TradeEvent):
                    description = (
                        f"Order {event.aggressor_order_id} matched {event.execution_quantity} "
                        f"at {event.execution_price} with resting order {event.resting_order_id}."
                    )
                    self.trades[header.instrument_id].appendleft(
                        TradeView(
                            id=eid,
                            sequence=str(header.command_sequence),
                            instrument_id=str(header.instrument_id),
                            side="buy" if event.aggressor_side == 1 else "sell",
                            price=str(event.execution_price),
                            quantity=str(event.execution_quantity),
                            aggressor_id=str(event.aggressor_order_id),
                            resting_id=str(event.resting_order_id),
                        )
                    )
                self.events[header.instrument_id].appendleft(
                    EventView(
                        id=eid,
                        sequence=str(header.command_sequence),
                        instrument_id=str(header.instrument_id),
                        kind=kind,
                        description=description,
                        details=detail,
                        command=command_details,
                    )
                )
            self._save_metadata()
            self.revision += 1
            return (
                ("rejected", f"Rejected: {reason}")
                if result.rejected
                else ("committed", "Command processed.")
            )
        except Exception as exc:
            self.fault = (
                f"Engine, persistence, or chart projection failure: {exc}. "
                "Do not resubmit this command. "
                "Reopen the session through strict recovery to establish what committed."
            )
            self.playing = False
            self.revision += 1
            return "fault", self.fault

    def command(self, request: NewCommand | CancelCommand | ReplaceCommand) -> ActionResult:
        self._guard(request.activation)
        self._mutable()
        assert self.meta is not None
        if self.meta["mode"] != "manual":
            raise ServiceError(
                409, "Scenario sessions do not allow manual mutations, even when paused."
            )
        iid = bounded(request.instrument_id, "Instrument ID", 1, 2**32 - 1)
        if iid not in (7, 42):
            raise ServiceError(422, "Select DEMO-A (7) or DEMO-B (42).")
        assigned = None
        command: Command
        if isinstance(request, NewCommand):
            client = bounded(request.client_id, "Demo client", 1, 3)
            quantity = bounded(request.quantity, "Quantity")
            market = request.order_type == "market_ioc"
            if market and request.price is not None:
                raise ServiceError(422, "Market IOC commands must omit price.")
            if not market and request.price is None:
                raise ServiceError(422, "Limit orders require a price in integer ticks.")
            price = (
                None if request.price is None else bounded(request.price, "Price", I64_MIN, I64_MAX)
            )
            assigned = self.next_id
            command = NewOrder(
                client,
                assigned,
                iid,
                1 if request.side == "buy" else 2,
                2 if market else 1,
                1 if request.order_type == "limit_gtc" else 2,
                price,
                quantity,
            )
        else:
            oid = bounded(request.order_id, "Order ID")
            order = next(
                (
                    o
                    for b in self.snapshot.instruments
                    if b.instrument_id == iid
                    for level in (*b.bids, *b.asks)
                    for o in level.orders
                    if o.order_id == oid
                ),
                None,
            )
            if order is None:
                raise ServiceError(
                    409, "This order is no longer open. Refresh the authoritative book."
                )
            if isinstance(request, CancelCommand):
                command = CancelOrder(order.client_id, oid, iid)
            else:
                price = bounded(request.price, "Price", I64_MIN, I64_MAX)
                quantity = bounded(request.quantity, "Quantity")
                assigned = self.next_id
                command = ReplaceOrder(order.client_id, oid, assigned, iid, price, quantity)
        if assigned is not None:
            if assigned > U64_MAX:
                raise ServiceError(409, "Order ID space exhausted. Create another session.")
            self.next_id += 1
        outcome, message = self._execute(command)
        if assigned is not None:
            message += f" Assigned order ID {assigned}."
        return ActionResult(
            message=message,
            outcome=outcome,
            assigned_order_id=str(assigned) if assigned is not None else None,
            view=self.view(instrument=iid),
        )

    def _steps(self) -> list[dict[str, Any]]:
        return (
            self.meta["scenario"]["payload"]["steps"]
            if self.meta and self.meta["mode"] == "scenario"
            else []
        )

    def playback(self, request: PlaybackRequest) -> ActionResult:
        self._guard(request.activation)
        if not self.meta or self.meta["mode"] != "scenario":
            raise ServiceError(409, "Open a scenario to use playback.")
        if request.action == "pause":
            self.playing = False
        elif request.action == "speed":
            if request.speed is None:
                raise ServiceError(422, "Select a playback speed.")
            self.speed = request.speed
            self.next_due = time.monotonic() + 1 / self.speed
        else:
            self._mutable()
            if self.snapshot.last_sequence >= len(self._steps()):
                raise ServiceError(409, "Scenario complete. Restart to create a fresh saved run.")
            if request.action == "step":
                if self.playing:
                    raise ServiceError(409, "Pause before stepping.")
                outcome, message = self._step()
                return ActionResult(message=message, outcome=outcome, view=self.view())
            self.playing = True
            self.next_due = time.monotonic() + 1 / self.speed
        self.revision += 1
        return ActionResult(
            message=f"Playback {request.action} confirmed at step {self.snapshot.last_sequence}.",
            view=self.view(),
        )

    def _step(self) -> tuple[Outcome, str]:
        command = decode(self._steps()[self.snapshot.last_sequence]["command"])
        result = self._execute(command)
        if self.snapshot.last_sequence >= len(self._steps()) or self.fault:
            self.playing = False
        self.next_due = time.monotonic() + 1 / self.speed
        return result

    def tick(self) -> None:
        if self.playing and time.monotonic() >= self.next_due:
            self._step()

    def _instrument_summaries(self) -> list[InstrumentSummary]:
        names = (
            {int(item["id"]): item["name"] for item in self.meta["instruments"]}
            if self.meta
            else {7: "DEMO-A", 42: "DEMO-B"}
        )
        counts = (
            {book.instrument_id: book.active_order_count for book in self.snapshot.instruments}
            if self.snapshot
            else {}
        )
        return [
            self.projection.summary(instrument, names[instrument], counts.get(instrument, 0))
            for instrument in (7, 42)
        ]

    def chart(
        self, instrument: int = 7, kind: ChartKind = "price", visible_range: ChartRange = "500"
    ) -> ChartView:
        if (
            instrument not in (7, 42)
            or kind not in ("price", "depth")
            or visible_range not in ("100", "500", "all")
        ):
            raise ServiceError(422, "Invalid chart instrument, kind, or range.")
        if self.chart_fault:
            raise ServiceError(503, self.chart_fault)
        if self.cached_chart_revision != self.chart_revision:
            self.chart_cache.clear()
            self.cached_chart_revision = self.chart_revision
        key = (instrument, kind, visible_range)
        if key in self.chart_cache:
            return self.chart_cache[key]
        sequence = self.snapshot.last_sequence if self.snapshot else 0
        values: dict[str, Any] = dict(
            instance_id=self.instance_id,
            activation=self.activation,
            revision=self.chart_revision,
            sequence=str(sequence),
            instrument_id=str(instrument),
            kind=kind,
            range=visible_range,
            end_sequence=str(sequence),
        )
        if kind == "price":
            bucket_size, start, candles = self.projection.price(instrument, visible_range)
            values.update(bucket_size=bucket_size, start_sequence=str(start), candles=candles)
        elif self.snapshot:
            book = next(b for b in self.snapshot.instruments if b.instrument_id == instrument)
            values.update(
                start_sequence=str(sequence),
                bids=depth_points(book.bids),
                asks=depth_points(book.asks),
            )
        result = ChartView(**values)
        self.chart_cache[key] = result
        return result

    def view(self, instrument: int = 7, depth: int = 20, page: int = 1) -> View:
        if instrument not in (7, 42) or depth not in (10, 20, 50) or page < 1:
            raise ServiceError(422, "Invalid instrument, depth, or page.")
        if self.cached_revision != self.revision:
            self.view_cache.clear()
            self.cached_revision = self.revision
        key = (instrument, depth, page)
        if key in self.view_cache:
            return self.view_cache[key]
        base: dict[str, Any] = dict(
            instance_id=self.instance_id,
            activation=self.activation,
            revision=self.revision,
            chart_revision=self.chart_revision,
            instruments=self._instrument_summaries(),
            session=self._info(self.meta) if self.meta else None,
            instrument_id=str(instrument),
            fault=self.fault,
        )
        if self.snapshot is None:
            return View(**base)
        assert self.meta is not None
        book = next(b for b in self.snapshot.instruments if b.instrument_id == instrument)
        orders = sorted(
            (o for level in (*book.bids, *book.asks) for o in level.orders),
            key=lambda o: (o.priority_sequence, o.order_id),
        )
        pages = max(1, (len(orders) + 49) // 50)
        page = min(page, pages)
        steps = self._steps()
        cursor = self.snapshot.last_sequence if steps else 0
        result = View(
            **base,
            sequence=str(self.snapshot.last_sequence),
            saved_sequence=str(self.saved_sequence),
            active_count=book.active_order_count,
            total_active_count=self.snapshot.active_order_count,
            bids=[
                LevelView(
                    price=str(level.price),
                    quantity=str(level.aggregate_quantity),
                    count=len(level.orders),
                )
                for level in book.bids[:depth]
            ],
            asks=[
                LevelView(
                    price=str(level.price),
                    quantity=str(level.aggregate_quantity),
                    count=len(level.orders),
                )
                for level in book.asks[:depth]
            ],
            bid_levels=len(book.bids),
            ask_levels=len(book.asks),
            spread=str(book.asks[0].price - book.bids[0].price)
            if book.asks and book.bids
            else None,
            orders=[
                OrderView(
                    order_id=str(o.order_id),
                    client_id=str(o.client_id),
                    instrument_id=str(instrument),
                    side="buy" if o.side == 1 else "sell",
                    price=str(o.price),
                    quantity=str(o.remaining_quantity),
                    priority=str(o.priority_sequence),
                )
                for o in orders[(page - 1) * 50 : page * 50]
            ],
            orders_page=page,
            orders_pages=pages,
            events=list(self.events[instrument]),
            trades=list(self.trades[instrument]),
            activity_marker=self.marker,
            playback=PlaybackView(
                playing=self.playing,
                speed=self.speed,
                cursor=cursor,
                total=len(steps),
                next_description=steps[cursor]["description"] if cursor < len(steps) else None,
                scenario_name=self.meta["scenario"]["payload"]["name"] if steps else None,
            ),
            command_limit_reached=self.snapshot.last_sequence >= COMMAND_LIMIT,
        )
        self.view_cache[key] = result
        return result

    def status(self) -> Status:
        return Status(
            ready=self.fault is None,
            version=version("atlaslob"),
            semantics_version=ATLASLOB_SEMANTICS_VERSION,
            active_session=self.meta["id"] if self.meta else None,
            activation=self.activation,
            error=self.fault,
        )

    def close(self) -> None:
        self._release()
        self.store.close()


class Controller:
    """The executor queue is the serialization boundary, including pause acknowledgements."""

    def __init__(self, data_dir: Path):
        self.data_dir = data_dir
        self.executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="atlaslob-owner")
        self.service: Service | None = None
        self.timer: asyncio.Task[None] | None = None

    async def start(self) -> None:
        loop = asyncio.get_running_loop()
        try:
            self.service = await loop.run_in_executor(self.executor, Service, self.data_dir)
        except Exception:
            self.executor.shutdown(wait=True)
            raise
        self.timer = asyncio.create_task(self._clock())

    async def call(self, method: str, *args: Any) -> Any:
        assert self.service is not None
        return await asyncio.get_running_loop().run_in_executor(
            self.executor, getattr(self.service, method), *args
        )

    async def _clock(self) -> None:
        while True:
            await asyncio.sleep(0.025)
            await self.call("tick")

    async def close(self) -> None:
        if self.timer:
            self.timer.cancel()
            try:
                await self.timer
            except asyncio.CancelledError:
                pass
        if self.service:
            await self.call("close")
        self.executor.shutdown(wait=True)
