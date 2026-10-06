"""UI integration tests always use the compiled native Engine, never the oracle."""

import time
from collections.abc import Iterator
from pathlib import Path

import pytest

pytest.importorskip("fastapi", reason="Install the optional [ui,ui-test] extras to run UI tests")
from atlaslob.ui.app import create_app
from atlaslob.ui.models import ActionResult, CreateSession, NewCommand, PlaybackRequest, View
from atlaslob.ui.scenarios import SCENARIOS, Scenario
from atlaslob.ui.service import CATALOG, Service
from atlaslob.ui.store import Store
from fastapi.testclient import TestClient


@pytest.fixture
def service(tmp_path: Path) -> Iterator[Service]:
    instance = Service(tmp_path)
    yield instance
    instance.close()


def sandbox(service: Service) -> View:
    return service.create(
        CreateSession(activation=service.activation, name="Test", mode="manual")
    ).view


def submit(service: Service) -> ActionResult:
    return service.command(
        NewCommand(
            activation=service.activation,
            kind="new",
            instrument_id="7",
            client_id="1",
            side="buy",
            order_type="limit_gtc",
            quantity="5",
            price="100",
        )
    )


def test_native_logged_save_recover(service: Service) -> None:
    sandbox(service)
    response = submit(service)
    assert response.view.sequence == response.view.saved_sequence == "1"
    digest = service.engine.state_digest()
    assert service.meta is not None
    sid, activation = service.meta["id"], service.activation
    reopened = service.open(sid, activation).view
    assert reopened.bids[0].quantity == "5"
    assert service.engine.state_digest() == digest
    assert reopened.events == []
    assert "Session reopened" in reopened.activity_marker
    assert reopened.activation != activation


@pytest.mark.parametrize("scenario", SCENARIOS, ids=lambda s: s.id)
def test_every_scenario_prefix(service: Service, scenario: Scenario) -> None:
    from atlaslob import Engine

    service.create(
        CreateSession(
            activation=service.activation, name="Run", mode="scenario", scenario_id=scenario.id
        )
    )
    expected = Engine(CATALOG, max_total_active_orders=2_000)
    assert service.meta is not None
    for cursor, (_, command) in enumerate(scenario.steps, 1):
        expected.submit(command)
        result = service.playback(PlaybackRequest(activation=service.activation, action="step"))
        assert result.view.playback.cursor == cursor
        assert service.engine.snapshot() == expected.snapshot()
        digest = service.engine.state_digest()
        service.open(service.meta["id"], service.activation)
        assert service.engine.state_digest() == digest
        assert service.view().playback.cursor == cursor
        assert not service.playing


def test_api_validation_exact_integers_and_guards(tmp_path: Path) -> None:
    with TestClient(create_app(tmp_path, serve_frontend=False)) as client:
        initial = client.get("/api/v1/view").json()
        result = client.post(
            "/api/v1/sessions",
            json={"activation": initial["activation"], "name": "Exact", "mode": "manual"},
        ).json()
        activation = result["view"]["activation"]
        command = {
            "activation": activation,
            "kind": "new",
            "instrument_id": "7",
            "client_id": "2",
            "side": "buy",
            "order_type": "limit_gtc",
            "quantity": "9007199254740993",
            "price": "9007199254740993",
        }
        assert (
            client.post("/api/v1/commands", json={**command, "quantity": "1.5"}).status_code == 422
        )
        assert client.get("/api/v1/view").json()["sequence"] == "0"
        response = client.post("/api/v1/commands", json=command).json()
        assert response["view"]["bids"][0]["price"] == "9007199254740993"
        assert response["view"]["bids"][0]["quantity"] == "9007199254740993"
        rejected = client.post("/api/v1/commands", json={**command, "quantity": "0"}).json()
        assert rejected["outcome"] == "rejected"
        assert rejected["view"]["sequence"] == "2"
        assert any(e["kind"] == "Rejected" for e in rejected["view"]["events"])
        client.post(
            "/api/v1/sessions", json={"activation": activation, "name": "Next", "mode": "manual"}
        )
        assert client.post("/api/v1/commands", json=command).status_code == 409
        assert client.get("/api/v1/view").json()["sequence"] == "0"
        assert (
            client.post(
                "/api/v1/commands", json=command, headers={"origin": "https://evil.example"}
            ).status_code
            == 403
        )


def test_strict_corruption_preserves_log(service: Service) -> None:
    sandbox(service)
    submit(service)
    assert service.meta is not None
    sid = service.meta["id"]
    log = service.store.directory(sid) / "commands.atlaslog"
    service._release()
    with log.open("ab") as stream:
        stream.write(b"torn")
    before = log.read_bytes()
    with pytest.raises(Exception, match="Strict recovery failed"):
        service.open(sid, None)
    assert log.read_bytes() == before
    assert service.sessions().sessions[0].error


def test_lock_and_restart_library(service: Service) -> None:
    with pytest.raises(RuntimeError, match="owns"):
        Store(service.store.root)
    root = service.store.root
    assert service.meta is not None
    sid = service.meta["id"]
    service.close()
    second = Service(root)
    try:
        assert second.view().session is None
        assert second.sessions().last_opened == sid
        assert len(second.sessions().sessions) == 1
    finally:
        second.close()


def test_metadata_lag_and_frozen_restart(service: Service) -> None:
    service.create(
        CreateSession(activation=service.activation, name="IOC", mode="scenario", scenario_id="ioc")
    )
    assert service.meta is not None
    original = service.meta["id"]
    old_metadata = service.store.read(original)
    for _ in range(4):
        service.playback(PlaybackRequest(activation=service.activation, action="step"))
    service.store.write(old_metadata)
    service.open(original, service.activation)
    assert service.view().playback.cursor == 4
    service.create(
        CreateSession(activation=service.activation, name="Restart", mode="scenario", restart=True)
    )
    assert service.meta["id"] != original
    assert service.view().playback.cursor == 0
    assert service.store.read(original)["scenario"] == service.meta["scenario"]


def test_play_pause_and_mode_gate(tmp_path: Path) -> None:
    with TestClient(create_app(tmp_path, serve_frontend=False)) as client:
        activation = client.get("/api/v1/view").json()["activation"]
        view = client.post(
            "/api/v1/sessions",
            json={
                "activation": activation,
                "name": "FIFO",
                "mode": "scenario",
                "scenario_id": "fifo",
            },
        ).json()["view"]
        guard = {"activation": view["activation"]}
        assert (
            client.post(
                "/api/v1/commands",
                json={
                    **guard,
                    "kind": "new",
                    "instrument_id": "7",
                    "client_id": "1",
                    "side": "buy",
                    "order_type": "market_ioc",
                    "quantity": "2",
                },
            ).status_code
            == 409
        )
        client.post("/api/v1/playback", json={**guard, "action": "speed", "speed": 10})
        client.post("/api/v1/playback", json={**guard, "action": "play"})
        time.sleep(0.25)
        paused = client.post("/api/v1/playback", json={**guard, "action": "pause"}).json()["view"]
        assert paused["playback"]["cursor"] >= 1
        time.sleep(0.2)
        assert client.get("/api/v1/view").json()["sequence"] == paused["sequence"]
