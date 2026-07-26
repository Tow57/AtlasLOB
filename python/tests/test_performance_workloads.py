from __future__ import annotations

import os
from pathlib import Path
from typing import NotRequired, TypedDict

import pytest
from atlaslob.domain import (
    CancelOrder,
    Command,
    EngineSnapshot,
    NewOrder,
    OrderType,
    ReplaceOrder,
    Side,
    TimeInForce,
)
from atlaslob.performance import workloads as workloads_module
from atlaslob.performance.schemas import (
    canonical_json_bytes,
    workload_to_dict,
)
from atlaslob.performance.workloads import (
    BENCHMARK_GENERATOR_VERSION,
    DEFERRED_WORKLOADS,
    MAX_BENCHMARK_INSTRUMENTS,
    MAX_BENCHMARK_PLAN_BYTES,
    WORKLOAD_IDS,
    BenchmarkPlan,
    BenchmarkPlanPoint,
    _iter_benchmark_commands,
    build_workload_spec,
    load_benchmark_plan,
    materialize_benchmark_plan,
    materialize_workload,
    verify_workload_manifest,
)
from atlaslob.router import ReferenceRouter

REPOSITORY = Path(__file__).resolve().parents[2]
NATIVE_LOG_MATERIALIZER = (
    REPOSITORY / "build" / "phase5-cpp-runner" / "atlas_bench_log_materializer.exe"
)


class _Shape(TypedDict):
    preload_commands: int
    warmup_commands: int
    measured_commands: int
    active_order_target: int
    instrument_count: NotRequired[int]
    sweep_depth: NotRequired[int]


def _shape(workload_id: str) -> _Shape:
    if workload_id == "W01":
        return {
            "preload_commands": 64,
            "warmup_commands": 2,
            "measured_commands": 4,
            "active_order_target": 64,
        }
    if workload_id == "W02":
        return {
            "preload_commands": 16,
            "warmup_commands": 8,
            "measured_commands": 16,
            "active_order_target": 16,
        }
    if workload_id == "W04":
        return {
            "preload_commands": 20,
            "warmup_commands": 20,
            "measured_commands": 40,
            "active_order_target": 20,
        }
    if workload_id == "W05":
        return {
            "preload_commands": 16,
            "warmup_commands": 18,
            "measured_commands": 18,
            "active_order_target": 16,
            "sweep_depth": 8,
        }
    if workload_id == "W09":
        return {
            "preload_commands": 20,
            "warmup_commands": 8,
            "measured_commands": 16,
            "active_order_target": 20,
            "instrument_count": 4,
        }
    if workload_id == "W10":
        return {
            "preload_commands": 0,
            "warmup_commands": 0,
            "measured_commands": 8,
            "active_order_target": 16,
        }
    if workload_id == "W12":
        return {
            "preload_commands": 16,
            "warmup_commands": 100,
            "measured_commands": 100,
            "active_order_target": 16,
        }
    return {
        "preload_commands": 16,
        "warmup_commands": 4,
        "measured_commands": 12,
        "active_order_target": 16,
    }


def _native_log_materializer() -> Path:
    if not NATIVE_LOG_MATERIALIZER.is_file():
        pytest.skip("native benchmark log materializer has not been built")
    return NATIVE_LOG_MATERIALIZER


def _reference_region_state(
    workload_id: str,
    *,
    seed: int,
    preload_commands: int,
    warmup_commands: int,
    measured_commands: int,
    active_order_target: int,
    sweep_depth: int = 16,
) -> tuple[int, int, EngineSnapshot]:
    total = preload_commands + warmup_commands + measured_commands
    spec = build_workload_spec(
        workload_id,
        command_count=total,
        active_order_target=active_order_target,
        sweep_depth=sweep_depth,
    )
    commands: tuple[Command, ...] = tuple(
        _iter_benchmark_commands(
            workload_id,
            spec,
            seed,
            active_order_target=active_order_target,
            sweep_depth=sweep_depth,
            preload_commands=preload_commands,
            warmup_commands=warmup_commands,
            measured_commands=measured_commands,
        )
    )
    router = ReferenceRouter(spec.catalog, spec.engine)
    preload_active = -1
    pre_measured_active = -1
    for index, command in enumerate(commands, start=1):
        result = router.execute(command)
        assert result.error is None
        assert result.committed
        if index == preload_commands:
            preload_active = router.active_order_count
        if index == preload_commands + warmup_commands:
            pre_measured_active = router.active_order_count
    final = router.snapshot()
    assert isinstance(final, EngineSnapshot)
    return preload_active, pre_measured_active, final


