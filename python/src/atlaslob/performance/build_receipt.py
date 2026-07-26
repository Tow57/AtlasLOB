"""Derive benchmark build provenance from CMake's generated build metadata."""

from __future__ import annotations

import hashlib
import json
import re
import shlex
import subprocess
from dataclasses import dataclass
from pathlib import Path

from atlaslob.performance.schemas import document_sha256

BUILD_RECEIPT_SCHEMA = "ATLAS_BENCH_BUILD_RECEIPT_V1"
_TARGET_PATTERN = re.compile(r"(?:^|/)CMakeFiles/([^/]+)\.dir(?:/|$)")
_TIMED_TARGETS = frozenset(
    {
        "atlas_bench_runner",
        "atlas_benchmark_support",
        "atlas_core",
        "atlas_domain",
        "atlas_internal_utility",
        "atlas_persistence",
        "atlas_persistence_cli_support",
    }
)
_ALLOCATION_TARGETS = frozenset(
    {
        "atlas_bench_alloc_runner",
        "atlas_benchmark_support",
        "atlas_core",
        "atlas_domain",
        "atlas_internal_utility",
        "atlas_persistence",
        "atlas_persistence_cli_support",
    }
)
_PYTHON_TARGETS = frozenset(
    {
        "atlas_native_engine",
        "atlas_core",
        "atlas_domain",
        "atlas_internal_utility",
        "atlas_persistence",
    }
)


@dataclass(frozen=True, slots=True)
class BuildReceipt:
    sha256: str
    compiler: str
    compiler_flags: tuple[str, ...]
    target_profiles: tuple[tuple[str, str], ...]
    build_type: str
    optimization: str
    ndebug: bool
    frame_pointers: bool
    invariants: bool
    sanitizers: bool
    lto: bool
    benchmark_build: bool
    warnings_as_errors: bool
    debug_symbols: bool
    cxx20: bool
    release_flags_locked: bool

    @property
    def native_official_ready(self) -> bool:
        return (
            self.build_type == "Release"
            and self.optimization == "O3"
            and self.ndebug
            and self.frame_pointers
            and not self.invariants
            and not self.sanitizers
            and not self.lto
            and self.benchmark_build
            and self.warnings_as_errors
            and self.debug_symbols
            and self.cxx20
            and self.release_flags_locked
        )


