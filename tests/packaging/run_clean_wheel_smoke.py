#!/usr/bin/env python3
"""Run each native wheel outside the checkout in a compiler-free container."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

IMAGES = {
    "cp311": "python:3.11.15-slim-bookworm",
    "cp312": "python:3.12.13-slim-bookworm",
    "cp313": "python:3.13.13-slim-bookworm",
    "cp314": "python:3.14.5-slim-bookworm",
}


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("wheel_directory", type=Path)
    return parser.parse_args()


def _wheel_for(wheel_directory: Path, interpreter: str) -> Path:
    matches = sorted(wheel_directory.glob(f"atlaslob-0.2.1-{interpreter}-{interpreter}-*.whl"))
    if len(matches) != 1:
        raise AssertionError(
            f"expected one {interpreter} wheel, found {[path.name for path in matches]!r}"
        )
    return matches[0]


def main() -> int:
    arguments = _arguments()
    wheel_directory = arguments.wheel_directory.resolve(strict=True)
    smoke_script = Path(__file__).with_name("wheel_smoke.py").resolve(strict=True)

    with tempfile.TemporaryDirectory(prefix="atlaslob-clean-wheel-") as temporary:
        staged = Path(temporary)
        shutil.copy2(smoke_script, staged / smoke_script.name)
        for interpreter in IMAGES:
            source = _wheel_for(wheel_directory, interpreter)
            shutil.copy2(source, staged / source.name)

        for interpreter, image in IMAGES.items():
            wheel = _wheel_for(staged, interpreter)
            container_script = f"""
set -eu
for compiler in c++ cc gcc g++ clang clang++; do
  if command -v "$compiler" >/dev/null 2>&1; then
    echo "unexpected compiler in clean wheel runtime: $compiler" >&2
    exit 1
  fi
done
python -m pip install --disable-pip-version-check --no-deps /artifacts/{wheel.name}
cd /tmp
python /artifacts/wheel_smoke.py
"""
            subprocess.run(
                [
                    "docker",
                    "run",
                    "--rm",
                    "--mount",
                    f"type=bind,source={staged},target=/artifacts,readonly",
                    image,
                    "sh",
                    "-c",
                    container_script,
                ],
                check=True,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
