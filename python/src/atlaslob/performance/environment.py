"""Privacy-preserving benchmark environment capture."""

from __future__ import annotations

import getpass
import json
import os
import platform
import re
import subprocess
import threading
import time
from dataclasses import replace
from pathlib import Path
from typing import BinaryIO

from atlaslob.performance.build_receipt import derive_build_receipt
from atlaslob.performance.schemas import EnvironmentManifest, file_sha256

_HOST_ALIAS = re.compile(r"(?=.{1,64}\Z)[a-z0-9]+(?:-[a-z0-9]+)*\Z")
_UNAVAILABLE = frozenset({"", "unknown", "unavailable"})


def _validate_public_alias(value: str, name: str, *, informative: bool = False) -> None:
    if _HOST_ALIAS.fullmatch(value) is None or informative and value in _UNAVAILABLE:
        raise ValueError(f"{name} must be a sanitized lowercase alias")
    normalized = value.casefold()
    for private_identity in _ambient_private_identities():
        if (
            normalized == private_identity
            or len(private_identity) >= 3
            and private_identity in normalized
        ):
            raise ValueError(f"{name} must not disclose an ambient username or hostname")


def _ambient_private_identities() -> frozenset[str]:
    raw_values = [platform.node()]
    for environment_name in ("USERNAME", "USER", "LOGNAME"):
        raw_values.append(os.environ.get(environment_name, ""))
    try:
        raw_values.append(getpass.getuser())
    except (KeyError, OSError):
        pass

    identities: set[str] = set()
    for raw in raw_values:
        normalized = raw.strip().casefold()
        if not normalized:
            continue
        identities.add(normalized)
        account = normalized.rsplit("\\", 1)[-1].rsplit("/", 1)[-1].split("@", 1)[0]
        if account:
            identities.add(account)
    return frozenset(identities)


