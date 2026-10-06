"""Real installed-wheel launch/restart smoke. Run outside the repository checkout."""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any
from urllib.error import URLError
from urllib.request import Request, urlopen


def main() -> None:
    import atlaslob
    import atlaslob.ui

    package = Path(atlaslob.__file__).resolve()
    assert "site-packages" in str(package), f"Not a clean installed package: {package}"
    static = Path(atlaslob.ui.__file__).parent / "static"
    assert (static / "index.html").is_file()
    assert list((static / "assets").glob("*.js"))
    assert (static / "fonts" / "Manrope-Variable.ttf").is_file()
    assert (static / "atlaslob-logo.svg").is_file()
    assert (static / "atlaslob-logo-light.svg").is_file()
    assert not list(static.rglob("*.map"))
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    origin = f"http://127.0.0.1:{port}"

    def api(path: str, body: dict[str, Any] | None = None) -> Any:
        request = Request(
            origin + "/api/v1" + path,
            data=None if body is None else json.dumps(body).encode(),
            headers={} if body is None else {"Content-Type": "application/json"},
        )
        with urlopen(request, timeout=10) as response:
            return json.load(response)

    with tempfile.TemporaryDirectory(prefix="atlaslob-installed-") as directory:
        store = Path(directory) / "sessions"
        environment = {
            **os.environ,
            "PATH": "/usr/bin:/bin",
        }  # No Node or compiler needed to launch.
        environment.pop("PYTHONPATH", None)

        def start() -> subprocess.Popen[bytes]:
            process = subprocess.Popen(
                [
                    sys.executable,
                    "-m",
                    "atlaslob.ui",
                    "--port",
                    str(port),
                    "--data-dir",
                    str(store),
                ],
                cwd=directory,
                env=environment,
            )
            for _ in range(100):
                if process.poll() is not None:
                    raise AssertionError("Installed server exited during startup")
                try:
                    api("/status")
                    return process
                except (OSError, URLError):
                    time.sleep(0.05)
            process.terminate()
            process.wait(timeout=10)
            raise AssertionError("Installed server did not become ready")

        process = start()
        try:
            with urlopen(origin, timeout=10) as response:
                assert response.status == 200
            initial = api("/view")
            assert initial["sequence"] == "24"
            sid = initial["session"]["id"]
            submitted = api(
                "/commands",
                {
                    "activation": initial["activation"],
                    "kind": "new",
                    "instrument_id": "7",
                    "client_id": "1",
                    "side": "buy",
                    "order_type": "limit_gtc",
                    "quantity": "9007199254740993",
                    "price": "90",
                },
            )
            assert submitted["outcome"] == "committed"
            assert submitted["view"]["sequence"] == "25"
            traded = api(
                "/commands",
                {
                    "activation": initial["activation"],
                    "kind": "new",
                    "instrument_id": "7",
                    "client_id": "2",
                    "side": "sell",
                    "order_type": "market_ioc",
                    "quantity": "9007199254740993",
                },
            )
            assert traded["outcome"] == "committed"
            history = api("/chart?range=all")
            assert history["candles"][-1]["volume"] == "9007199254740993"
            assert history["candles"][-1]["close"] == "90"
            summaries = traded["view"]["instruments"]
        finally:
            process.terminate()
            process.wait(timeout=10)
        process = start()
        try:
            assert api("/view")["session"] is None
            reopened = api(f"/sessions/{sid}/open", {"activation": None})["view"]
            assert reopened["sequence"] == "26"
            assert reopened["events"] == []
            assert reopened["trades"] == []
            assert reopened["instruments"] == summaries
            assert api("/chart?range=all")["candles"] == history["candles"]
            assert any(
                s["id"] == "market-walk" and s["step_count"] == 600 for s in api("/scenarios")
            )
        finally:
            process.terminate()
            process.wait(timeout=10)
    print("Installed wheel: assets, native commands, exact charts and full history restart passed.")


if __name__ == "__main__":
    main()