def _commands_for(
    workload_id: str,
    *,
    seed: int,
    preload_commands: int,
    warmup_commands: int,
    measured_commands: int,
    active_order_target: int,
    sweep_depth: int = 16,
    instrument_count: int = 1,
) -> tuple[Command, ...]:
    spec = build_workload_spec(
        workload_id,
        command_count=preload_commands + warmup_commands + measured_commands,
        active_order_target=active_order_target,
        sweep_depth=sweep_depth,
        instrument_count=instrument_count,
    )
    return tuple(
        _iter_benchmark_commands(
            workload_id,
            spec,
            seed,
            active_order_target=active_order_target,
            sweep_depth=sweep_depth,
            preload_commands=preload_commands,
            warmup_commands=warmup_commands,
            measured_commands=measured_commands,
        )
    )


@pytest.mark.parametrize("workload_id", WORKLOAD_IDS)
def test_every_tiny_workload_materializes_and_deeply_reproduces(
    tmp_path: Path,
    workload_id: str,
) -> None:
    manifest_path, manifest = materialize_workload(
        workload_id,
        tmp_path,
        seed=11,
        log_materializer=(_native_log_materializer() if workload_id == "W10" else None),
        **_shape(workload_id),
    )

    assert manifest.generator_version == BENCHMARK_GENERATOR_VERSION
    assert sum(count for _, count in manifest.operation_distribution) == (
        manifest.measured_commands
    )
    assert verify_workload_manifest(manifest_path) == manifest


def test_w11_is_explicitly_deferred() -> None:
    assert DEFERRED_WORKLOADS == {"W11": "gateway fragmentation is deferred to Phase 6"}
    assert "W11" not in WORKLOAD_IDS


def test_w01_grows_then_churns_without_unintended_rejections(tmp_path: Path) -> None:
    path, manifest = materialize_workload(
        "W01",
        tmp_path,
        seed=1,
        preload_commands=64,
        warmup_commands=20,
        measured_commands=40,
        active_order_target=64,
    )
    command_lines = (tmp_path / manifest.stream_file).read_text(encoding="ascii").splitlines()[2:]
    prices = {int(line.split()[8]) for line in command_lines if line.startswith("N ")}

    assert len(prices) == 64
    assert any(line.startswith("C ") for line in command_lines)
    assert manifest.expected_rejected == 0
    assert verify_workload_manifest(path) == manifest


def test_w02_preloads_long_fifo_queues_at_exactly_eight_levels(tmp_path: Path) -> None:
    _, manifest = materialize_workload(
        "W02",
        tmp_path,
        seed=3,
        preload_commands=16,
        warmup_commands=16,
        measured_commands=32,
        active_order_target=16,
    )
    lines = (tmp_path / manifest.stream_file).read_text(encoding="ascii").splitlines()[2:]
    preload = lines[:16]
    prices = [int(line.split()[8]) for line in preload]
    order_prices: dict[int, int] = {}
    canceled_prices: set[int] = set()
    for line in lines:
        fields = line.split()
        if fields[0] == "N":
            order_prices[int(fields[2])] = int(fields[8])
        elif fields[0] == "C":
            canceled_prices.add(order_prices[int(fields[2])])

    assert len(set(prices)) == 8
    assert all(prices.count(price) == 2 for price in set(prices))
    assert canceled_prices == set(prices)
    assert manifest.expected_rejected == 0


