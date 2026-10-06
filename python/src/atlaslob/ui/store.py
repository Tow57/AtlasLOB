"""Session metadata and single-writer ownership; native logs own all engine state."""

import json
import os
import re
from pathlib import Path
from typing import Any

ID = re.compile(r"^[0-9a-f]{32}$")


def atomic_json(path: Path, value: Any) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, sort_keys=True, separators=(",", ":"))
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    descriptor = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


class Store:
    def __init__(self, root: Path):
        import fcntl

        self.root = root.expanduser().resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.lock = (self.root / ".writer.lock").open("a+")
        try:
            fcntl.flock(self.lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self.lock.close()
            raise RuntimeError("Another AtlasLOB UI owns this session directory.") from None

    def directory(self, session_id: str) -> Path:
        if not ID.fullmatch(session_id):
            raise ValueError("Invalid session identifier.")
        path = self.root / session_id
        if path.is_symlink():
            raise ValueError("Session directories cannot be symbolic links.")
        return path

    def read(self, session_id: str) -> dict[str, Any]:
        path = self.directory(session_id) / "metadata.json"
        if path.is_symlink() or not path.is_file():
            raise ValueError("Session metadata is missing or unsafe.")
        value = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(value, dict) or value.get("schema") != 1 or value.get("id") != session_id:
            raise ValueError("Unsupported session metadata.")
        return value

    def write(self, value: dict[str, Any]) -> None:
        atomic_json(self.directory(value["id"]) / "metadata.json", value)

    def ids(self) -> list[str]:
        return [p.name for p in self.root.iterdir() if ID.fullmatch(p.name)]

    def last_opened(self) -> str | None:
        try:
            value = json.loads((self.root / "library.json").read_text())
            result = value.get("last_opened") if isinstance(value, dict) else None
            return result if isinstance(result, str) and ID.fullmatch(result) else None
        except (OSError, ValueError):
            return None

    def mark_opened(self, session_id: str) -> None:
        atomic_json(self.root / "library.json", {"last_opened": session_id})

    def close(self) -> None:
        self.lock.close()