def capture_environment(
    binary: Path,
    *,
    build_directory: Path,
    host_class: str,
    storage_class: str,
    smt_sibling_idle: bool | None,
    repository: Path | None = None,
    request_official: bool = False,
) -> EnvironmentManifest:
    """Capture reproducibility facts without hostnames, users, paths, or IPs."""

    if not binary.is_file():
        raise ValueError("benchmark binary does not exist")
    _validate_public_alias(host_class, "host_class")
    _validate_public_alias(storage_class, "storage_class", informative=True)
    root = Path.cwd() if repository is None else repository
    receipt = derive_build_receipt(binary, build_directory, root)
    limitations: list[str] = []

    commit = _git(root, "rev-parse", "HEAD")
    if commit is None or re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        raise ValueError("repository does not expose a canonical commit")
    tag = _git(root, "describe", "--tags", "--exact-match")
    dirty = _git_dirty(root)
    if dirty is None:
        raise ValueError("cannot determine repository dirty state")

    system = platform.system()
    architecture = _architecture(platform.machine())
    os_name = _os_name(system)
    kernel = _ascii_or_unknown(platform.release())
    logical_cpus = os.cpu_count() or 1
    cpu_model, microcode, physical_cores, cpu_flags = _cpu_facts(logical_cpus)
    memory_bytes = _memory_bytes()
    if memory_bytes is None:
        memory_bytes = 1
        limitations.append("physical memory size was unavailable")
    affinity = _affinity(logical_cpus)
    pinned_cpu = affinity[0] if len(affinity) == 1 and affinity[0] != 0 else None
    numa_nodes = _numa_nodes()
    numa_cpu_policy, numa_memory_policy = _process_numa_policies()
    filesystem = _filesystem(binary)
    if filesystem == "unknown":
        limitations.append("filesystem type was unavailable")
    governor = _governor(affinity[0])
    if governor == "unavailable":
        limitations.append("CPU governor was unavailable")
    turbo = _turbo_state()
    if turbo == "unavailable":
        limitations.append("turbo state was unavailable")
    smt = _smt_state(logical_cpus, physical_cores)
    virtualization = _virtualization(system, platform.release(), cpu_flags)
    perf_version = _run(("perf", "--version"))
    if perf_version is None:
        limitations.append("perf was unavailable")

    eligible = (
        os_name.startswith("Ubuntu 24.04")
        and architecture == "x86_64"
        and virtualization == "none"
        and not dirty
        and memory_bytes > 1
        and governor != "unavailable"
        and turbo != "unavailable"
        and perf_version is not None
        and pinned_cpu is not None
        and smt_sibling_idle is True
        and all(
            fact not in _UNAVAILABLE
            for fact in (
                kernel,
                cpu_model,
                microcode,
                governor,
                turbo,
                smt,
                numa_cpu_policy,
                numa_memory_policy,
                filesystem,
                storage_class,
            )
        )
        and receipt.native_official_ready
    )
    if request_official and not eligible:
        raise ValueError(
            "official environment qualification failed; capture as exploratory "
            "or satisfy every native-host prerequisite"
        )
    if not eligible:
        limitations.append("environment does not satisfy the official native-host gate")
    if pinned_cpu is None:
        limitations.append("process affinity is not pinned to one nonzero CPU")
    if smt_sibling_idle is not True:
        limitations.append("idle SMT sibling was not qualified")
    for name, fact in (
        ("kernel", kernel),
        ("CPU model", cpu_model),
        ("CPU microcode", microcode),
        ("NUMA CPU policy", numa_cpu_policy),
        ("NUMA memory policy", numa_memory_policy),
        ("filesystem", filesystem),
        ("storage class", storage_class),
        ("CPU governor", governor),
        ("turbo state", turbo),
        ("SMT state", smt),
    ):
        if fact in _UNAVAILABLE:
            limitations.append(f"{name} was unavailable")

    return EnvironmentManifest(
        commit=commit,
        tag=tag,
        dirty=dirty,
        binary_sha256=file_sha256(binary),
        os=os_name,
        kernel=kernel,
        host_class=host_class,
        cpu_model=cpu_model,
        architecture=architecture,
        physical_cores=physical_cores,
        logical_cpus=logical_cpus,
        microcode=microcode,
        memory_bytes=memory_bytes,
        compiler=receipt.compiler,
        compiler_flags=receipt.compiler_flags,
        build_receipt_sha256=receipt.sha256,
        build_target_profiles=receipt.target_profiles,
        build_type=receipt.build_type,
        optimization=receipt.optimization,
        ndebug=receipt.ndebug,
        frame_pointers=receipt.frame_pointers,
        invariants=receipt.invariants,
        sanitizers=receipt.sanitizers,
        lto=receipt.lto,
        benchmark_build=receipt.benchmark_build,
        warnings_as_errors=receipt.warnings_as_errors,
        debug_symbols=receipt.debug_symbols,
        cxx20=receipt.cxx20,
        release_flags_locked=receipt.release_flags_locked,
        affinity=affinity,
        pinned_cpu=pinned_cpu,
        smt_sibling_idle=smt_sibling_idle,
        numa_nodes=numa_nodes,
        numa_cpu_policy=numa_cpu_policy,
        numa_memory_policy=numa_memory_policy,
        filesystem=filesystem,
        storage_class=storage_class,
        governor=governor,
        turbo=turbo,
        smt=smt,
        virtualization=virtualization,
        perf_version=perf_version,
        runtime_kind="native",
        python_implementation=None,
        python_version=None,
        python_cache_tag=None,
        atlaslob_version=None,
        interpreter_sha256=None,
        wheel_sha256=None,
        package_sha256=None,
        wrapper_sha256=None,
        harness_sha256=None,
        classification="official" if eligible and request_official else "exploratory",
        host_context_sha256="",
        limitations=tuple(sorted(set(limitations))),
    )