def test_w03_keeps_one_order_per_widely_spaced_level() -> None:
    commands = _commands_for(
        "W03",
        seed=17,
        preload_commands=16,
        warmup_commands=4,
        measured_commands=4,
        active_order_target=16,
    )
    preload = commands[:16]
    post_preload = commands[16:]
    assert all(isinstance(command, NewOrder) for command in preload)
    for side in (Side.BUY, Side.SELL):
        prices = sorted(
            command.limit_price
            for command in preload
            if isinstance(command, NewOrder)
            and command.side == side
            and command.limit_price is not None
        )
        assert len(prices) == 8
        assert all(
            right - left == 4_096 for left, right in zip(prices[:-1], prices[1:], strict=True)
        )
    assert all(
        isinstance(command, CancelOrder) if index % 2 == 0 else isinstance(command, NewOrder)
        for index, command in enumerate(post_preload)
    )


def test_w04_measured_region_has_exact_mix_and_preserves_preload_shape(
    tmp_path: Path,
) -> None:
    _, manifest = materialize_workload(
        "W04",
        tmp_path,
        seed=5,
        preload_commands=40,
        warmup_commands=20,
        measured_commands=100,
        active_order_target=40,
    )

    assert dict(manifest.operation_distribution) == {
        "cancel": 35,
        "marketable_flow": 10,
        "nonmarketable_add": 45,
        "replace": 10,
    }
    assert dict(manifest.parameters)["after_preload_active_order_count"] == "40"
    assert dict(manifest.parameters)["measured_start_active_order_count"] == "40"
    assert manifest.expected_rejected == 0
    lines = (tmp_path / manifest.stream_file).read_text(encoding="ascii").splitlines()
    measured = lines[2 + 40 + 20 :]
    passive_sides = {
        int(fields[4])
        for line in measured
        if (fields := line.split())[0] == "N" and int(fields[5]) == int(OrderType.LIMIT)
    }
    aggressor_sides = {
        int(fields[4])
        for line in measured
        if (fields := line.split())[0] == "N" and int(fields[5]) == int(OrderType.MARKET)
    }
    assert passive_sides == {int(Side.BUY), int(Side.SELL)}
    assert aggressor_sides == {int(Side.BUY), int(Side.SELL)}
    preload_active, pre_measured_active, final = _reference_region_state(
        "W04",
        seed=5,
        preload_commands=40,
        warmup_commands=20,
        measured_commands=100,
        active_order_target=40,
    )
    assert preload_active == 40
    assert pre_measured_active == 40
    assert final.active_order_count == 40


@pytest.mark.parametrize("depth", (1, 8, 16, 32, 64))
def test_w05_emits_controlled_sweeps_over_a_preserved_background_shape(
    tmp_path: Path,
    depth: int,
) -> None:
    active = 64
    cycles = 2
    _, manifest = materialize_workload(
        "W05",
        tmp_path,
        seed=7,
        preload_commands=active,
        warmup_commands=depth + 1,
        measured_commands=cycles * (depth + 1),
        active_order_target=active,
        sweep_depth=depth,
    )

    assert dict(manifest.operation_distribution) == {
        "crossing_sweep": cycles,
        "sweep_level_insert": cycles * depth,
    }
    assert dict(manifest.parameters)["measured_sweep_count"] == str(cycles)
    assert manifest.expected_rejected == 0
    preload_active, pre_measured_active, final = _reference_region_state(
        "W05",
        seed=7,
        preload_commands=active,
        warmup_commands=depth + 1,
        measured_commands=cycles * (depth + 1),
        active_order_target=active,
        sweep_depth=depth,
    )
    remaining_ids = {
        order.order_id
        for instrument in final.instruments
        for level in (*instrument.bids, *instrument.asks)
        for order in level.orders
    }
    assert preload_active == active
    assert pre_measured_active == active
    assert final.active_order_count == active
    assert remaining_ids == set(range(1, active + 1))


def test_w06_directly_cancels_and_recreates_unique_adjacent_levels() -> None:
    commands = _commands_for(
        "W06",
        seed=19,
        preload_commands=16,
        warmup_commands=4,
        measured_commands=4,
        active_order_target=16,
    )
    preload = commands[:16]
    post_preload = commands[16:]
    assert all(
        isinstance(command, CancelOrder) if index % 2 == 0 else isinstance(command, NewOrder)
        for index, command in enumerate(post_preload)
    )
    for side in (Side.BUY, Side.SELL):
        prices = sorted(
            command.limit_price
            for command in preload
            if isinstance(command, NewOrder)
            and command.side == side
            and command.limit_price is not None
        )
        assert len(prices) == 8
        assert all(right - left == 1 for left, right in zip(prices[:-1], prices[1:], strict=True))


