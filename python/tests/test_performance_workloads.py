from __future__ import annotations

import os
from collections import Counter
from dataclasses import replace
from pathlib import Path
from typing import NotRequired, TypedDict

import pytest
from atlaslob.domain import (
    CancelOrder,
    Command,
    EngineSnapshot,
    InstrumentConfig,
    MatchingConfig,
    NewOrder,
    OrderType,
    ReferenceResult,
    ReplaceOrder,
    Side,
    TimeInForce,
)
from atlaslob.performance import workloads as workloads_module
from atlaslob.performance.schemas import (
    canonical_json_bytes,
    validate_workload_parameters,
    workload_to_dict,
    write_canonical_document,
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
    _maximal_count_vector,
    _resolved_parameters,
    build_workload_spec,
    load_benchmark_plan,
    materialize_benchmark_plan,
    materialize_workload,
    preflight_benchmark_plan,
    verify_workload_manifest,
)
from atlaslob.reference import ReferenceEngine
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
    for candidate in (
        NATIVE_LOG_MATERIALIZER,
        REPOSITORY / "build" / "release-benchmark-clang" / "atlas_bench_log_materializer",
    ):
        if candidate.is_file():
            return candidate
    pytest.skip("native benchmark log materializer has not been built")


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


@pytest.mark.parametrize("workload_id", ("W04", "W05", "W06", "W07", "W09"))
def test_eager_and_bulk_materialization_are_byte_identical(
    tmp_path: Path,
    workload_id: str,
) -> None:
    eager_directory = tmp_path / "eager"
    bulk_directory = tmp_path / "bulk"
    shape = _shape(workload_id)

    eager_path, eager = materialize_workload(
        workload_id,
        eager_directory,
        seed=23,
        eager_invariant_checks=True,
        **shape,
    )
    bulk_path, bulk = materialize_workload(
        workload_id,
        bulk_directory,
        seed=23,
        **shape,
    )

    assert eager == bulk
    assert eager_path.read_bytes() == bulk_path.read_bytes()
    assert (eager_directory / eager.stream_file).read_bytes() == (
        bulk_directory / bulk.stream_file
    ).read_bytes()
    assert (
        eager.stream_sha256,
        eager.expected_event_digest,
        eager.expected_final_digest,
        eager.operation_distribution,
        eager.after_preload_active_order_count,
        eager.measured_start_active_order_count,
        eager.final_active_order_count,
        eager.expected_committed,
        eager.expected_rejected,
        eager.expected_events,
    ) == (
        bulk.stream_sha256,
        bulk.expected_event_digest,
        bulk.expected_final_digest,
        bulk.operation_distribution,
        bulk.after_preload_active_order_count,
        bulk.measured_start_active_order_count,
        bulk.final_active_order_count,
        bulk.expected_committed,
        bulk.expected_rejected,
        bulk.expected_events,
    )


def _assert_router_active_identity_exact(router: ReferenceRouter) -> None:
    expected = {
        order_id: (order.instrument_id, order.client_id)
        for book in router._books.values()
        for order_id, order in book._orders.items()
    }
    actual = {
        order_id: (identity.instrument_id, identity.client_id)
        for order_id, identity in router._active.items()
    }
    assert actual == expected
    router.assert_invariants()


def test_bulk_router_projects_every_active_identity_transition_from_events() -> None:
    router = ReferenceRouter(
        (InstrumentConfig(1, MatchingConfig(max_active_orders=32)),),
        check_invariants=False,
    )
    commands: tuple[Command, ...] = (
        NewOrder(1, 1, 1, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 1),
        NewOrder(1, 1, 1, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 1),
        NewOrder(2, 2, 1, Side.SELL, OrderType.LIMIT, TimeInForce.GTC, 110, 1),
        CancelOrder(1, 1, 1),
        NewOrder(3, 3, 1, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 99, 1),
        ReplaceOrder(3, 3, 4, 1, 98, 1),
        NewOrder(9, 6, 1, Side.BUY, OrderType.MARKET, TimeInForce.IOC, None, 1),
        NewOrder(9, 7, 1, Side.BUY, OrderType.LIMIT, TimeInForce.IOC, 50, 1),
        NewOrder(9, 8, 1, Side.SELL, OrderType.MARKET, TimeInForce.IOC, None, 1),
        CancelOrder(9, 999, 1),
    )

    for command in commands:
        result = router.execute(command)
        assert result.error is None
        _assert_router_active_identity_exact(router)
    assert router.active_order_count == 0