def capture_python_environment(
    target_python: Path,
    wheel: Path,
    worker: Path,
    *,
    build_directory: Path,
    host_class: str,
    storage_class: str,
    smt_sibling_idle: bool | None,
    repository: Path | None = None,
    request_official: bool = False,
) -> EnvironmentManifest:
    """Capture a clean target-wheel CPython boundary without source imports."""

    identity = _python_identity(target_python, wheel, worker)
    extension = Path(identity.pop("extension_path"))
    base = capture_environment(
        extension,
        build_directory=build_directory,
        host_class=host_class,
        storage_class=storage_class,
        smt_sibling_idle=smt_sibling_idle,
        repository=repository,
        request_official=False,
    )
    limitations = set(base.limitations)
    limitations.discard("environment does not satisfy the official native-host gate")
    host_ready = (
        base.os.startswith("Ubuntu 24.04")
        and base.architecture == "x86_64"
        and base.virtualization == "none"
        and not base.dirty
        and base.memory_bytes > 1
        and base.perf_version is not None
        and base.pinned_cpu is not None
        and base.smt_sibling_idle is True
        and all(
            value not in _UNAVAILABLE
            for value in (
                base.kernel,
                base.cpu_model,
                base.microcode,
                base.governor,
                base.turbo,
                base.smt,
                base.numa_cpu_policy,
                base.numa_memory_policy,
                base.filesystem,
                base.storage_class,
            )
        )
    )
    build_ready = (
        base.build_type == "Release"
        and base.optimization == "O3"
        and base.ndebug
        and base.frame_pointers
        and not base.invariants
        and not base.sanitizers
        and not base.lto
        and base.warnings_as_errors
        and base.debug_symbols
        and base.cxx20
        and base.release_flags_locked
    )
    eligible = host_ready and build_ready
    if request_official and not eligible:
        raise ValueError(
            "official CPython environment qualification failed; capture as exploratory "
            "or satisfy every target-wheel prerequisite"
        )
    if not eligible:
        limitations.add("environment does not satisfy the official CPython host/build gate")
    return replace(
        base,
        runtime_kind="cpython",
        python_implementation=identity["python_implementation"],
        python_version=identity["python_version"],
        python_cache_tag=identity["python_cache_tag"],
        atlaslob_version=identity["package_version"],
        interpreter_sha256=identity["interpreter_sha256"],
        wheel_sha256=identity["wheel_sha256"],
        package_sha256=identity["package_sha256"],
        wrapper_sha256=identity["wrapper_sha256"],
        harness_sha256=file_sha256(worker),
        classification="official" if request_official else "exploratory",
        host_context_sha256="",
        limitations=tuple(sorted(limitations)),
    )


def _python_identity(
    target_python: Path,
    wheel: Path,
    worker: Path,
) -> dict[str, str]:
    if not target_python.is_file() or not wheel.is_file() or not worker.is_file():
        raise ValueError("target Python, wheel, and worker must be regular files")
    environment = dict(os.environ)
    environment.pop("PYTHONPATH", None)
    environment.pop("PYTHONHOME", None)
    returncode, stdout, stderr = _bounded_identity_process(
        (
            str(target_python),
            str(worker),
            "identity",
            "--wheel",
            str(wheel),
            "--include-private-path",
        ),
        cwd=worker.parent,
        environment=environment,
    )
    if returncode != 0 or stderr:
        raise ValueError("target-wheel identity worker failed")
    try:
        text = stdout.decode("ascii")
        if not text.endswith("\n") or "\r" in text or text.count("\n") != 1:
            raise ValueError("identity output is not one canonical record")
        raw = json.loads(text, object_pairs_hook=_unique_identity_object)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("target-wheel identity output is invalid") from exc
    expected = {
        "schema",
        "python_implementation",
        "python_version",
        "python_cache_tag",
        "interpreter_sha256",
        "wheel_sha256",
        "package_sha256",
        "wrapper_sha256",
        "extension_sha256",
        "package_version",
        "extension_path",
    }
    if not isinstance(raw, dict) or set(raw) != expected:
        raise ValueError("target-wheel identity has unexpected fields")
    if raw["schema"] != "ATLAS_PYTHON_TARGET_IDENTITY_V1":
        raise ValueError("target-wheel identity schema is unsupported")
    result: dict[str, str] = {}
    for name in expected - {"schema"}:
        value = raw[name]
        if not isinstance(value, str) or not value:
            raise ValueError("target-wheel identity field is malformed")
        result[name] = value
    for name in (
        "interpreter_sha256",
        "wheel_sha256",
        "package_sha256",
        "wrapper_sha256",
        "extension_sha256",
    ):
        if re.fullmatch(r"[0-9a-f]{64}", result[name]) is None:
            raise ValueError("target-wheel identity digest is malformed")
    if (
        result["python_implementation"] != "CPython"
        or re.fullmatch(r"3\.(?:11|12|13|14)\.[0-9]+", result["python_version"]) is None
        or file_sha256(wheel) != result["wheel_sha256"]
    ):
        raise ValueError("target-wheel identity differs from the supported runtime")
    extension = Path(result["extension_path"])
    if not extension.is_file() or file_sha256(extension) != result["extension_sha256"]:
        raise ValueError("target-wheel extension differs from its identity")
    canonical = (
        json.dumps(raw, allow_nan=False, ensure_ascii=True, separators=(",", ":"), sort_keys=True)
        + "\n"
    ).encode("ascii")
    if stdout != canonical:
        raise ValueError("target-wheel identity output is not canonical")
    return result


