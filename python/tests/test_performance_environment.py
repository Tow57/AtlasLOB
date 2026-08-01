from __future__ import annotations

import getpass
import os
import platform
import subprocess
from dataclasses import replace
from pathlib import Path

import pytest
from atlaslob.performance import environment
from atlaslob.performance.build_receipt import BuildReceipt
from atlaslob.performance.schemas import environment_to_dict


def _stat_result(value: str) -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess(
        args=("stat",),
        returncode=0,
        stdout=value,
        stderr="",
    )


def _official_build_receipt() -> BuildReceipt:
    return BuildReceipt(
        sha256="a" * 64,
        compiler="clang version 18",
        compiler_flags=("-O3",),
        target_profiles=(("atlas_bench_runner.0000", "-O3"),),
        build_type="Release",
        optimization="O3",
        ndebug=True,
        frame_pointers=True,
        invariants=False,
        sanitizers=False,
        lto=False,
        benchmark_build=True,
        warnings_as_errors=True,
        debug_symbols=True,
        cxx20=True,
        release_flags_locked=True,
    )


def _patch_official_host(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(
        environment, "derive_build_receipt", lambda *_args: _official_build_receipt()
    )
    monkeypatch.setattr(
        environment,
        "_git",
        lambda _root, *args: "b" * 40 if args == ("rev-parse", "HEAD") else None,
    )
    monkeypatch.setattr(environment, "_git_dirty", lambda _root: False)
    monkeypatch.setattr(platform, "system", lambda: "Linux")
    monkeypatch.setattr(platform, "machine", lambda: "x86_64")
    monkeypatch.setattr(platform, "release", lambda: "6.8.0-generic")
    monkeypatch.setattr(environment, "_os_name", lambda _system: "Ubuntu 24.04 LTS")
    monkeypatch.setattr(os, "cpu_count", lambda: 16)
    monkeypatch.setattr(
        environment,
        "_cpu_facts",
        lambda _logical: ("AMD Ryzen", "0x1", 8, frozenset()),
    )
    monkeypatch.setattr(environment, "_memory_bytes", lambda: 64 * 1024**3)
    monkeypatch.setattr(environment, "_affinity", lambda _logical: (1,))
    monkeypatch.setattr(environment, "_numa_nodes", lambda: 1)
    monkeypatch.setattr(environment, "_process_numa_policies", lambda: ("allowed-1", "allowed-0"))
    monkeypatch.setattr(environment, "_governor", lambda _cpu: "performance")
    monkeypatch.setattr(environment, "_turbo_state", lambda: "enabled")
    monkeypatch.setattr(environment, "_smt_state", lambda _logical, _physical: "enabled")
    monkeypatch.setattr(environment, "_virtualization", lambda *_args: "none")
    monkeypatch.setattr(environment, "_run", lambda _command: "perf version 6.8")


def test_public_aliases_reject_embedded_ambient_hostname_and_username(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(platform, "node", lambda: "Lucas-Workstation")
    monkeypatch.setattr(getpass, "getuser", lambda: "lucas")
    monkeypatch.setenv("USERNAME", "lucas")
    monkeypatch.delenv("USER", raising=False)
    monkeypatch.delenv("LOGNAME", raising=False)

    with pytest.raises(ValueError, match="ambient username or hostname"):
        environment._validate_public_alias("ryzen-lucas-ubuntu", "host_class")
    with pytest.raises(ValueError, match="ambient username or hostname"):
        environment._validate_public_alias("lab-lucas-workstation-a", "host_class")
    with pytest.raises(ValueError, match="ambient username or hostname"):
        environment._validate_public_alias("local-lucas-ssd", "storage_class", informative=True)


def test_public_aliases_accept_reusable_hardware_classes(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(platform, "node", lambda: "private-machine-name")
    monkeypatch.setattr(getpass, "getuser", lambda: "private-user")
    monkeypatch.setenv("USER", "private-user")
    monkeypatch.delenv("USERNAME", raising=False)
    monkeypatch.delenv("LOGNAME", raising=False)

    environment._validate_public_alias(
        "ryzen-9800x3d-64g-ubuntu2404-a",
        "host_class",
    )
    environment._validate_public_alias("local-nvme-ssd", "storage_class", informative=True)


def test_hosted_ci_aliases_do_not_embed_the_runner_account(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(platform, "node", lambda: "fv-az1234-567")
    monkeypatch.setattr(getpass, "getuser", lambda: "runner")
    monkeypatch.setenv("USER", "runner")
    monkeypatch.delenv("USERNAME", raising=False)
    monkeypatch.delenv("LOGNAME", raising=False)

    environment._validate_public_alias(
        "github-hosted-ubuntu2404-x86-64",
        "host_class",
    )
    environment._validate_public_alias(
        "ephemeral-hosted-storage",
        "storage_class",
        informative=True,
    )
    with pytest.raises(ValueError, match="ambient username or hostname"):
        environment._validate_public_alias(
            "ephemeral-hosted-runner",
            "storage_class",
            informative=True,
        )


@pytest.mark.parametrize("value", ("", "unknown", "unavailable"))
def test_storage_class_requires_an_informative_alias(
    monkeypatch: pytest.MonkeyPatch,
    value: str,
) -> None:
    monkeypatch.setattr(platform, "node", lambda: "private-machine-name")
    monkeypatch.setattr(getpass, "getuser", lambda: "private-user")
    monkeypatch.delenv("USERNAME", raising=False)
    monkeypatch.delenv("USER", raising=False)
    monkeypatch.delenv("LOGNAME", raising=False)

    with pytest.raises(ValueError, match="sanitized lowercase alias"):
        environment._validate_public_alias(value, "storage_class", informative=True)


@pytest.mark.parametrize(
    ("output", "expected"),
    (
        ("ext2/ext3\n", "ext2/ext3"),
        ("ext4\n", "ext4"),
        ("xfs\n", "xfs"),
        ("btrfs\n", "btrfs"),
    ),
)
def test_filesystem_accepts_canonical_stat_tokens(
    monkeypatch: pytest.MonkeyPatch,
    output: str,
    expected: str,
) -> None:
    monkeypatch.setattr(
        subprocess,
        "run",
        lambda *_args, **_kwargs: _stat_result(output),
    )

    assert environment._filesystem(Path("benchmark")) == expected


@pytest.mark.parametrize(
    "output",
    (
        "",
        "/ext4\n",
        "ext4/\n",
        "ext2//ext3\n",
        " ext4\n",
        "ext4 \n",
        "ext 4\n",
        "ext4\t\n",
        "ext4\nextra\n",
        "ext4\n\n",
        "EXT4\n",
        "ext4;id\n",
        "ext4|id\n",
        "ext4&run\n",
        "ext4@host\n",
        "ext4\\path\n",
    ),
)
def test_filesystem_rejects_noncanonical_stat_tokens(
    monkeypatch: pytest.MonkeyPatch,
    output: str,
) -> None:
    monkeypatch.setattr(
        subprocess,
        "run",
        lambda *_args, **_kwargs: _stat_result(output),
    )

    assert environment._filesystem(Path("benchmark")) == "unknown"


def test_official_capture_accepts_slash_separated_filesystem(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    _patch_official_host(monkeypatch)
    binary = tmp_path / "atlas_bench_runner"
    binary.write_bytes(b"runner")
    monkeypatch.setattr(
        subprocess,
        "run",
        lambda *_args, **_kwargs: _stat_result("ext2/ext3\n"),
    )

    manifest = environment.capture_environment(
        binary,
        build_directory=tmp_path,
        host_class="benchmark-host",
        storage_class="local-nvme-ext4",
        smt_sibling_idle=True,
        repository=tmp_path,
        request_official=True,
    )

    assert manifest.classification == "official"
    assert manifest.filesystem == "ext2/ext3"
    assert not manifest.limitations
    ordinary = replace(manifest, filesystem="ext4", host_context_sha256="")
    slash_fields = environment_to_dict(manifest)
    ordinary_fields = environment_to_dict(ordinary)
    assert {
        key: value
        for key, value in slash_fields.items()
        if key not in {"filesystem", "host_context_sha256"}
    } == {
        key: value
        for key, value in ordinary_fields.items()
        if key not in {"filesystem", "host_context_sha256"}
    }