def derive_build_receipt(
    binary: Path,
    build_directory: Path,
    source_directory: Path,
) -> BuildReceipt:
    """Derive a canonical target-closure receipt without trusting CLI build claims."""

    cache = _read_cache(build_directory / "CMakeCache.txt")
    entries = _read_compile_commands(build_directory / "compile_commands.json")
    binary_name = binary.stem
    targets = (
        _ALLOCATION_TARGETS
        if binary_name == "atlas_bench_alloc_runner"
        else _TIMED_TARGETS
        if binary_name == "atlas_bench_runner"
        else _PYTHON_TARGETS
        if binary.name.startswith("_native_engine.")
        else None
    )
    if targets is None:
        raise ValueError("binary is not a Phase 5 benchmark target")
    _verify_binary_belongs_to_build(binary, build_directory)

    selected: dict[str, list[tuple[str, ...]]] = {target: [] for target in targets}
    compilers: set[str] = set()
    for entry in entries:
        arguments = _arguments(entry)
        output = entry.get("output")
        if not isinstance(output, str):
            raise ValueError("compile_commands entry omits output target identity")
        match = _TARGET_PATTERN.search(output.replace("\\", "/"))
        if match is None or match.group(1) not in targets:
            continue
        if not arguments:
            raise ValueError("compile_commands entry has an empty command")
        compilers.add(arguments[0])
        selected[match.group(1)].append(
            _normalized_profile(
                arguments,
                entry,
                source_directory.resolve(),
                build_directory.resolve(),
            )
        )
    missing = tuple(sorted(target for target, profiles in selected.items() if not profiles))
    if missing:
        raise ValueError(f"compile_commands omits required benchmark targets: {','.join(missing)}")
    if len(compilers) != 1:
        raise ValueError("benchmark target closure uses multiple compiler executables")

    target_profiles: list[tuple[str, str]] = []
    flattened_flags: list[str] = []
    for target in sorted(selected):
        profiles = sorted(set(selected[target]))
        for index, profile in enumerate(profiles):
            target_profiles.append((f"{target}.{index:04d}", " ".join(profile)))
            for flag in profile:
                if flag not in flattened_flags:
                    flattened_flags.append(flag)

    cache_build_type = cache.get("CMAKE_BUILD_TYPE", "unknown")
    profile_values = tuple(profile for profiles in selected.values() for profile in profiles)
    compiler_executable = next(iter(compilers))
    compiler = _compiler_identity(compiler_executable)
    invariant_definitions = tuple(
        _effective_boolean_definition(profile, "ATLAS_ENABLE_INVARIANTS")
        for profile in selected["atlas_core"]
    )
    invariants = _cache_bool(cache, "ATLAS_ENABLE_INVARIANTS", default=True) or any(
        value is not False for value in invariant_definitions
    )
    sanitizers = any(
        _cache_bool(cache, name, default=False)
        for name in ("ATLAS_ENABLE_ASAN", "ATLAS_ENABLE_UBSAN", "ATLAS_ENABLE_TSAN")
    ) or any(any("-fsanitize" in flag for flag in profile) for profile in profile_values)
    lto = _cache_bool(cache, "CMAKE_INTERPROCEDURAL_OPTIMIZATION", default=False) or any(
        _effective_lto(profile) for profile in profile_values
    )
    release_flags = _tokenize_command(cache.get("CMAKE_CXX_FLAGS_RELEASE", ""))
    release_flags_locked = release_flags == (
        "-O3",
        "-DNDEBUG",
        "-g",
        "-fno-omit-frame-pointer",
    )
    facts = {
        "schema": BUILD_RECEIPT_SCHEMA,
        "compiler": compiler,
        "compiler_flags": flattened_flags,
        "target_profiles": {name: profile for name, profile in target_profiles},
        "build_type": cache_build_type,
        "optimization": (
            "O3"
            if all(_effective_optimization(profile) == "O3" for profile in profile_values)
            else "unknown"
        ),
        "ndebug": all(_effective_ndebug(profile) for profile in profile_values),
        "frame_pointers": all(_effective_frame_pointers(profile) for profile in profile_values),
        "invariants": invariants,
        "sanitizers": sanitizers,
        "lto": lto,
        "benchmark_build": _cache_bool(cache, "ATLAS_BUILD_BENCHMARKS", default=False),
        "warnings_as_errors": _cache_bool(cache, "ATLAS_WARNINGS_AS_ERRORS", default=False)
        and all(_effective_werror(profile) for profile in profile_values),
        "debug_symbols": all(_effective_debug_symbols(profile) for profile in profile_values),
        "cxx20": all(_effective_cxx_standard(profile) for profile in profile_values),
        "release_flags_locked": release_flags_locked
        and all(not _has_extra_codegen_flag(profile) for profile in profile_values),
    }
    return BuildReceipt(
        sha256=document_sha256(facts),
        compiler=compiler,
        compiler_flags=tuple(flattened_flags),
        target_profiles=tuple(target_profiles),
        build_type=cache_build_type,
        optimization=str(facts["optimization"]),
        ndebug=bool(facts["ndebug"]),
        frame_pointers=bool(facts["frame_pointers"]),
        invariants=invariants,
        sanitizers=sanitizers,
        lto=lto,
        benchmark_build=bool(facts["benchmark_build"]),
        warnings_as_errors=bool(facts["warnings_as_errors"]),
        debug_symbols=bool(facts["debug_symbols"]),
        cxx20=bool(facts["cxx20"]),
        release_flags_locked=bool(facts["release_flags_locked"]),
    )


def _read_cache(path: Path) -> dict[str, str]:
    try:
        lines = _bounded_text(path, 4 * 1024 * 1024).splitlines()
    except (OSError, UnicodeDecodeError) as exc:
        raise ValueError("build directory does not contain a readable CMakeCache.txt") from exc
    values: dict[str, str] = {}
    for line in lines:
        if not line or line.startswith(("//", "#")) or "=" not in line or ":" not in line:
            continue
        name_and_type, value = line.split("=", 1)
        name, _separator, _kind = name_and_type.partition(":")
        if name in values:
            raise ValueError("CMakeCache contains a duplicate variable")
        values[name] = value
    return values


def _read_compile_commands(path: Path) -> tuple[dict[str, object], ...]:
    try:
        raw = json.loads(
            _bounded_text(path, 64 * 1024 * 1024),
            object_pairs_hook=_unique_json_object,
        )
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("build directory lacks valid compile_commands.json") from exc
    if not isinstance(raw, list) or not raw:
        raise ValueError("compile_commands.json must be a nonempty array")
    entries: list[dict[str, object]] = []
    for entry in raw:
        if not isinstance(entry, dict) or any(not isinstance(key, str) for key in entry):
            raise ValueError("compile_commands contains a malformed entry")
        entries.append(entry)
    return tuple(entries)


