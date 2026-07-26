#!/usr/bin/env python3
"""Verify the native source distribution is complete and clean."""

from __future__ import annotations

import argparse
import tarfile
from pathlib import Path, PurePosixPath


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("distribution_directory", type=Path)
    arguments = parser.parse_args()

    archives = sorted(arguments.distribution_directory.glob("atlaslob-0.2.0.tar.gz"))
    if len(archives) != 1:
        raise AssertionError(f"expected one AtlasLOB sdist, found {archives!r}")

    with tarfile.open(archives[0], mode="r:gz") as archive:
        names = {PurePosixPath(name) for name in archive.getnames()}

    roots = {name.parts[0] for name in names if name.parts}
    if roots != {"atlaslob-0.2.0"}:
        raise AssertionError(f"unexpected sdist roots: {sorted(roots)!r}")
    root = PurePosixPath("atlaslob-0.2.0")

    required = {
        root / "CMakeLists.txt",
        root / "LICENSE",
        root / "README.md",
        root / "THIRD_PARTY_NOTICES.md",
        root / "pyproject.toml",
        root / "cmake" / "AtlasSanitizers.cmake",
        root / "benchmarks" / "python" / "atlas_python_bench_worker.py",
        root / "include" / "atlaslob" / "multi_instrument_engine.hpp",
        root / "include" / "atlaslob" / "persistence" / "logged_engine.hpp",
        root / "src" / "core" / "multi_instrument_engine.cpp",
        root / "src" / "persistence" / "logged_engine.cpp",
        root / "src" / "python" / "native_engine_module.cpp",
        root / "python" / "src" / "atlaslob" / "__init__.py",
        root / "python" / "src" / "atlaslob" / "engine.py",
        root / "python" / "src" / "atlaslob" / "_native_engine.pyi",
        root / "python" / "src" / "atlaslob" / "performance" / "__main__.py",
        root / "python" / "src" / "atlaslob" / "performance" / "schemas.py",
        root / "python" / "src" / "atlaslob" / "py.typed",
    }
    missing = sorted(str(name) for name in required - names)
    if missing:
        raise AssertionError(f"sdist is missing required entries: {missing!r}")

    forbidden = sorted(
        str(name)
        for name in names
        if any(part in {".git", ".github", "build", "out", "__pycache__"} for part in name.parts)
        or any(part.endswith(".egg-info") for part in name.parts)
        or name.suffix in {".pyc", ".pyo"}
        or "tests" in name.parts
        or "corpus" in name.parts
    )
    if forbidden:
        raise AssertionError(f"sdist contains excluded development artifacts: {forbidden!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
