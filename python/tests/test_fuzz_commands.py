from __future__ import annotations

import os
from pathlib import Path

import pytest
from atlaslob.canonical import event_digest
from atlaslob.domain import Command, MatchingConfig
from atlaslob.fuzzing import commands_from_bytes, mutate_one_command
from atlaslob.generation import (
    WorkloadProfile,
    iter_commands,
    resolve_workload_spec,
)
from atlaslob.native import NativeInputConfig, run_native
from atlaslob.reference import ReferenceEngine
from hypothesis import example, given, settings
from hypothesis import strategies as st

INSTRUMENT = 7
CONFIG = MatchingConfig(
    max_order_quantity=1_000,
    tick_increment=1,
    max_active_orders=64,
)
FUZZ_CORPUS = Path("python/corpus/fuzz/v1")

pytestmark = pytest.mark.differential_fuzz


def _executable() -> Path:
    configured = os.environ.get("ATLAS_DIFF_NATIVE")
    if configured is not None:
        candidate = Path(configured)
        if not candidate.is_file():
            raise FileNotFoundError(
                f"ATLAS_DIFF_NATIVE does not name a native evidence executable: {configured}"
            )
        return candidate.resolve()
    candidates = (
        Path("build/dev-gcc/atlas_diff_native.exe"),
        Path("build/dev-gcc/atlas_diff_native"),
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError("build atlas_diff_native or set ATLAS_DIFF_NATIVE")


def _assert_cross_language(commands: tuple[Command, ...]) -> None:
    reference = ReferenceEngine(INSTRUMENT, CONFIG)
    expected = []
    for command in commands:
        result = reference.execute(command)
        expected.append((result, reference.snapshot(), reference.state_digest()))
        reference.assert_invariants()

    native = run_native(
        _executable(),
        NativeInputConfig(INSTRUMENT, CONFIG, snapshot_interval=1),
        commands,
        timeout=60.0,
    )
    assert native.returncode == 0
    assert native.transcript.error is None
    assert len(native.transcript.results) == len(expected)
    for (result, snapshot, digest), native_result in zip(
        expected, native.transcript.results, strict=True
    ):
        assert native_result.snapshot == snapshot
        assert native_result.state.state_digest == digest
        if result.batch is None:
            assert native_result.engine_error == result.error
        else:
            assert native_result.events == result.batch.events
            assert native_result.event_digest == event_digest(result.batch)
    assert native.transcript.final is not None
    assert native.transcript.final.snapshot == reference.snapshot()


@pytest.mark.parametrize(
    "fixture",
    sorted(FUZZ_CORPUS.glob("*.hex")),
    ids=lambda path: path.stem,
)
def test_checked_fuzz_seed_corpus(fixture: Path) -> None:
    encoded = "".join(fixture.read_text(encoding="ascii").split())
    commands = commands_from_bytes(
        bytes.fromhex(encoded),
        instrument_id=INSTRUMENT,
        config=CONFIG,
        max_commands=32,
    )
    _assert_cross_language(commands)


@settings(
    max_examples=30,
    deadline=None,
    database=None,
    derandomize=True,
)
@example(b"\x00")
@example(bytes.fromhex("ff00000000000000"))
@example(bytes.fromhex("20ff0102030405060708090a0b0c0d0e0f"))
@example(bytes(range(64)))
@given(st.binary(min_size=1, max_size=256))
def test_byte_stream_fuzzer_compares_bounded_command_sequences(data: bytes) -> None:
    commands = commands_from_bytes(
        data,
        instrument_id=INSTRUMENT,
        config=CONFIG,
        max_commands=32,
    )

    assert 1 <= len(commands) <= 32
    _assert_cross_language(commands)


@settings(
    max_examples=30,
    deadline=None,
    database=None,
    derandomize=True,
)
@example(b"\x00\x00")
@example(b"\xff\xff")
@example(bytes(range(32)))
@given(st.binary(min_size=2, max_size=64))
def test_single_field_mutation_fuzzer_starts_from_valid_generated_stream(
    data: bytes,
) -> None:
    spec = resolve_workload_spec(
        WorkloadProfile.HOT_LEVEL_CONTENTION,
        command_count=24,
        engine=CONFIG,
        invalid_basis_points=0,
        active_order_target=16,
        snapshot_interval=1,
    )
    original = tuple(iter_commands(spec, 0xF00D))
    mutated = mutate_one_command(original, data, routed_instrument=INSTRUMENT)

    assert len(mutated) == len(original)
    assert sum(left != right for left, right in zip(original, mutated, strict=True)) == 1
    _assert_cross_language(mutated)