def _arguments(entry: dict[str, object]) -> tuple[str, ...]:
    raw_arguments = entry.get("arguments")
    if isinstance(raw_arguments, list) and all(isinstance(item, str) for item in raw_arguments):
        return tuple(raw_arguments)
    raw_command = entry.get("command")
    if not isinstance(raw_command, str):
        raise ValueError("compile_commands entry lacks arguments or command")
    try:
        return _tokenize_command(raw_command)
    except ValueError as exc:
        raise ValueError("compile command cannot be tokenized") from exc


def _normalized_profile(
    arguments: tuple[str, ...],
    entry: dict[str, object],
    source_directory: Path,
    build_directory: Path,
) -> tuple[str, ...]:
    raw_file = entry.get("file")
    raw_output = entry.get("output")
    source = "" if not isinstance(raw_file, str) else raw_file
    output = "" if not isinstance(raw_output, str) else raw_output
    skipped_operand = False
    result: list[str] = []
    for index, argument in enumerate(arguments):
        if index == 0:
            continue
        if skipped_operand:
            skipped_operand = False
            continue
        if argument in {"-o", "-c", "-MF", "-MT", "-MQ"}:
            skipped_operand = True
            continue
        if argument == source or argument == output:
            continue
        if argument.startswith(("/Fo", "-o")) and len(argument) > 2:
            continue
        result.append(_normalize_prefix(argument, source_directory, build_directory))
    if not result:
        raise ValueError("compile profile is empty after removing non-semantic operands")
    return tuple(result)


def _normalize_prefix(argument: str, source: Path, build: Path) -> str:
    normalized = argument.replace("\\", "/")
    source_text = source.as_posix().rstrip("/")
    build_text = build.as_posix().rstrip("/")
    normalized = normalized.replace(build_text, "<BUILD>")
    normalized = normalized.replace(source_text, "<SOURCE>")
    return normalized


def _cache_bool(cache: dict[str, str], name: str, *, default: bool) -> bool:
    raw = cache.get(name)
    if raw is None:
        return default
    normalized = raw.upper()
    if normalized in {"ON", "YES", "TRUE", "Y", "1"}:
        return True
    if normalized in {"OFF", "NO", "FALSE", "N", "0", ""}:
        return False
    raise ValueError(f"CMakeCache boolean {name} is malformed")


