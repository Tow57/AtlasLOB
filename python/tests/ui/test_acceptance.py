"""Additional capacity, projection, failure and scheduling acceptance tests."""

from __future__ import annotations

import asyncio
import time
from pathlib import Path
from typing import Any

import pytest

pytest.importorskip("fastapi")
from atlaslob.ui.models import (
    CancelCommand,
    CreateSession,
    NewCommand,
    PlaybackRequest,
    ReplaceCommand,
)
from atlaslob.ui.service import Controller, Service, ServiceError


def fresh(service: Service) -> None:
    service.create(CreateSession(activation=service.activation, name="Acceptance", mode="manual"))


def order(service: Service, **overrides: Any) -> Any:
    values = dict(
        activation=service.activation,
        kind="new",
        instrument_id="7",
        client_id="1",
        side="buy",
        order_type="limit_gtc",
        quantity="5",
        price="100",
    )
    return service.command(NewCommand.model_validate({**values, **overrides}))


def test_first_demo_ids_and_cached_projection(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    service = Service(tmp_path)
    try:
        result = order(service, price="98")
        assert result.assigned_order_id == "25"
        assert result.outcome == "committed"
        cached = service.view()

        def forbidden(*args: Any, **kwargs: Any) -> None:
            raise AssertionError("An unchanged poll must not invoke the native engine")

        monkeypatch.setattr(service.engine_type, "snapshot", forbidden)
        monkeypatch.setattr(service.engine_type, "state_digest", forbidden)
        assert service.view() is cached
        assert service.view(42).active_count == 12
    finally:
        service.close()


def test_native_matching_and_display_projection(tmp_path: Path) -> None:
    service = Service(tmp_path)
    try:
        fresh(service)
        order(service, side="sell", price="101", client_id="2", quantity="6")
        order(service, side="sell", price="101", client_id="3", quantity="8")
        result = order(service, price="102", quantity="9")
        assert [trade.resting_id for trade in reversed(result.view.trades)] == ["1", "2"]
        assert all(trade.price == "101" for trade in result.view.trades)
        assert result.view.asks[0].quantity == "5"
        assert result.view.orders[0].priority == "2"
        # Replace uses the actual client 3, not the order-entry client's selection.
        replacement = service.command(
            ReplaceCommand(
                activation=service.activation,
                kind="replace",
                instrument_id="7",
                order_id="2",
                price="103",
                quantity="7",
            )
        )
        assert replacement.view.orders[0].client_id == "3"
        assert replacement.view.orders[0].priority == "4"
        assert replacement.assigned_order_id == "4"
        order(service, price="103", quantity="2", order_type="limit_ioc")
        view = service.view()
        native = service.engine.snapshot(7)
        assert [(level.price, level.quantity) for level in view.asks] == [
            (str(level.price), str(level.aggregate_quantity)) for level in native.asks
        ]
        assert [(row.order_id, row.quantity) for row in view.orders] == [
            (str(o.order_id), str(o.remaining_quantity))
            for level in native.asks
            for o in level.orders
        ]
        canceled = service.command(
            CancelCommand(
                activation=service.activation, kind="cancel", instrument_id="7", order_id="4"
            )
        )
        assert not canceled.view.orders
    finally:
        service.close()


def test_metadata_failure_stops_mutations_and_strict_reopen_recovers_commit(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    service = Service(tmp_path)
    try:
        fresh(service)
        assert service.meta is not None
        sid = service.meta["id"]
        original = service.store.write

        def fail(value: Any) -> None:
            raise OSError("injected disk failure")

        monkeypatch.setattr(service.store, "write", fail)
        failed = order(service)
        assert failed.outcome == "fault"
        assert failed.view.saved_sequence == "1"  # Native sync succeeded before metadata failed.
        with pytest.raises(ServiceError, match="Mutations are stopped"):
            order(service)
        monkeypatch.setattr(service.store, "write", original)
        reopened = service.open(sid, service.activation)
        assert reopened.view.sequence == "1"
        assert reopened.view.orders[0].quantity == "5"
        assert not reopened.view.fault
    finally:
        service.close()


def test_capacity_pagination_and_responsiveness(tmp_path: Path) -> None:
    service = Service(tmp_path)
    try:
        fresh(service)
        for index in range(1_000):
            assert order(service, price=str(1_000 + index % 60)).outcome == "committed"
        view = service.view()
        assert view.total_active_count == 1_000
        assert len(view.orders) == 50 and view.orders_pages == 20
        assert len(service.view(depth=50).bids) == 50
        assert len(service.view(depth=10).bids) == 10
        assert len(service.view(page=20).orders) == 50
        assert len(view.events) == 200
        started = time.perf_counter()
        rejected = order(service)
        elapsed = time.perf_counter() - started
        assert rejected.outcome == "rejected"
        assert rejected.view.sequence == "1001"
        assert rejected.view.total_active_count == 1_000
        print(f"\n1,000-order service feedback: {elapsed * 1000:.1f} ms")
        assert elapsed < 0.5
        assert order(service, instrument_id="42").outcome == "committed"
    finally:
        service.close()


def test_command_limit_inspectable_and_scenario_hash_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    import atlaslob.ui.service as implementation

    service = Service(tmp_path)
    try:
        fresh(service)
        monkeypatch.setattr(implementation, "COMMAND_LIMIT", 2)
        order(service)
        order(service, quantity="0")
        assert service.view().command_limit_reached
        with pytest.raises(ServiceError, match="10,000-command limit"):
            order(service)
        monkeypatch.setattr(implementation, "COMMAND_LIMIT", 10_000)
        service.create(
            CreateSession(
                activation=service.activation, name="Hash", mode="scenario", scenario_id="fifo"
            )
        )
        assert service.meta is not None
        sid = service.meta["id"]
        meta = service.store.read(sid)
        meta["scenario"]["payload"]["steps"][0]["command"]["quantity"] = "999"
        service.store.write(meta)
        log = service.store.directory(sid) / "commands.atlaslog"
        before = log.read_bytes()
        with pytest.raises(ServiceError, match="hash/version"):
            service.open(sid, service.activation)
        assert log.read_bytes() == before
    finally:
        service.close()


def test_slow_execution_does_not_catch_up_and_pause_is_serialized(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    async def check() -> None:
        controller = Controller(tmp_path)
        await controller.start()
        try:
            service = controller.service
            assert service is not None
            await controller.call(
                "create",
                CreateSession(
                    activation=service.activation, name="Slow", mode="scenario", scenario_id="fifo"
                ),
            )
            original = service.engine_type.submit
            starts: list[float] = []

            def slow(engine: Any, command: Any) -> Any:
                starts.append(time.monotonic())
                time.sleep(0.12)
                return original(engine, command)

            monkeypatch.setattr(service.engine_type, "submit", slow)
            await controller.call(
                "playback", PlaybackRequest(activation=service.activation, action="speed", speed=10)
            )
            await controller.call(
                "playback", PlaybackRequest(activation=service.activation, action="play")
            )
            await asyncio.sleep(0.6)
            paused = await controller.call(
                "playback", PlaybackRequest(activation=service.activation, action="pause")
            )
            await asyncio.sleep(0.2)
            after = await controller.call("view")
            assert after.sequence == paused.view.sequence
            assert len(starts) >= 2
            assert all(b - a >= 0.21 for a, b in zip(starts, starts[1:], strict=False))
        finally:
            await controller.close()

    asyncio.run(check())
