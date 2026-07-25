#!/usr/bin/env python3
"""Verify the exact native-wheel set and its distribution contents."""

from __future__ import annotations

import argparse
import re
from pathlib import Path
from zipfile import ZipFile

EXPECTED_INTERPRETERS = ("cp311", "cp312", "cp313", "cp314")
FORBIDDEN_SUFFIXES = (".a", ".h", ".hpp", ".lib")
VERSIONED_SHARED_OBJECT = re.compile(r"\.so(?:\.\d+)*$")


def _metadata_entry(names: set[str]) -> str:
    matches = sorted(name for name in names if name.endswith(".dist-info/METADATA"))
    if len(matches) != 1:
        raise AssertionError(f"expected one METADATA entry, found {matches!r}")
    return matches[0]


def _verify_one(wheel: Path, interpreter: str) -> None:
    filename = wheel.name
    if f"-{interpreter}-{interpreter}-" not in filename:
        raise AssertionError(f"{filename}: wheel is not CPython-minor-specific")
    if "abi3" in filename:
        raise AssertionError(f"{filename}: abi3 wheels are deferred")
    if not re.search(r"manylinux[^-]*_x86_64\.whl$", filename):
        raise AssertionError(f"{filename}: expected a manylinux x86-64 platform tag")

    with ZipFile(wheel) as archive:
        names = set(archive.namelist())
        required = {
            "atlaslob/__init__.py",
            "atlaslob/engine.py",
            "atlaslob/_native_engine.pyi",
            "atlaslob/py.typed",
        }
        missing = sorted(required - names)
        if missing:
            raise AssertionError(f"{filename}: missing package entries {missing!r}")

        extensions = sorted(
            name
            for name in names
            if name.startswith("atlaslob/_native_engine.") and name.endswith(".so")
        )
        if len(extensions) != 1:
            raise AssertionError(f"{filename}: expected one native extension, found {extensions!r}")
        extension_abi = f"cpython-{interpreter.removeprefix('cp')}"
        if extension_abi not in extensions[0]:
            raise AssertionError(f"{filename}: extension does not carry the {interpreter} ABI tag")
        shared_objects = sorted(
            name
            for name in names
            if name.endswith((".dll", ".dylib", ".pyd", ".so"))
            or VERSIONED_SHARED_OBJECT.search(name) is not None
        )
        if shared_objects != extensions:
            raise AssertionError(
                f"{filename}: contains unexpected shared objects {shared_objects!r}"
            )

        forbidden = sorted(
            name
            for name in names
            if name.endswith(FORBIDDEN_SUFFIXES)
            or "/tests/" in f"/{name}"
            or "/corpus/" in f"/{name}"
            or "__pycache__" in name
            or name.endswith((".pyc", ".pyo"))
        )
        if forbidden:
            raise AssertionError(f"{filename}: contains development/build artifacts {forbidden!r}")

        metadata = archive.read(_metadata_entry(names)).decode("utf-8")
        if "Version: 0.2.0\n" not in metadata:
            raise AssertionError(f"{filename}: package version is not 0.2.0")
        if "Requires-Python: <3.15,>=3.11\n" not in metadata:
            raise AssertionError(f"{filename}: unexpected Requires-Python metadata")
        if "License-Expression: MIT\n" not in metadata:
            raise AssertionError(f"{filename}: missing MIT license expression")

        license_entries = {
            name.rsplit("/", maxsplit=1)[-1] for name in names if ".dist-info/licenses/" in name
        }
        expected_licenses = {"LICENSE", "THIRD_PARTY_NOTICES.md"}
        if not expected_licenses <= license_entries:
            raise AssertionError(
                f"{filename}: missing license files {sorted(expected_licenses - license_entries)!r}"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wheel_directory", type=Path)
    arguments = parser.parse_args()

    wheels = sorted(arguments.wheel_directory.glob("atlaslob-0.2.0-*.whl"))
    if len(wheels) != len(EXPECTED_INTERPRETERS):
        raise AssertionError(
            f"expected four AtlasLOB wheels, found {[path.name for path in wheels]!r}"
        )

    by_interpreter: dict[str, Path] = {}
    for wheel in wheels:
        matching = [tag for tag in EXPECTED_INTERPRETERS if f"-{tag}-" in wheel.name]
        if len(matching) != 1:
            raise AssertionError(f"cannot determine one interpreter tag for {wheel.name}")
        interpreter = matching[0]
        if interpreter in by_interpreter:
            raise AssertionError(f"duplicate {interpreter} wheel")
        by_interpreter[interpreter] = wheel
        _verify_one(wheel, interpreter)

    missing = sorted(set(EXPECTED_INTERPRETERS) - by_interpreter.keys())
    if missing:
        raise AssertionError(f"missing interpreter wheels: {missing!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
