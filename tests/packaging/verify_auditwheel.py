#!/usr/bin/env python3
"""Reject bundled or non-policy native dependencies in repaired Linux wheels."""

from __future__ import annotations

import argparse
import re
import subprocess
import tempfile
from pathlib import Path
from zipfile import ZipFile

ALLOWED_SYSTEM_SONAMES = frozenset(
    {
        "ld-linux-x86-64.so.2",
        "libc.so.6",
        "libdl.so.2",
        "libgcc_s.so.1",
        "libm.so.6",
        "libpthread.so.0",
        "libresolv.so.2",
        "librt.so.1",
        "libstdc++.so.6",
        "libutil.so.1",
    }
)
SONAME = re.compile(r"\b(?:ld-linux-x86-64|lib[A-Za-z0-9_+.-]+)\.so(?:\.\d+)*\b")


def _verify_wheel(wheel: Path) -> None:
    with ZipFile(wheel) as archive:
        bundled_libraries = sorted(
            name
            for name in archive.namelist()
            if ".libs/" in name or name.startswith("atlaslob.libs/")
        )
        extensions = sorted(
            name
            for name in archive.namelist()
            if name.startswith("atlaslob/_native_engine.") and name.endswith(".so")
        )
    if bundled_libraries:
        raise AssertionError(
            f"{wheel.name}: repaired wheel bundled unexpected libraries {bundled_libraries!r}"
        )
    if len(extensions) != 1:
        raise AssertionError(
            f"{wheel.name}: expected one native extension for export inspection, "
            f"found {extensions!r}"
        )

    with tempfile.TemporaryDirectory(prefix="atlaslob-exports-") as temporary:
        extension = Path(temporary, "_native_engine.so")
        with ZipFile(wheel) as archive:
            extension.write_bytes(archive.read(extensions[0]))
        exported = subprocess.run(
            ["nm", "-D", "--defined-only", "--format=posix", str(extension)],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    exported_names = {
        line.split(maxsplit=1)[0] for line in exported.stdout.splitlines() if line.strip()
    }
    if exported_names != {"PyInit__native_engine"}:
        raise AssertionError(
            f"{wheel.name}: unexpected defined dynamic symbols {sorted(exported_names)!r}"
        )

    completed = subprocess.run(
        ["auditwheel", "show", str(wheel)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    print(completed.stdout, end="")
    referenced_sonames = set(SONAME.findall(completed.stdout))
    unexpected = sorted(referenced_sonames - ALLOWED_SYSTEM_SONAMES)
    if unexpected:
        raise AssertionError(f"{wheel.name}: unexpected shared-library references {unexpected!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wheel_directory", type=Path)
    arguments = parser.parse_args()

    wheels = sorted(arguments.wheel_directory.glob("atlaslob-0.2.1-*.whl"))
    if not wheels:
        raise AssertionError("no AtlasLOB wheels were found for auditwheel inspection")
    for wheel in wheels:
        _verify_wheel(wheel)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