def test_w07_is_replace_only_after_preload_and_resets_identity() -> None:
    commands = _commands_for(
        "W07",
        seed=23,
        preload_commands=16,
        warmup_commands=2,
        measured_commands=4,
        active_order_target=16,
    )
    replacements = commands[16:]
    assert all(isinstance(command, ReplaceOrder) for command in replacements)
    assert all(
        command.old_order_id != command.new_order_id
        for command in replacements
        if isinstance(command, ReplaceOrder)
    )
    assert len(
        {command.new_order_id for command in replacements if isinstance(command, ReplaceOrder)}
    ) == len(replacements)


def test_w08_ioc_residuals_never_disturb_protected_background() -> None:
    commands = _commands_for(
        "W08",
        seed=29,
        preload_commands=16,
        warmup_commands=2,
        measured_commands=4,
        active_order_target=16,
    )
    for passive, aggressor in zip(commands[16::2], commands[17::2], strict=True):
        assert isinstance(passive, NewOrder)
        assert isinstance(aggressor, NewOrder)
        assert passive.time_in_force == TimeInForce.GTC
        assert passive.quantity == 1
        assert aggressor.order_type == OrderType.LIMIT
        assert aggressor.time_in_force == TimeInForce.IOC
        assert aggressor.quantity == 2
        assert aggressor.side != passive.side
        assert aggressor.limit_price == passive.limit_price
    preload_active, pre_measured_active, final = _reference_region_state(
        "W08",
        seed=29,
        preload_commands=16,
        warmup_commands=2,
        measured_commands=4,
        active_order_target=16,
    )
    assert preload_active == pre_measured_active == final.active_order_count == 16


def test_w09_uses_total_active_semantics_and_skewed_measured_activity(
    tmp_path: Path,
) -> None:
    _, manifest = materialize_workload(
        "W09",
        tmp_path,
        seed=13,
        preload_commands=64,
        warmup_commands=40,
        measured_commands=200,
        active_order_target=64,
        instrument_count=4,
    )
    parameters = dict(manifest.parameters)
    assert parameters["after_preload_active_order_count"] == "64"
    assert parameters["measured_start_active_order_count"] == "64"
    assert parameters["w09_active_order_counts"] == "44,7,7,6"
    assert manifest.max_total_active_orders == 128
    assert manifest.expected_rejected == 0

    lines = (tmp_path / manifest.stream_file).read_text(encoding="ascii").splitlines()
    measured = lines[1 + 4 + 64 + 40 :]
    activity: dict[int, int] = {}
    for line in measured:
        fields = line.split()
        instrument = int(fields[3] if fields[0] in {"N", "C"} else fields[4])
        activity[instrument] = activity.get(instrument, 0) + 1
    assert activity[1] > sum(activity.get(index, 0) for index in (2, 3, 4))


def test_w09_catalog_is_bounded_at_4096_instruments() -> None:
    spec = build_workload_spec(
        "W09",
        command_count=MAX_BENCHMARK_INSTRUMENTS,
        instrument_count=MAX_BENCHMARK_INSTRUMENTS,
        active_order_target=MAX_BENCHMARK_INSTRUMENTS,
    )
    assert len(spec.catalog) == MAX_BENCHMARK_INSTRUMENTS
    with pytest.raises(ValueError, match="between 1 and 4096"):
        build_workload_spec(
            "W09",
            command_count=MAX_BENCHMARK_INSTRUMENTS + 1,
            instrument_count=MAX_BENCHMARK_INSTRUMENTS + 1,
            active_order_target=MAX_BENCHMARK_INSTRUMENTS + 1,
        )


