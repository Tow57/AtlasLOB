from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest
from atlaslob.performance import build_receipt

_TARGETS = (
    "atlas_bench_runner",
    "atlas_benchmark_support",
    "atlas_core",
    "atlas_domain",
    "atlas_internal_utility",
    "atlas_persistence",
    "atlas_persistence_cli_support",
)


def _build_tree(tmp_path: Path) -> tuple[Path, Path, Path]:
    source = tmp_path / "source"
    build = source / "build"
    source.mkdir()
    build.mkdir()
    binary = build / "atlas_bench_runner"
    binary.write_bytes(b"runner-v1")
    cache = "\n".join(
        (
            "CMAKE_BUILD_TYPE:STRING=Release",
            "CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG -g -fno-omit-frame-pointer",
            "ATLAS_ENABLE_INVARIANTS:BOOL=OFF",
            "ATLAS_ENABLE_ASAN:BOOL=OFF",
            "ATLAS_ENABLE_UBSAN:BOOL=OFF",
            "ATLAS_ENABLE_TSAN:BOOL=OFF",
            "ATLAS_BUILD_BENCHMARKS:BOOL=ON",
            "ATLAS_WARNINGS_AS_ERRORS:BOOL=ON",
            "CMAKE_INTERPROCEDURAL_OPTIMIZATION:BOOL=OFF",
            "",
        )
    )
    (build / "CMakeCache.txt").write_text(cache, encoding="utf-8", newline="\n")
    entries: list[dict[str, object]] = []
    for target in _TARGETS:
        source_file = source / f"{target}.cpp"
        source_file.write_text("// fixture\n", encoding="ascii", newline="\n")
        output = f"CMakeFiles/{target}.dir/{target}.cpp.o"
        entries.append(
            {
                "directory": str(build),
                "file": str(source_file),
                "output": output,
                "arguments": [
                    sys.executable,
                    "-O3",
                    "-DNDEBUG",
                    "-g",
                    "-fno-omit-frame-pointer",
                    "-Werror",
                    "-std=c++20",
                    "-DATLAS_ENABLE_INVARIANTS=0",
                    f"-I{source / 'include'}",
                    f"-I{build / 'generated'}",
                    "-c",
                    str(source_file),
                    "-o",
                    output,
                ],
            }
        )
    (build / "compile_commands.json").write_text(
        json.dumps(entries, separators=(",", ":")),
        encoding="utf-8",
        newline="\n",
    )
    return binary, build, source


def test_receipt_derives_frozen_release_facts_and_redacts_paths(tmp_path: Path) -> None:
    binary, build, source = _build_tree(tmp_path)

    receipt = build_receipt.derive_build_receipt(binary, build, source)

    assert receipt.native_official_ready
    assert receipt.optimization == "O3"
    assert receipt.release_flags_locked
    profiles = "\n".join(value for _, value in receipt.target_profiles)
    assert str(source).replace("\\", "/") not in profiles
    assert str(build).replace("\\", "/") not in profiles
    assert "<SOURCE>" in profiles
    assert "<BUILD>" in profiles


def test_receipt_rejects_stale_binary_and_missing_target_closure(tmp_path: Path) -> None:
    binary, build, source = _build_tree(tmp_path)
    external = tmp_path / "outside" / binary.name
    external.parent.mkdir()
    external.write_bytes(b"not-the-build-output")

    with pytest.raises(ValueError, match="not produced"):
        build_receipt.derive_build_receipt(external, build, source)

    entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    assert isinstance(entries, list)
    entries = [
        item
        for item in entries
        if isinstance(item, dict)
        and "atlas_persistence_cli_support.dir" not in str(item.get("output"))
    ]
    (build / "compile_commands.json").write_text(
        json.dumps(entries, separators=(",", ":")),
        encoding="utf-8",
        newline="\n",
    )
    with pytest.raises(ValueError, match="atlas_persistence_cli_support"):
        build_receipt.derive_build_receipt(binary, build, source)


def test_receipt_uses_effective_flags_and_rejects_metadata_duplicates(
    tmp_path: Path,
) -> None:
    binary, build, source = _build_tree(tmp_path)
    entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    assert isinstance(entries, list)
    core_entry = next(
        entry
        for entry in entries
        if isinstance(entry, dict) and "atlas_core.dir" in str(entry.get("output"))
    )
    core_arguments = core_entry["arguments"]
    assert isinstance(core_arguments, list)
    definition_index = core_arguments.index("-DATLAS_ENABLE_INVARIANTS=0")
    core_arguments[definition_index] = "-DATLAS_ENABLE_INVARIANTS=1"
    (build / "compile_commands.json").write_text(
        json.dumps(entries, separators=(",", ":")),
        encoding="utf-8",
        newline="\n",
    )
    contradictory = build_receipt.derive_build_receipt(binary, build, source)
    assert contradictory.invariants
    assert not contradictory.native_official_ready

    core_arguments[definition_index] = "-DATLAS_ENABLE_INVARIANTS=0"
    for entry in entries:
        assert isinstance(entry, dict)
        arguments = entry["arguments"]
        assert isinstance(arguments, list)
        arguments.extend(("-O0", "-fomit-frame-pointer", "-Wno-error"))
    (build / "compile_commands.json").write_text(
        json.dumps(entries, separators=(",", ":")),
        encoding="utf-8",
        newline="\n",
    )

    receipt = build_receipt.derive_build_receipt(binary, build, source)
    assert receipt.optimization == "unknown"
    assert not receipt.frame_pointers
    assert not receipt.warnings_as_errors
    assert not receipt.release_flags_locked
    assert not receipt.native_official_ready

    (build / "compile_commands.json").write_text(
        '[{"file":"a","file":"b"}]',
        encoding="ascii",
        newline="\n",
    )
    with pytest.raises(ValueError, match="duplicate"):
        build_receipt.derive_build_receipt(binary, build, source)


def test_windows_compile_command_tokenizer_preserves_quoted_paths() -> None:
    command = (
        r'"C:\Program Files\LLVM\bin\clang++.exe" -O3 '
        r'-I"C:\Atlas Source\include" -c "C:\Atlas Source\file.cpp"'
    )

    assert build_receipt._tokenize_command(command) == (
        r"C:\Program Files\LLVM\bin\clang++.exe",
        "-O3",
        r"-IC:\Atlas Source\include",
        "-c",
        r"C:\Atlas Source\file.cpp",
    )
