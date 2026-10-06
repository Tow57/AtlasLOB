"""Launch a single, local-only AtlasLOB console; no Node runtime required."""

import argparse
import os
import sys
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description="Local AtlasLOB demonstration console")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument(
        "--data-dir",
        type=Path,
        default=Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share")))
        / "atlaslob-ui",
    )
    parser.add_argument(
        "--dev", action="store_true", help="API-only mode for the Vite development proxy"
    )
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.exit(
            1, "AtlasLOB UI requires Linux. On Windows, launch in Ubuntu 24.04 under WSL2.\n"
        )
    if not 1 <= args.port <= 65535:
        parser.error("port must be in 1..65535")
    try:
        import uvicorn

        from atlaslob import (
            Engine,  # noqa: F401 -- diagnose native dependencies before starting HTTP
        )
        from atlaslob.ui.app import create_app

        app = create_app(args.data_dir, serve_frontend=not args.dev, dev=args.dev)
    except (ImportError, RuntimeError) as exc:
        parser.exit(
            1, f"Cannot launch AtlasLOB UI: {exc}\nInstall a compiled wheel with the [ui] extra.\n"
        )
    print(
        f"AtlasLOB UI: http://localhost:{args.port}\nSession store: {args.data_dir.resolve()}",
        flush=True,
    )
    uvicorn.run(app, host="127.0.0.1", port=args.port, workers=1, access_log=False)


if __name__ == "__main__":
    main()