def test_w10_is_an_empty_to_empty_replay_source_with_native_log(
    tmp_path: Path,
) -> None:
    commands = _commands_for(
        "W10",
        seed=31,
        preload_commands=0,
        warmup_commands=0,
        measured_commands=8,
        active_order_target=16,
    )
    for start in range(0, len(commands), 4):
        add, cancel, passive, aggressor = commands[start : start + 4]
        assert isinstance(add, NewOrder)
        assert isinstance(cancel, CancelOrder)
        assert cancel.order_id == add.order_id
        assert isinstance(passive, NewOrder)
        assert isinstance(aggressor, NewOrder)
        assert passive.side == Side.SELL
        assert passive.time_in_force == TimeInForce.GTC
        assert aggressor.side == Side.BUY
        assert aggressor.time_in_force == TimeInForce.IOC
        assert aggressor.limit_price == passive.limit_price
    path, manifest = materialize_workload(
        "W10",
        tmp_path,
        seed=31,
        preload_commands=0,
        warmup_commands=0,
        measured_commands=8,
        active_order_target=16,
        log_materializer=_native_log_materializer(),
    )
    assert manifest.final_active_order_count == 0
    assert manifest.timed_input_file is not None
    assert manifest.timed_input_file.endswith(".atlslg")
    assert (tmp_path / manifest.timed_input_file).read_bytes()[:8] == b"ATLSLG01"
    assert verify_workload_manifest(path) == manifest


def test_w12_uses_a_bounded_limit_ioc_sweep_and_preserves_background() -> None:
    active = 16
    commands = _commands_for(
        "W12",
        seed=37,
        preload_commands=active,
        warmup_commands=100,
        measured_commands=100,
        active_order_target=active,
    )
    cycle = commands[active : active + 100]
    inserted = cycle[:64]
    sweep = cycle[64]
    assert all(
        isinstance(command, NewOrder)
        and command.order_type == OrderType.LIMIT
        and command.time_in_force == TimeInForce.GTC
        for command in inserted
    )
    assert isinstance(sweep, NewOrder)
    assert sweep.order_type == OrderType.LIMIT
    assert sweep.time_in_force == TimeInForce.IOC
    assert sweep.quantity == 64
    inserted_prices = tuple(
        command.limit_price
        for command in inserted
        if isinstance(command, NewOrder) and command.limit_price is not None
    )
    assert len(inserted_prices) == 64
    assert sweep.limit_price in {min(inserted_prices), max(inserted_prices)}
    first_reuse = cycle[70]
    second_reuse = cycle[71]
    assert isinstance(first_reuse, NewOrder)
    assert isinstance(second_reuse, NewOrder)
    assert first_reuse.order_id == second_reuse.order_id
    preload_active, pre_measured_active, final = _reference_region_state(
        "W12",
        seed=37,
        preload_commands=active,
        warmup_commands=100,
        measured_commands=100,
        active_order_target=active,
    )
    assert preload_active == pre_measured_active == final.active_order_count == active


@pytest.mark.parametrize("workload_id", ("W01", "W02", "W05"))
def test_custom_composition_seed_changes_stream_bytes(
    tmp_path: Path,
    workload_id: str,
) -> None:
    shape = _shape(workload_id)
    _, first = materialize_workload(workload_id, tmp_path, seed=1, **shape)
    _, second = materialize_workload(workload_id, tmp_path, seed=2, **shape)
    assert first.stream_sha256 != second.stream_sha256


def test_shape_parameters_coexist_without_filename_collisions(tmp_path: Path) -> None:
    first_path, _ = materialize_workload(
        "W05",
        tmp_path,
        seed=1,
        preload_commands=20,
        warmup_commands=2,
        measured_commands=16,
        active_order_target=20,
        sweep_depth=1,
    )
    second_path, _ = materialize_workload(
        "W05",
        tmp_path,
        seed=1,
        preload_commands=20,
        warmup_commands=9,
        measured_commands=9,
        active_order_target=20,
        sweep_depth=8,
    )

    assert first_path != second_path
    assert first_path.exists() and second_path.exists()
    assert "-d1." in first_path.name and "-d8." in second_path.name


