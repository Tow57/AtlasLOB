"""Chart arithmetic tests and recovery/API tests against the compiled native boundary."""

from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

pytest.importorskip("fastapi")
from atlaslob.domain import I64_MAX, I64_MIN, U64_MAX, EventHeader, Side, TradeEvent
from atlaslob.ui.app import create_app
from atlaslob.ui.charts import Projection
from atlaslob.ui.models import ActionResult, CreateSession, NewCommand
from atlaslob.ui.service import Service, ServiceError
from fastapi.testclient import TestClient


def trade(sequence: int, index: int, price: int, quantity: int = 1, iid: int = 7) -> TradeEvent:
    return TradeEvent(
        EventHeader(sequence, index, iid), 2, 1, 1, 2, Side.BUY, price, quantity, 0, 0
    )


@pytest.fixture
def service(tmp_path: Path) -> Iterator[Service]:
    result = Service(tmp_path)
    result.create(CreateSession(activation=result.activation, mode="manual", name="Chart tests"))
    try:
        yield result
    finally:
        result.close()


def submit(service: Service, **overrides: Any) -> ActionResult:
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


def test_ordered_ohlc_empty_buckets_and_arbitrary_precision_volume() -> None:
    projection = Projection()
    projection.apply_command(1, [])
    projection.apply_command(
        2,
        [trade(2, 1, I64_MAX, U64_MAX), trade(2, 2, I64_MIN, U64_MAX)],
    )
    projection.apply_command(3, [trade(3, 1, -7, iid=42)])
    size, start, candles = projection.price(7, "500")
    assert (size, start) == (1, 1)
    assert candles[0].open is None and candles[0].volume == "0"
    assert candles[1].open == candles[1].high == str(I64_MAX)
    assert candles[1].close == candles[1].low == str(I64_MIN)
    assert candles[1].volume == str(2 * U64_MAX)
    assert candles[2].open is None
    summary = projection.summary(7, "DEMO-A", 4)
    assert summary.volume == str(2 * U64_MAX)
    assert summary.change == str(I64_MIN - I64_MAX)
    assert summary.active_count == 4
    assert projection.summary(42, "DEMO-B", 0).volume == "1"


@pytest.mark.parametrize(
    ("sequence", "visible_range", "bucket_size", "start"),
    [
        (0, "all", 1, 0),
        (500, "all", 1, 1),
        (501, "all", 5, 1),
        (2501, "all", 20, 1),
        (10_000, "all", 20, 1),
        (2501, "500", 1, 2002),
        (2501, "100", 1, 2402),
    ],
)
def test_bucket_alignment_and_partial_current_bucket(
    sequence: int, visible_range: Any, bucket_size: int, start: int
) -> None:
    projection = Projection.recover(sequence, [trade(sequence, 1, 101)] if sequence else [])
    size, first, candles = projection.price(7, visible_range)
    assert (size, first) == (bucket_size, start)
    assert len(candles) <= 500
    if sequence:
        assert candles[-1].end_sequence == str(sequence)
        assert candles[-1].close == "101"
        assert (int(candles[-1].start_sequence) - 1) % size == 0
        assert sum(int(candle.volume) for candle in candles) == 1


def test_projection_failure_is_atomic() -> None:
    projection = Projection()
    with pytest.raises(ValueError, match="unordered"):
        projection.apply_command(1, [trade(1, 2, 10), trade(1, 1, 20)])
    assert projection.sequence == 0
    assert not projection.totals
    assert not projection.commands[7]
    with pytest.raises(ValueError, match="prefix"):
        Projection.recover(1, [trade(2, 1, 10)])


def test_native_multifill_and_reopen_history_match(service: Service) -> None:
    submit(service, side="sell", quantity="3", price="101", client_id="2")
    submit(service, side="sell", quantity="4", price="102", client_id="3")
    response = submit(service, quantity="6", order_type="market_ioc", price=None)
    assert response.outcome == "committed"
    candle = service.chart().candles[-1]
    assert (candle.open, candle.close, candle.high, candle.low, candle.volume) == (
        "101",
        "102",
        "102",
        "101",
        "6",
    )
    assert service.chart(kind="depth").asks[0].quantity == "1"
    rejected = submit(service, quantity="0")
    assert rejected.outcome == "rejected"
    assert service.chart().candles[-1].open is None
    assert service.chart().end_sequence == "4"
    before = service.chart()
    summaries = service.view().instruments
    assert service.meta is not None
    service.open(service.meta["id"], service.activation)
    reopened = service.chart()
    assert reopened.candles == before.candles
    assert reopened.activation != before.activation
    assert service.view().instruments == summaries
    assert not service.view().trades
    assert not service.view().events
    assert "Session reopened" in service.view().activity_marker
    # Live trades append once after the recovered prefix, without replay duplication.
    submit(service, order_type="market_ioc", quantity="1", price=None)
    assert service.view().instruments[0].volume == "7"


