"""Loopback-only HTTP adapter. Importing this module does not load the native engine."""

from collections.abc import AsyncIterator, Awaitable, Callable
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Annotated, cast

from fastapi import FastAPI, Query, Request
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from starlette.middleware.trustedhost import TrustedHostMiddleware
from starlette.responses import Response

from atlaslob.ui.models import (
    ActionResult,
    ChartKind,
    ChartRange,
    ChartView,
    CommandRequest,
    CreateSession,
    ErrorResponse,
    Guard,
    PlaybackRequest,
    RenameSession,
    ScenarioInfo,
    SessionInfo,
    SessionList,
    Status,
    View,
)
from atlaslob.ui.scenarios import SCENARIOS
from atlaslob.ui.service import Controller, ServiceError


def create_app(data_dir: Path, *, serve_frontend: bool = True, dev: bool = False) -> FastAPI:
    controller = Controller(data_dir)

    @asynccontextmanager
    async def lifespan(app: FastAPI) -> AsyncIterator[None]:
        await controller.start()
        try:
            yield
        finally:
            await controller.close()

    app = FastAPI(
        title="AtlasLOB UI",
        version="1.0.0",
        lifespan=lifespan,
        responses={409: {"model": ErrorResponse}, 503: {"model": ErrorResponse}},
    )
    app.state.controller = controller
    app.add_middleware(
        TrustedHostMiddleware, allowed_hosts=["localhost", "127.0.0.1", "[::1]", "testserver"]
    )

    @app.middleware("http")
    async def same_origin(
        request: Request, call_next: Callable[[Request], Awaitable[Response]]
    ) -> Response:
        origin = request.headers.get("origin")
        allowed = {f"{request.url.scheme}://{request.headers.get('host', '')}"}
        if dev:
            allowed.update({"http://localhost:5173", "http://127.0.0.1:5173"})
        if request.method not in ("GET", "HEAD", "OPTIONS"):
            if (origin and origin not in allowed) or request.headers.get(
                "sec-fetch-site"
            ) == "cross-site":
                return JSONResponse(
                    status_code=403, content={"detail": "Cross-origin mutations are disabled."}
                )
        response = await call_next(request)
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Referrer-Policy"] = "no-referrer"
        response.headers["Cache-Control"] = "no-store"
        response.headers["Content-Security-Policy"] = (
            "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
            "img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'self'"
        )
        return response

    @app.exception_handler(ServiceError)
    async def service_error(request: Request, exc: ServiceError) -> JSONResponse:
        return JSONResponse(status_code=exc.status, content={"detail": exc.detail})

    @app.get("/api/v1/status", response_model=Status)
    async def status() -> Status:
        return cast(Status, await controller.call("status"))

    @app.get("/api/v1/sessions", response_model=SessionList)
    async def sessions() -> SessionList:
        return cast(SessionList, await controller.call("sessions"))

    @app.post("/api/v1/sessions", response_model=ActionResult)
    async def create(request: CreateSession) -> ActionResult:
        return cast(ActionResult, await controller.call("create", request))

    @app.patch("/api/v1/sessions/{sid}", response_model=SessionInfo)
    async def rename(sid: str, request: RenameSession) -> SessionInfo:
        return cast(SessionInfo, await controller.call("rename", sid, request.name))

    @app.post("/api/v1/sessions/{sid}/open", response_model=ActionResult)
    async def open_session(sid: str, request: Guard) -> ActionResult:
        return cast(ActionResult, await controller.call("open", sid, request.activation))

    @app.get("/api/v1/view", response_model=View)
    async def view(instrument: int = 7, depth: int = 20, page: int = Query(1, ge=1, le=40)) -> View:
        return cast(View, await controller.call("view", instrument, depth, page))

    @app.get("/api/v1/chart", response_model=ChartView)
    async def chart(
        instrument: int = 7,
        kind: ChartKind = "price",
        visible_range: Annotated[ChartRange, Query(alias="range")] = "500",
    ) -> ChartView:
        return cast(ChartView, await controller.call("chart", instrument, kind, visible_range))

    @app.post("/api/v1/commands", response_model=ActionResult)
    async def command(request: CommandRequest) -> ActionResult:
        return cast(ActionResult, await controller.call("command", request))

    @app.get("/api/v1/scenarios", response_model=list[ScenarioInfo])
    async def scenarios() -> list[ScenarioInfo]:
        return [
            ScenarioInfo(id=s.id, name=s.name, description=s.description, step_count=len(s.steps))
            for s in SCENARIOS
        ]

    @app.post("/api/v1/playback", response_model=ActionResult)
    async def playback(request: PlaybackRequest) -> ActionResult:
        return cast(ActionResult, await controller.call("playback", request))

    if serve_frontend:
        assets = Path(__file__).parent / "static"
        if not (assets / "index.html").is_file():
            raise RuntimeError(
                "UI assets are missing. Run python scripts/build_ui.py before packaging."
            )
        app.mount("/", StaticFiles(directory=assets, html=True), name="frontend")
    return app