@pytest.mark.parametrize(("eager", "expected_calls"), ((False, 5), (True, 85)))
def test_materialization_invariant_check_policy_has_promised_boundaries(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    eager: bool,
    expected_calls: int,
) -> None:
    calls = 0
    original = ReferenceRouter.assert_invariants

    def counted(router: ReferenceRouter) -> None:
        nonlocal calls
        calls += 1
        original(router)

    monkeypatch.setattr(ReferenceRouter, "assert_invariants", counted)
    materialize_workload(
        "W04",
        tmp_path,
        seed=31,
        preload_commands=20,
        warmup_commands=20,
        measured_commands=40,
        active_order_target=20,
        eager_invariant_checks=eager,
    )
    assert calls == expected_calls


@pytest.mark.parametrize(("eager", "expected_calls"), ((False, 5), (None, 85)))
def test_deep_verification_policy_defaults_to_eager_and_checks_bulk_boundaries(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    eager: bool | None,
    expected_calls: int,
) -> None:
    manifest_path, _ = materialize_workload(
        "W04",
        tmp_path,
        seed=43,
        preload_commands=20,
        warmup_commands=20,
        measured_commands=40,
        active_order_target=20,
    )
    calls = 0
    original = ReferenceRouter.assert_invariants

    def counted(router: ReferenceRouter) -> None:
        nonlocal calls
        calls += 1
        original(router)

    monkeypatch.setattr(ReferenceRouter, "assert_invariants", counted)
    if eager is None:
        verify_workload_manifest(manifest_path)
    else:
        verify_workload_manifest(manifest_path, eager_invariant_checks=eager)
    assert calls == expected_calls