def test_chart_cache_and_full_depth_are_independent_of_ladder(
    service: Service, monkeypatch: pytest.MonkeyPatch
) -> None:
    for price in range(1, 31):
        submit(service, price=str(price), quantity="2")
    assert len(service.view(depth=10).bids) == 10
    chart = service.chart(kind="depth")
    assert len(chart.bids) == 30
    assert chart.bids[-1].cumulative == "60"
    price_chart = service.chart()

    def forbidden(*args: Any, **kwargs: Any) -> Any:
        raise AssertionError("Unchanged chart poll rebuilt native or projection data.")

    monkeypatch.setattr(service.engine_type, "snapshot", forbidden)
    monkeypatch.setattr(service.engine_type, "state_digest", forbidden)
    monkeypatch.setattr(service.projection, "price", forbidden)
    assert service.chart() is price_chart
    assert service.chart(kind="depth") is chart
    assert service.meta is not None
    service.rename(service.meta["id"], "Renamed")
    assert service.chart() is price_chart
    assert service.view().chart_revision == price_chart.revision


def test_projection_error_stops_mutation_and_recovery_rebuilds_history(
    service: Service, monkeypatch: pytest.MonkeyPatch
) -> None:
    submit(service, side="sell", price="101", client_id="2")
    assert service.meta is not None
    sid = service.meta["id"]

    def fail(*args: Any, **kwargs: Any) -> None:
        raise ValueError("injected projection failure")

    monkeypatch.setattr(service.projection, "apply_command", fail)
    result = submit(service, order_type="market_ioc", quantity="2", price=None)
    assert result.outcome == "fault"
    assert result.view.saved_sequence == "2"
    with pytest.raises(ServiceError, match="Mutations are stopped"):
        submit(service)
    with pytest.raises(ServiceError, match="Chart projection failed"):
        service.chart()
    service.open(sid, service.activation)
    assert service.chart().candles[-1].volume == "2"
    assert service.view().instruments[0].volume == "2"


def test_invalid_instrument_metadata_does_not_publish_partial_chart(service: Service) -> None:
    submit(service, side="sell", price="101", client_id="2")
    submit(service, order_type="market_ioc", quantity="2", price=None)
    assert service.meta is not None
    sid = service.meta["id"]
    meta = service.store.read(sid)
    meta["instruments"] = []
    service.store.write(meta)
    log = service.store.directory(sid) / "commands.atlaslog"
    before = log.read_bytes()
    with pytest.raises(ServiceError, match="Strict recovery failed"):
        service.open(sid, service.activation)
    assert service.view().session is None
    assert service.view().fault is not None
    assert not service.chart().candles
    assert all(item.volume == "0" for item in service.view().instruments)
    assert log.read_bytes() == before


def test_chart_http_validation_exact_values_and_activation(tmp_path: Path) -> None:
    with TestClient(create_app(tmp_path, serve_frontend=False)) as client:
        initial = client.get("/api/v1/view").json()
        created = client.post(
            "/api/v1/sessions",
            json={"activation": initial["activation"], "name": "Exact chart", "mode": "manual"},
        ).json()["view"]
        command = dict(
            activation=created["activation"],
            kind="new",
            instrument_id="7",
            client_id="2",
            side="sell",
            order_type="limit_gtc",
            quantity="9007199254740993",
            price="9007199254740993",
        )
        assert client.post("/api/v1/commands", json=command).json()["outcome"] == "committed"
        response = client.post(
            "/api/v1/commands", json={**command, "client_id": "1", "side": "buy"}
        ).json()
        assert response["outcome"] == "committed"
        chart = client.get("/api/v1/chart?kind=price&range=all").json()
        assert chart["candles"][-1]["close"] == "9007199254740993"
        assert chart["candles"][-1]["volume"] == "9007199254740993"
        assert chart["revision"] == response["view"]["chart_revision"]
        assert chart["instance_id"] == response["view"]["instance_id"]
        # Negative prices are a native semantic rejection, not charted executions.
        rejected = client.post(
            "/api/v1/commands", json={**command, "price": "-9007199254740993"}
        ).json()
        assert rejected["outcome"] == "rejected"
        assert rejected["view"]["sequence"] == "3"
        assert client.get("/api/v1/chart").json()["candles"][-1]["open"] is None
        assert client.get("/api/v1/chart?range=7").status_code == 422
        assert client.get("/api/v1/chart?kind=trades").status_code == 422
        assert client.get("/api/v1/chart?instrument=43").status_code == 422
        empty = client.get("/api/v1/chart?instrument=42").json()
        assert all(candle["open"] is None for candle in empty["candles"])
        next_view = client.post(
            "/api/v1/sessions",
            json={"activation": chart["activation"], "name": "Fresh", "mode": "manual"},
        ).json()["view"]
        fresh = client.get("/api/v1/chart").json()
        assert fresh["activation"] == next_view["activation"] != chart["activation"]
        assert fresh["candles"] == []
        assert fresh["sequence"] == "0"