def _compiler_identity(executable: str) -> str:
    try:
        completed = subprocess.run(
            (executable, "--version"),
            capture_output=True,
            check=False,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ValueError("cannot execute the compiler recorded by compile_commands") from exc
    lines = completed.stdout.splitlines()
    if completed.returncode != 0 or completed.stderr or not lines:
        raise ValueError("compiler identity command failed")
    identity = " ".join(lines[0].split())
    if not identity or not identity.isascii():
        raise ValueError("compiler identity is not sanitized ASCII")
    return identity


def _bounded_text(path: Path, maximum: int) -> str:
    size = path.stat().st_size
    if size < 0 or size > maximum:
        raise ValueError("build metadata exceeds its bound")
    with path.open("rb") as source:
        data = source.read(maximum + 1)
    if len(data) != size or len(data) > maximum:
        raise ValueError("build metadata changed or exceeds its bound")
    return data.decode("utf-8")


def _unique_json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate compile_commands key: {key}")
        value[key] = item
    return value


def _tokenize_command(command: str) -> tuple[str, ...]:
    windows_style = bool(re.search(r'(?:^|\s)"?[A-Za-z]:\\', command))
    return _split_windows_command(command) if windows_style else tuple(shlex.split(command))


def _split_windows_command(command: str) -> tuple[str, ...]:
    """Parse the quoting/backslash rules used by CommandLineToArgvW."""

    arguments: list[str] = []
    offset = 0
    while offset < len(command):
        while offset < len(command) and command[offset] in " \t":
            offset += 1
        if offset == len(command):
            break
        value: list[str] = []
        in_quotes = False
        while offset < len(command):
            character = command[offset]
            if character in " \t" and not in_quotes:
                break
            if character == "\\":
                begin = offset
                while offset < len(command) and command[offset] == "\\":
                    offset += 1
                backslashes = offset - begin
                if offset < len(command) and command[offset] == '"':
                    value.extend("\\" * (backslashes // 2))
                    if backslashes % 2:
                        value.append('"')
                    else:
                        in_quotes = not in_quotes
                    offset += 1
                else:
                    value.extend("\\" * backslashes)
                continue
            if character == '"':
                in_quotes = not in_quotes
                offset += 1
                continue
            value.append(character)
            offset += 1
        if in_quotes:
            raise ValueError("Windows compile command contains an unterminated quote")
        arguments.append("".join(value))
    return tuple(arguments)


def _last_matching(profile: tuple[str, ...], candidates: frozenset[str]) -> str | None:
    return next((flag for flag in reversed(profile) if flag in candidates), None)


def _effective_optimization(profile: tuple[str, ...]) -> str:
    value = next(
        (flag for flag in reversed(profile) if re.fullmatch(r"-O(?:0|1|2|3|g|s|z|fast)", flag)),
        None,
    )
    return "O3" if value == "-O3" else "unknown"


def _effective_ndebug(profile: tuple[str, ...]) -> bool:
    value = next(
        (
            flag
            for flag in reversed(profile)
            if flag == "-UNDEBUG" or flag == "-DNDEBUG" or flag.startswith("-DNDEBUG=")
        ),
        None,
    )
    return value is not None and value.startswith("-DNDEBUG")


def _effective_frame_pointers(profile: tuple[str, ...]) -> bool:
    return (
        _last_matching(
            profile,
            frozenset({"-fno-omit-frame-pointer", "-fomit-frame-pointer"}),
        )
        == "-fno-omit-frame-pointer"
    )


def _effective_werror(profile: tuple[str, ...]) -> bool:
    return _last_matching(profile, frozenset({"-Werror", "-Wno-error"})) == "-Werror"


def _effective_debug_symbols(profile: tuple[str, ...]) -> bool:
    value = next((flag for flag in reversed(profile) if flag.startswith("-g")), None)
    return value is not None and value not in {"-g0", "-ggdb0"}


def _effective_cxx_standard(profile: tuple[str, ...]) -> bool:
    value = next((flag for flag in reversed(profile) if flag.startswith("-std=")), None)
    return value in {"-std=c++20", "-std=gnu++20"}


def _effective_lto(profile: tuple[str, ...]) -> bool:
    value = next(
        (flag for flag in reversed(profile) if flag == "-fno-lto" or flag.startswith("-flto")),
        None,
    )
    return value is not None and value != "-fno-lto"


def _effective_boolean_definition(
    profile: tuple[str, ...],
    name: str,
) -> bool | None:
    value = next(
        (
            flag
            for flag in reversed(profile)
            if flag == f"-U{name}" or flag == f"-D{name}" or flag.startswith(f"-D{name}=")
        ),
        None,
    )
    if value is None:
        return None
    if value == f"-U{name}":
        return False
    raw = "1" if value == f"-D{name}" else value.partition("=")[2]
    if raw in {"0", "OFF", "FALSE"}:
        return False
    if raw in {"1", "ON", "TRUE"}:
        return True
    return None


def _has_extra_codegen_flag(profile: tuple[str, ...]) -> bool:
    allowed = {
        "-O3",
        "-DNDEBUG",
        "-g",
        "-fno-omit-frame-pointer",
        "-fPIC",
        "-fvisibility=hidden",
        "-fvisibility-inlines-hidden",
    }
    prefixes = (
        "-O",
        "-march",
        "-mtune",
        "-mcpu",
        "-mavx",
        "-msse",
        "-flto",
        "-fno-lto",
        "-fprofile",
        "-ffast-math",
        "-fno-fast-math",
        "-fomit-frame-pointer",
        "-fno-omit-frame-pointer",
        "-g",
        "-falign",
        "-fassociative-math",
        "-fcf-protection",
        "-fconserve-stack",
        "-fexceptions",
        "-ffinite-math-only",
        "-ffp-",
        "-fgraphite",
        "-finline",
        "-fipa",
        "-fno-exceptions",
        "-fno-rtti",
        "-fno-semantic-interposition",
        "-fno-stack-protector",
        "-fno-strict-aliasing",
        "-fno-unroll",
        "-freorder",
        "-frtti",
        "-fsemantic-interposition",
        "-fstack-protector",
        "-fstrict-aliasing",
        "-ftree",
        "-funroll",
    )
    return any(
        (
            flag.startswith(prefixes)
            or flag.startswith("-m")
            or flag in {"-UNDEBUG"}
            or flag.startswith("-DNDEBUG=")
        )
        and flag not in allowed
        for flag in profile
    )


def _verify_binary_belongs_to_build(binary: Path, build_directory: Path) -> None:
    if not binary.is_file() or not build_directory.is_dir():
        raise ValueError("benchmark binary or build directory does not exist")
    expected = _file_digest(binary)
    candidates = tuple(
        path
        for path in build_directory.rglob(binary.name)
        if path.is_file() and _file_digest(path) == expected
    )
    if not candidates:
        raise ValueError("benchmark binary is not produced by the supplied build directory")


def _file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()
