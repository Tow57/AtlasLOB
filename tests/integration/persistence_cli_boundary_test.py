"""Exercise persistence CLI behavior at the native process boundary."""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from pathlib import Path

HEADER_SEED = (
    "41544c534c4730310001000601020304000000980000003800112233445566778899aabbccddeeff"
    "00000000000000010000000000000064000000020000000700000000000003e80000000000000005"
    "00000000000000100000000900000000000007d0000000000000000a0000000000000020"
    "f93e001822f68d814b1cde645bb290e0efec18c6be81a4f38d6255d4e6052d7e811c888f"
)

SNAPSHOT_SEED = (
    "41544c53534e30310001000601020304000000000000023100000000000000e10011223344556677"
    "8899aabbccddeeff000000000000000700000000000001c800000000000000000400000000000000"
    "1e00000002000000000000003800000002000000000000014ca370b7f2421f4f54ebfdb409c0e957"
    "70c09f9d7bcd34e979459faac90fb1f396957270029cbc49b98c90553190ea1e918b67fb080eab4cf"
    "ddc5762c2f6f1f2a40000000700000000000000640000000000000005000000000000000a00000009"
    "00000000000000c8000000000000000a000000000000001400000000000001280000000700000000"
    "00000004000000000000000200000000000000010000000000000072000000000000006400000000"
    "0000000c000000000000000200000000000000010000000b00000007010000000000000064000000"
    "0000000005000000000000000100000000000000020000000c000000070100000000000000640000"
    "00000000000700000000000000020000000000000049000000000000005a00000000000000030000"
    "00000000000100000000000000040000000e0000000701000000000000005a000000000000000300"
    "000000000000030000000000000049000000000000006e0000000000000009000000000000000100"
    "000000000000030000000d0000000702000000000000006e00000000000000090000000000000004"
    "000000000000002400000009000000000000000000000000000000000000000000000000f3a24bf4"
)


def run(command: list[str], expected_exit_code: int) -> subprocess.CompletedProcess[bytes]:
    completed = subprocess.run(command, check=False, capture_output=True)
    if completed.returncode != expected_exit_code:
        raise AssertionError(
            f"{command!r} returned {completed.returncode}; "
            f"stdout={completed.stdout!r}; stderr={completed.stderr!r}"
        )
    return completed


def require_lf_only(payload: bytes, stream: str) -> None:
    if b"\r" in payload:
        raise AssertionError(f"{stream} contains a carriage return: {payload!r}")
    if payload and not payload.endswith(b"\n"):
        raise AssertionError(f"{stream} does not end in LF: {payload!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--inspect", required=True, type=Path)
    parser.add_argument("--replay", required=True, type=Path)
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="atlaslob-cli-boundary-") as temporary:
        unicode_directory = Path(temporary) / "路径-🧪"
        unicode_directory.mkdir()
        log_path = unicode_directory / "日志.log"
        log_path.write_bytes(bytes.fromhex(HEADER_SEED))

        inspected = run(
            [str(arguments.inspect), "log", str(log_path), "--json"],
            expected_exit_code=0,
        )
        if inspected.stderr:
            raise AssertionError(f"atlas_inspect wrote stderr: {inspected.stderr!r}")
        require_lf_only(inspected.stdout, "atlas_inspect stdout")
        if json.loads(inspected.stdout)["schema"] != "ATLAS_LOG_REPORT_V1":
            raise AssertionError("atlas_inspect emitted the wrong report schema")

        snapshot_path = unicode_directory / "快照-🧭.snapshot"
        snapshot_path.write_bytes(bytes.fromhex(SNAPSHOT_SEED))
        snapshot_inspection = run(
            [str(arguments.inspect), "snapshot", str(snapshot_path), "--json"],
            expected_exit_code=0,
        )
        if snapshot_inspection.stderr:
            raise AssertionError(
                f"atlas_inspect snapshot wrote stderr: {snapshot_inspection.stderr!r}"
            )
        require_lf_only(snapshot_inspection.stdout, "atlas_inspect snapshot stdout")
        if json.loads(snapshot_inspection.stdout)["schema"] != "ATLAS_SNAPSHOT_REPORT_V1":
            raise AssertionError("atlas_inspect snapshot emitted the wrong report schema")

        replayed = run(
            [str(arguments.replay), str(log_path), "--mode", "verify", "--json"],
            expected_exit_code=0,
        )
        if replayed.stderr:
            raise AssertionError(f"atlas_replay wrote stderr: {replayed.stderr!r}")
        require_lf_only(replayed.stdout, "atlas_replay stdout")
        if json.loads(replayed.stdout)["schema"] != "ATLAS_REPLAY_REPORT_V1":
            raise AssertionError("atlas_replay emitted the wrong report schema")

        empty_snapshot_directory = unicode_directory / "空快照目录"
        empty_snapshot_directory.mkdir()
        recovered = run(
            [
                str(arguments.replay),
                str(log_path),
                "--snapshot-dir",
                str(empty_snapshot_directory),
                "--mode",
                "verify",
                "--json",
            ],
            expected_exit_code=0,
        )
        if recovered.stderr:
            raise AssertionError(f"atlas_replay recovery wrote stderr: {recovered.stderr!r}")
        require_lf_only(recovered.stdout, "atlas_replay recovery stdout")
        recovery_report = json.loads(recovered.stdout)
        if recovery_report["schema"] != "ATLAS_REPLAY_REPORT_V2":
            raise AssertionError("atlas_replay recovery emitted the wrong report schema")
        if recovery_report["recovery_source"] != "full_log":
            raise AssertionError("empty snapshot directory did not fall back to the log")

        torn_path = unicode_directory / "撕裂尾部.log"
        torn_path.write_bytes(bytes.fromhex(HEADER_SEED) + b"\x00")
        repaired_path = unicode_directory / "修复.log"
        repaired = run(
            [
                str(arguments.inspect),
                "repair-tail",
                str(torn_path),
                str(repaired_path),
                "--json",
            ],
            expected_exit_code=0,
        )
        if repaired.stderr:
            raise AssertionError(f"atlas_inspect repair wrote stderr: {repaired.stderr!r}")
        require_lf_only(repaired.stdout, "atlas_inspect repair stdout")
        if repaired_path.read_bytes() != bytes.fromhex(HEADER_SEED):
            raise AssertionError("atlas_inspect repair wrote the wrong valid prefix")

        for executable in (arguments.inspect, arguments.replay):
            usage = run([str(executable)], expected_exit_code=2)
            if usage.stdout:
                raise AssertionError(f"{executable.name} wrote usage to stdout")
            require_lf_only(usage.stderr, f"{executable.name} stderr")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