def test_default_router_and_engine_remain_eager(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    router_calls = 0
    engine_calls = 0
    original_router = ReferenceRouter.assert_invariants
    original_engine = ReferenceEngine.assert_invariants

    def counted_router(router: ReferenceRouter) -> None:
        nonlocal router_calls
        router_calls += 1
        original_router(router)

    def counted_engine(engine: ReferenceEngine) -> None:
        nonlocal engine_calls
        engine_calls += 1
        original_engine(engine)

    monkeypatch.setattr(ReferenceRouter, "assert_invariants", counted_router)
    monkeypatch.setattr(ReferenceEngine, "assert_invariants", counted_engine)
    router = ReferenceRouter((InstrumentConfig(1),))
    router.execute(NewOrder(1, 1, 1, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 1))
    router.execute(CancelOrder(1, 1, 1))

    assert router_calls == 3
    assert engine_calls == 3


def test_bulk_materialization_detects_corruption_at_preload_boundary_and_cleans_staging(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    original = ReferenceRouter.execute

    def corrupt_after_preload(
        router: ReferenceRouter,
        command: Command,
    ) -> ReferenceResult:
        result = original(router, command)
        if router.next_sequence == 21:
            book = router._books[1]
            price = next(iter(book._bid_aggregates))
            book._bid_aggregates[price] += 1
        return result

    monkeypatch.setattr(ReferenceRouter, "execute", corrupt_after_preload)
    with pytest.raises(RuntimeError, match="cached aggregate"):
        materialize_workload(
            "W04",
            tmp_path,
            seed=37,
            preload_commands=20,
            warmup_commands=20,
            measured_commands=40,
            active_order_target=20,
        )
    assert tuple(tmp_path.iterdir()) == ()


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


def test_official_w09_4096_parameters_fit_the_bounded_schema() -> None:
    plan = load_benchmark_plan(REPOSITORY / "benchmarks" / "plans" / "phase5-study-v1.json")
    point = next(point for point in plan.points if point.point_id == "study-w09-i4096")
    command_count = point.preload_commands + point.warmup_commands + point.measured_commands
    spec = build_workload_spec(
        point.workload_id,
        command_count=command_count,
        instrument_count=point.instrument_count,
        active_order_target=point.active_order_target,
        sweep_depth=point.sweep_depth,
    )
    actual = Counter(
        command.instrument_id
        for command in _iter_benchmark_commands(
            point.workload_id,
            spec,
            point.seed,
            active_order_target=point.active_order_target,
            sweep_depth=point.sweep_depth,
            preload_commands=point.preload_commands,
            warmup_commands=point.warmup_commands,
            measured_commands=point.measured_commands,
        )
    )
    actual_counts = tuple(
        actual[instrument_id] for instrument_id in range(1, point.instrument_count + 1)
    )
    parameters = _resolved_parameters(
        point.workload_id,
        spec,
        active_order_target=point.active_order_target,
        instrument_count=point.instrument_count,
        sweep_depth=point.sweep_depth,
        preload_commands=point.preload_commands,
        warmup_commands=point.warmup_commands,
        measured_commands=point.measured_commands,
        after_preload_active_order_count=point.active_order_target,
        measured_start_active_order_count=point.active_order_target,
        actual_instrument_command_counts=actual_counts,
    )
    values = dict(parameters)

    assert sum(actual_counts) == command_count == 2_327_680
    assert len(values["actual_instrument_command_counts"]) == 16_387
    assert len(values["stream_command_budgets"]) == 16_387
    assert len(values["w09_active_order_counts"]) == 12_291
    validate_workload_parameters(parameters)


def test_phase5_study_manifest_preflight_generates_no_stream_commands(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def forbid_generation(*_args: object, **_kwargs: object) -> None:
        raise AssertionError("preflight generated stream commands")

    monkeypatch.setattr(workloads_module, "_iter_benchmark_commands", forbid_generation)
    preflight_benchmark_plan(REPOSITORY / "benchmarks" / "plans" / "phase5-study-v1.json")
    maximal = _maximal_count_vector(4_096, 100_000_000)
    assert len(maximal) == 4_096
    assert sum(maximal) == 100_000_000
    assert len(",".join(str(count) for count in maximal)) == 25_231


def test_w09_maximum_catalog_round_trips_and_deeply_verifies_exact_counts(
    tmp_path: Path,
) -> None:
    manifest_path, manifest = materialize_workload(
        "W09",
        tmp_path,
        seed=41,
        preload_commands=4_096,
        warmup_commands=2,
        measured_commands=2,
        active_order_target=4_096,
        instrument_count=4_096,
    )
    verified = verify_workload_manifest(manifest_path, eager_invariant_checks=False)
    assert verified == manifest
    actual_counts = tuple(
        int(count)
        for count in dict(manifest.parameters)["actual_instrument_command_counts"].split(",")
    )
    assert len(actual_counts) == 4_096
    assert sum(actual_counts) == manifest.command_count

    tampered_counts = list(actual_counts)
    tampered_counts[0] += 1
    tampered_counts[1] -= 1
    tampered_value = ",".join(str(count) for count in tampered_counts)
    tampered = replace(
        manifest,
        parameters=tuple(
            (name, tampered_value if name == "actual_instrument_command_counts" else value)
            for name, value in manifest.parameters
        ),
    )
    tampered_path = tmp_path / "tampered.json"
    write_canonical_document(tampered_path, tampered)
    with pytest.raises(ValueError, match="resolved parameters are not reproducible"):
        verify_workload_manifest(tampered_path, eager_invariant_checks=False)
    assert (tmp_path / manifest.stream_file).read_bytes() == (
        tmp_path / verified.stream_file
    ).read_bytes()
    assert (
        verified.stream_sha256,
        verified.expected_event_digest,
        verified.expected_final_digest,
    ) == (
        manifest.stream_sha256,
        manifest.expected_event_digest,
        manifest.expected_final_digest,
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


def test_latency_plan_requires_one_frozen_stride_sample() -> None:
    with pytest.raises(ValueError, match="at least one frozen-stride sample"):
        BenchmarkPlanPoint(
            point_id="latency-w05",
            tier="smoke",
            workload_id="W05",
            seed=5,
            preload_commands=16,
            warmup_commands=9,
            measured_commands=18,
            active_order_target=16,
            instrument_count=1,
            sweep_depth=8,
            boundaries=("core_latency", "core_throughput"),
        )


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
    materializer = _native_log_materializer()
    bulk_directory = tmp_path / "bulk"
    eager_directory = tmp_path / "eager"
    materialize_benchmark_plan(
        smoke,
        bulk_directory,
        log_materializer=materializer,
    )
    materialize_benchmark_plan(
        smoke,
        eager_directory,
        log_materializer=materializer,
        eager_invariant_checks=True,
    )
    fixture_directory = REPOSITORY / "benchmarks" / "fixtures" / "v1"
    expected = {
        path.name: path.read_bytes() for path in fixture_directory.iterdir() if path.is_file()
    }
    bulk = {path.name: path.read_bytes() for path in bulk_directory.iterdir() if path.is_file()}
    eager = {path.name: path.read_bytes() for path in eager_directory.iterdir() if path.is_file()}
    assert eager == bulk == expected
    assert len(tuple(bulk_directory.glob("w04-*.json"))) == 1
    assert len(tuple(bulk_directory.glob("w10-*.json"))) == 1