def _bounded_identity_process(
    command: tuple[str, ...],
    *,
    cwd: Path,
    environment: dict[str, str],
) -> tuple[int, bytes, bytes]:
    try:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise ValueError("target-wheel identity worker could not run") from exc
    assert process.stdout is not None
    assert process.stderr is not None
    exceeded = threading.Event()
    outputs = (bytearray(), bytearray())

    def read_stream(stream: BinaryIO, output: bytearray, maximum: int) -> None:
        try:
            while chunk := stream.read(16 * 1024):
                if len(output) + len(chunk) > maximum:
                    exceeded.set()
                    return
                output.extend(chunk)
        except OSError:
            exceeded.set()

    threads = (
        threading.Thread(
            target=read_stream,
            args=(process.stdout, outputs[0], 64 * 1024),
            daemon=True,
        ),
        threading.Thread(
            target=read_stream,
            args=(process.stderr, outputs[1], 16 * 1024),
            daemon=True,
        ),
    )
    for thread in threads:
        thread.start()
    deadline = time.monotonic() + 30
    failure: str | None = None
    try:
        while process.poll() is None:
            if exceeded.wait(timeout=0.05):
                failure = "target-wheel identity output exceeds its bound"
                process.kill()
                break
            if time.monotonic() >= deadline:
                failure = "target-wheel identity worker timed out"
                process.kill()
                break
        process.wait()
    except BaseException:
        if process.poll() is None:
            process.kill()
        process.wait()
        for thread in threads:
            thread.join(timeout=1)
        process.stdout.close()
        process.stderr.close()
        raise
    for thread in threads:
        thread.join(timeout=1)
    process.stdout.close()
    process.stderr.close()
    if failure is not None or exceeded.is_set() or any(thread.is_alive() for thread in threads):
        raise ValueError(failure or "target-wheel identity output could not be captured")
    return process.returncode, bytes(outputs[0]), bytes(outputs[1])


def _unique_identity_object(
    pairs: list[tuple[str, object]],
) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate target-wheel identity key: {key}")
        result[key] = value
    return result


def verify_runtime_compatibility(
    manifest: EnvironmentManifest,
    binary: Path,
    *,
    repository: Path | None = None,
) -> None:
    """Fail closed if an official manifest is stale on the current process host."""

    if manifest.classification != "official":
        return
    # The exact binary hash carries artifact identity.  Do not bind execution to
    # the orchestrator checkout: an official A/B suite intentionally compares
    # binaries captured from two different clean commits.
    del repository
    current_affinity = _affinity(os.cpu_count() or 1)
    current_virtualization = _virtualization(
        platform.system(),
        platform.release(),
        _cpu_facts(os.cpu_count() or 1)[3],
    )
    logical_cpus = os.cpu_count() or 1
    cpu_model, microcode, physical_cores, _ = _cpu_facts(logical_cpus)
    memory_bytes = _memory_bytes()
    if (
        file_sha256(binary) != manifest.binary_sha256
        or _os_name(platform.system()) != manifest.os
        or _architecture(platform.machine()) != manifest.architecture
        or _ascii_or_unknown(platform.release()) != manifest.kernel
        or current_affinity != manifest.affinity
        or current_virtualization != manifest.virtualization
        or _governor(current_affinity[0]) != manifest.governor
        or _turbo_state() != manifest.turbo
        or _process_numa_policies() != (manifest.numa_cpu_policy, manifest.numa_memory_policy)
        or _numa_nodes() != manifest.numa_nodes
        or _filesystem(binary) != manifest.filesystem
        or _run(("perf", "--version")) != manifest.perf_version
        or _smt_state(logical_cpus, physical_cores) != manifest.smt
        or cpu_model != manifest.cpu_model
        or microcode != manifest.microcode
        or physical_cores != manifest.physical_cores
        or logical_cpus != manifest.logical_cpus
        or memory_bytes != manifest.memory_bytes
    ):
        raise ValueError("official environment manifest is stale on this runtime host")


