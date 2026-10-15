"""Build/check the optional UI before producing a wheel or source archive.

Run with the native package and [ui] dependencies installed in your environment.
Generated OpenAPI and TypeScript files are checked in; static assets are build outputs.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

from atlaslob.ui.app import create_app

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--schema-only", action="store_true")
    parser.add_argument(
        "--check", action="store_true", help="Fail if OpenAPI or generated types are stale"
    )
    args = parser.parse_args()
    schema = (
        json.dumps(
            create_app(Path("unused"), serve_frontend=False).openapi(), indent=2, sort_keys=True
        )
        + "\n"
    )
    schema_file = ROOT / "ui/openapi.json"
    if args.check and (not schema_file.exists() or schema_file.read_text() != schema):
        sys.exit("OpenAPI contract is stale. Run python scripts/build_ui.py --schema-only.")
    schema_file.write_text(schema)
    types_file = ROOT / "ui/src/api.generated.ts"
    old_types = types_file.read_text() if types_file.exists() else None
    subprocess.run(["npm", "run", "types"], cwd=ROOT / "ui", check=True)
    if args.check and old_types != types_file.read_text():
        sys.exit("Frontend types were stale and have been regenerated. Review and commit them.")
    if args.schema_only:
        return
    subprocess.run(["npm", "run", "build"], cwd=ROOT / "ui", check=True)
    static = ROOT / "python/src/atlaslob/ui/static"
    if not (static / "index.html").is_file() or not list((static / "assets").glob("*.js")):
        sys.exit("UI packaging check failed: built assets missing.")
    if list(static.rglob("*.map")):
        sys.exit("UI packaging check failed: source maps must not be shipped.")
    print("UI assets and API contract verified. Ready for python -m build.")


if __name__ == "__main__":
    main()
