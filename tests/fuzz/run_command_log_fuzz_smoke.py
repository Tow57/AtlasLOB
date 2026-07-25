"""Run deterministic bounded smoke campaigns for the persistence decoders."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

HEADER_SEED = (
    "41544c534c4730310001000601020304000000980000003800112233445566778899aabbccddeeff"
    "00000000000000010000000000000064000000020000000700000000000003e80000000000000005"
    "00000000000000100000000900000000000007d0000000000000000a0000000000000020"
    "f93e001822f68d814b1cde645bb290e0efec18c6be81a4f38d6255d4e6052d7e811c888f"
)

RECORD_SEEDS = (
    (
        "000000660000002400010102000000000000000100040000000000000001"
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
        "ffffffffffffffffffffffff00000007fffefd018000000000000000ffffffffffffffffababe47a"
    ),
    (
        "000000660000002400010101000000000000000200000000000000000002"
        "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
        "000000010000000000000002000000070102020000000000000000000000000000000003df3fcfd8"
    ),
    (
        "000000520000001000010201000000000000000300000000000000000004"
        "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"
        "0000000b00000000000000020000000712401f73"
    ),
    (
        "0000006a00000028000103020000000000000004000f0000000000000001"
        "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
        "0000000b0000000000000002ffffffffffffffff000000097fffffffffffffffffffffffffffffff"
        "67e69f89"
    ),
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


def write_seed(directory: Path, name: str, encoded: str) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / name).write_bytes(bytes.fromhex(encoded))


def run_fuzzer(executable: Path, corpus: Path, runs: int, max_length: int) -> None:
    subprocess.run(
        [
            str(executable),
            f"-runs={runs}",
            f"-max_len={max_length}",
            "-seed=1096043347",
            "-print_final_stats=1",
            f"-artifact_prefix={corpus / 'artifact-'}",
            str(corpus),
        ],
        check=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--header-fuzzer", required=True, type=Path)
    parser.add_argument("--record-fuzzer", required=True, type=Path)
    parser.add_argument("--snapshot-fuzzer", required=True, type=Path)
    parser.add_argument("--corpus-dir", required=True, type=Path)
    parser.add_argument("--runs", type=int, default=1_000)
    arguments = parser.parse_args()
    if arguments.runs <= 0:
        parser.error("--runs must be positive")

    header_corpus = arguments.corpus_dir / "log-header"
    record_corpus = arguments.corpus_dir / "log-record"
    write_seed(header_corpus, "canonical-atlslg01", HEADER_SEED)
    for index, encoded in enumerate(RECORD_SEEDS, start=1):
        write_seed(record_corpus, f"canonical-record-{index}", encoded)
    snapshot_corpus = arguments.corpus_dir / "snapshot"
    write_seed(snapshot_corpus, "canonical-atlssn01", SNAPSHOT_SEED)

    run_fuzzer(
        arguments.header_fuzzer,
        header_corpus,
        arguments.runs,
        1024 * 1024,
    )
    run_fuzzer(
        arguments.record_fuzzer,
        record_corpus,
        arguments.runs,
        64 * 1024,
    )
    run_fuzzer(
        arguments.snapshot_fuzzer,
        snapshot_corpus,
        arguments.runs,
        1024 * 1024,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