def verify_python_runtime_compatibility(
    manifest: EnvironmentManifest,
    target_python: Path,
    wheel: Path,
    worker: Path,
) -> None:
    """Verify the exact installed target-wheel identity before each suite."""

    if manifest.runtime_kind != "cpython":
        raise ValueError("Python suite requires a CPython environment manifest")
    identity = _python_identity(target_python, wheel, worker)
    extension = Path(identity["extension_path"])
    expected = (
        manifest.python_implementation,
        manifest.python_version,
        manifest.python_cache_tag,
        manifest.atlaslob_version,
        manifest.interpreter_sha256,
        manifest.wheel_sha256,
        manifest.package_sha256,
        manifest.wrapper_sha256,
        manifest.binary_sha256,
        manifest.harness_sha256,
    )
    actual = (
        identity["python_implementation"],
        identity["python_version"],
        identity["python_cache_tag"],
        identity["package_version"],
        identity["interpreter_sha256"],
        identity["wheel_sha256"],
        identity["package_sha256"],
        identity["wrapper_sha256"],
        identity["extension_sha256"],
        file_sha256(worker),
    )
    if actual != expected:
        raise ValueError("target-wheel runtime differs from its environment manifest")
    verify_runtime_compatibility(manifest, extension)


def _git(root: Path, *arguments: str) -> str | None:
    return _run(("git", "-C", str(root), *arguments))


