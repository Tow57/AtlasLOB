from __future__ import annotations

import getpass
import platform

import pytest
from atlaslob.performance import environment


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