def test_deep_verifier_rejects_canonical_manifest_tampering(tmp_path: Path) -> None:
    _, manifest = materialize_workload(
        "W01",
        tmp_path,
        seed=4,
        preload_commands=64,
        warmup_commands=4,
        measured_commands=12,
        active_order_target=64,
    )
    mapping = workload_to_dict(manifest)
    mapping["expected_events"] = str(manifest.expected_events + 1)
    tampered = tmp_path / "tampered.json"
    tampered.write_bytes(canonical_json_bytes(mapping))

    with pytest.raises(ValueError, match="does not reproduce"):
        verify_workload_manifest(tampered)


def test_deep_verifier_streams_the_command_file(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    manifest_path, _ = materialize_workload(
        "W04",
        tmp_path,
        seed=4,
        preload_commands=20,
        warmup_commands=20,
        measured_commands=40,
        active_order_target=20,
    )
    original = Path.read_bytes

    def guarded_read_bytes(path: Path) -> bytes:
        if path.suffix == ".atlas":
            raise AssertionError("stream verification loaded the entire command file")
        return original(path)

    monkeypatch.setattr(Path, "read_bytes", guarded_read_bytes)
    verify_workload_manifest(manifest_path)


def test_plan_size_is_bounded_before_json_decoding(tmp_path: Path) -> None:
    oversized = tmp_path / "oversized.json"
    oversized.write_bytes(b"x" * (MAX_BENCHMARK_PLAN_BYTES + 1))
    with pytest.raises(ValueError, match="1 MiB"):
        load_benchmark_plan(oversized)


def test_python_plan_requires_the_exact_frozen_batch_sizes() -> None:
    def make_point(batch_sizes: tuple[int, ...]) -> BenchmarkPlanPoint:
        return BenchmarkPlanPoint(
            point_id="python-w04",
            tier="python",
            workload_id="W04",
            seed=1,
            preload_commands=20,
            warmup_commands=20,
            measured_commands=40,
            active_order_target=20,
            instrument_count=1,
            sweep_depth=16,
            boundaries=("python_columns", "python_objects", "python_summary"),
            python_batch_sizes=batch_sizes,
            python_output_modes=("columns", "objects", "summary"),
        )

    with pytest.raises(ValueError, match="exactly 1, 64, 1024, and 65536"):
        make_point((1, 64))
    point = make_point((1, 64, 1_024, 65_536))
    assert point.python_batch_sizes == (1, 64, 1_024, 65_536)


def test_plan_preflights_every_collision_before_materializing(
    tmp_path: Path,
) -> None:
    plan = BenchmarkPlan(
        "preflight",
        (
            BenchmarkPlanPoint(
                "a-w03",
                "smoke",
                "W03",
                1,
                16,
                2,
                2,
                16,
                1,
                16,
                ("core_throughput",),
            ),
            BenchmarkPlanPoint(
                "b-w06",
                "smoke",
                "W06",
                1,
                16,
                2,
                2,
                16,
                1,
                16,
                ("core_throughput",),
            ),
        ),
    )
    collision = tmp_path / "w06-g1-s1-p16-w2-m2-i1-a16-d16.json"
    collision.write_text("sentinel", encoding="ascii")
    with pytest.raises(FileExistsError, match="never overwrites"):
        materialize_benchmark_plan(plan, tmp_path)
    assert tuple(path.name for path in tmp_path.iterdir()) == (collision.name,)


def test_w10_failure_leaves_no_partial_final_artifacts(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    materializer = tmp_path / "materializer"
    materializer.write_bytes(b"placeholder")

    def fail_materialization(
        _executable: Path,
        *,
        output: Path,
        **_kwargs: object,
    ) -> str:
        output.write_bytes(b"ATLSLG01partial")
        raise ValueError("invalid receipt")

    monkeypatch.setattr(
        workloads_module,
        "_materialize_replay_log",
        fail_materialization,
    )
    with pytest.raises(ValueError, match="invalid receipt"):
        materialize_workload(
            "W10",
            tmp_path,
            seed=1,
            preload_commands=0,
            warmup_commands=0,
            measured_commands=8,
            active_order_target=16,
            log_materializer=materializer,
        )
    assert tuple(path.name for path in tmp_path.iterdir()) == (materializer.name,)


def test_publication_failure_rolls_back_every_final_artifact(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    original_link = os.link
    calls = 0

    def fail_second_link(source: Path, destination: Path) -> None:
        nonlocal calls
        calls += 1
        if calls == 2:
            raise OSError("injected publication failure")
        original_link(source, destination)

    monkeypatch.setattr(os, "link", fail_second_link)
    with pytest.raises(OSError, match="injected publication failure"):
        materialize_workload(
            "W03",
            tmp_path,
            seed=1,
            preload_commands=16,
            warmup_commands=2,
            measured_commands=2,
            active_order_target=16,
        )
    assert tuple(tmp_path.iterdir()) == ()


def test_stream_verification_rejects_records_over_1024_bytes(tmp_path: Path) -> None:
    path, manifest = materialize_workload(
        "W03",
        tmp_path,
        seed=41,
        preload_commands=16,
        warmup_commands=2,
        measured_commands=2,
        active_order_target=16,
    )
    (tmp_path / manifest.stream_file).write_bytes(b"x" * 1_024 + b"\n")
    with pytest.raises(ValueError, match="1024-byte"):
        verify_workload_manifest(path)


def test_checked_plans_and_fixture_manifests_are_canonical() -> None:
    smoke = load_benchmark_plan(REPOSITORY / "benchmarks" / "plans" / "ci-smoke-v1.json")
    study = load_benchmark_plan(REPOSITORY / "benchmarks" / "plans" / "phase5-study-v1.json")
    assert {point.workload_id for point in smoke.points} == set(WORKLOAD_IDS)
    assert {point.workload_id for point in study.points} == set(WORKLOAD_IDS)
    assert {point.instrument_count for point in study.points if point.workload_id == "W09"} == {
        1,
        16,
        256,
        4_096,
    }
    assert {point.measured_commands for point in study.points if point.workload_id == "W10"} == {
        1_000_000,
        10_000_000,
    }
    assert {
        point.active_order_target
        for point in study.points
        if point.point_id.startswith("memory-w01-")
    } == {1_000, 65_536, 1_000_000}
    assert {
        point.sweep_depth for point in study.points if point.point_id.startswith("study-w05-d")
    } == {1, 8, 16, 32, 64}
    python_points = tuple(point for point in study.points if point.tier == "python")
    assert len(python_points) == 1
    assert python_points[0].python_batch_sizes == (1, 64, 1_024, 65_536)
    headline = tuple(point for point in study.points if point.tier == "headline")
    assert {point.workload_id for point in headline} == {"W04", "W05"}
    assert all(point.active_order_target == 1_000_000 for point in headline)
    assert all(point.measured_commands <= 5_000_000 for point in headline)
    fixture_directory = REPOSITORY / "benchmarks" / "fixtures" / "v1"
    manifests = tuple(sorted(fixture_directory.glob("*.json")))
    assert len(manifests) == len(WORKLOAD_IDS)
    verified = tuple(verify_workload_manifest(path) for path in manifests)
    assert sum(manifest.workload_id == "W04" for manifest in verified) == 1
    assert sum(manifest.workload_id == "W10" for manifest in verified) == 1


def test_checked_smoke_plan_regenerates_the_exact_fixture_tree(
    tmp_path: Path,
) -> None:
    smoke = load_benchmark_plan(REPOSITORY / "benchmarks" / "plans" / "ci-smoke-v1.json")
    materialize_benchmark_plan(
        smoke,
        tmp_path,
        log_materializer=_native_log_materializer(),
    )
    fixture_directory = REPOSITORY / "benchmarks" / "fixtures" / "v1"
    expected = {
        path.name: path.read_bytes() for path in fixture_directory.iterdir() if path.is_file()
    }
    actual = {path.name: path.read_bytes() for path in tmp_path.iterdir() if path.is_file()}
    assert actual == expected
    assert len(tuple(tmp_path.glob("w04-*.json"))) == 1
    assert len(tuple(tmp_path.glob("w10-*.json"))) == 1