def _git_dirty(root: Path) -> bool | None:
    """Return porcelain dirty state while preserving successful empty output."""

    try:
        completed = subprocess.run(
            (
                "git",
                "-C",
                str(root),
                "status",
                "--porcelain=v1",
                "--untracked-files=all",
            ),
            capture_output=True,
            check=False,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if completed.returncode != 0 or completed.stderr:
        return None
    return bool(completed.stdout)


def _run(command: tuple[str, ...]) -> str | None:
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            check=False,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if completed.returncode != 0 or completed.stderr:
        return None
    value = completed.stdout.strip()
    if not value or not value.isascii() or "\n" in value:
        return None
    return value


def _os_name(system: str) -> str:
    if system == "Linux":
        try:
            values = {}
            for line in Path("/etc/os-release").read_text(encoding="utf-8").splitlines():
                if "=" in line:
                    key, raw = line.split("=", 1)
                    values[key] = raw.strip('"')
            candidate = values.get("PRETTY_NAME", "Linux")
            return _ascii_or_unknown(candidate)
        except OSError:
            return "Linux"
    return _ascii_or_unknown(f"{system} {platform.version()}")


def _architecture(value: str) -> str:
    normalized = value.lower()
    if normalized in {"amd64", "x86_64"}:
        return "x86_64"
    if normalized in {"aarch64", "arm64"}:
        return "aarch64"
    return _ascii_or_unknown(normalized)


def _cpu_facts(logical_cpus: int) -> tuple[str, str, int, frozenset[str]]:
    model = platform.processor() or "unknown"
    microcode = "unknown"
    physical: set[tuple[str, str]] = set()
    flags: set[str] = set()
    try:
        current_physical = "0"
        current_core = "0"
        for line in Path("/proc/cpuinfo").read_text(encoding="ascii").splitlines():
            key, separator, raw = line.partition(":")
            if not separator:
                continue
            value = raw.strip()
            if key.strip() == "model name" and model == "unknown":
                model = value
            elif key.strip() == "microcode":
                microcode = value
            elif key.strip() == "physical id":
                current_physical = value
            elif key.strip() == "core id":
                current_core = value
                physical.add((current_physical, current_core))
            elif key.strip() in {"flags", "Features"}:
                flags.update(value.split())
    except OSError:
        pass
    physical_cores = len(physical) if physical else logical_cpus
    return (
        _ascii_or_unknown(model),
        _ascii_or_unknown(microcode),
        max(1, physical_cores),
        frozenset(flags),
    )


def _memory_bytes() -> int | None:
    try:
        for line in Path("/proc/meminfo").read_text(encoding="ascii").splitlines():
            if line.startswith("MemTotal:"):
                parts = line.split()
                if len(parts) == 3 and parts[2] == "kB":
                    return int(parts[1]) * 1024
    except OSError:
        pass
    try:
        sysconf = getattr(os, "sysconf", None)
        if not callable(sysconf):
            return None
        page_size = sysconf("SC_PAGE_SIZE")
        page_count = sysconf("SC_PHYS_PAGES")
        if isinstance(page_size, int) and isinstance(page_count, int):
            return page_size * page_count
    except (AttributeError, OSError, ValueError):
        pass
    return None


def _affinity(logical_cpus: int) -> tuple[int, ...]:
    getter = getattr(os, "sched_getaffinity", None)
    if getter is not None:
        try:
            values = tuple(sorted(getter(0)))
            if values:
                return values
        except OSError:
            pass
    return tuple(range(logical_cpus))


def _numa_nodes() -> int:
    root = Path("/sys/devices/system/node")
    try:
        return max(1, sum(1 for entry in root.iterdir() if re.fullmatch(r"node[0-9]+", entry.name)))
    except OSError:
        return 1


def _process_numa_policies() -> tuple[str, str]:
    """Capture process CPU/memory-node constraints without device identifiers."""

    cpu_allowed: str | None = None
    memory_allowed: str | None = None
    try:
        for line in Path("/proc/self/status").read_text(encoding="ascii").splitlines():
            key, separator, raw = line.partition(":")
            if not separator:
                continue
            value = raw.strip()
            if key == "Cpus_allowed_list":
                cpu_allowed = value
            elif key == "Mems_allowed_list":
                memory_allowed = value
    except OSError:
        return ("unavailable", "unavailable")
    if (
        cpu_allowed is None
        or memory_allowed is None
        or re.fullmatch(r"[0-9,-]+", cpu_allowed) is None
        or re.fullmatch(r"[0-9,-]+", memory_allowed) is None
    ):
        return ("unavailable", "unavailable")
    return (f"allowed-{cpu_allowed}", f"allowed-{memory_allowed}")


def _filesystem(path: Path) -> str:
    try:
        completed = subprocess.run(
            ("stat", "-f", "-c", "%T", "--", str(path)),
            capture_output=True,
            check=False,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"
    value = completed.stdout.strip().lower()
    if (
        completed.returncode != 0
        or completed.stderr
        or not value
        or not value.isascii()
        or re.fullmatch(r"[a-z0-9_.+-]+", value) is None
    ):
        return "unknown"
    return value


def _governor(cpu: int) -> str:
    return _read_one_line(
        Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor"),
        "unavailable",
    )


def _turbo_state() -> str:
    no_turbo = _read_one_line(Path("/sys/devices/system/cpu/intel_pstate/no_turbo"), "")
    if no_turbo in {"0", "1"}:
        return "enabled" if no_turbo == "0" else "disabled"
    boost = _read_one_line(Path("/sys/devices/system/cpu/cpufreq/boost"), "")
    if boost in {"0", "1"}:
        return "enabled" if boost == "1" else "disabled"
    return "unavailable"


def _smt_state(logical: int, physical: int) -> str:
    active = _read_one_line(Path("/sys/devices/system/cpu/smt/active"), "")
    if active in {"0", "1"}:
        return "enabled" if active == "1" else "disabled"
    return "enabled" if logical > physical else "disabled"


def _virtualization(system: str, release: str, flags: frozenset[str]) -> str:
    lowered = release.lower()
    if system != "Linux":
        return "detected"
    if "microsoft" in lowered or "wsl" in lowered or "hypervisor" in flags:
        return "detected"
    try:
        completed = subprocess.run(
            ("systemd-detect-virt",),
            capture_output=True,
            check=False,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"
    output = completed.stdout.strip()
    if completed.stderr or not output or not output.isascii() or "\n" in output:
        return "unknown"
    if completed.returncode == 1 and output == "none":
        return "none"
    if completed.returncode == 0 and output != "none":
        return "detected"
    return "unknown"


def _read_one_line(path: Path, fallback: str) -> str:
    try:
        value = path.read_text(encoding="ascii").strip()
    except OSError:
        return fallback
    return _ascii_or_unknown(value) if value else fallback


def _ascii_or_unknown(value: str) -> str:
    encoded = value.encode("ascii", errors="replace").decode("ascii")
    return " ".join(encoded.split()) or "unknown"
