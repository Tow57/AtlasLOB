from __future__ import annotations

import hashlib

import pytest
from atlaslob.canonical import engine_state_bytes, engine_state_digest
from atlaslob.domain import (
    ATLASLOB_SEMANTICS_VERSION,
    CancelOrder,
    EngineSnapshot,
    InstrumentConfig,
    InstrumentSnapshot,
    MatchingConfig,
    MultiInstrumentEngineConfig,
    NewOrder,
    OrderSnapshot,
    OrderType,
    PriceLevelSnapshot,
    ReferenceResult,
    RejectedEvent,
    RejectReason,
    Side,
    TimeInForce,
)
from atlaslob.generation import WorkloadProfile, resolve_workload_spec
from atlaslob.multi_differential import (
    MULTI_DIFFERENTIAL_CAPTURE_SCHEMA,
    capture_reference_router,
    multi_capture_to_dict,
    normalized_economic_state,
    reinterleave_independent,
    reinterleaving_is_equivalent,
)
from atlaslob.multi_generation import (
    MULTI_GENERATOR_VERSION,
    MultiWorkloadSpec,
    iter_multi_generated,
    multi_spec_from_dict,
    multi_spec_to_dict,
)
from atlaslob.multi_shrinking import shrink_multi_failure
from atlaslob.multi_workload import (
    MULTI_WORKLOAD_MANIFEST_SCHEMA,
    build_multi_manifest,
    command_from_dict,
    command_to_dict,
    multi_manifest_from_dict,
    multi_manifest_to_dict,
    workload_stream_bytes,
)
from atlaslob.router import ReferenceRouter


def _golden_snapshot() -> EngineSnapshot:
    first_config = InstrumentConfig(
        7,
        MatchingConfig(max_order_quantity=100, tick_increment=5, max_active_orders=4),
    )
    second_config = InstrumentConfig(
        9,
        MatchingConfig(max_order_quantity=200, tick_increment=10, max_active_orders=6),
    )
    first_order = OrderSnapshot(11, 1, 7, Side.BUY, 100, 5, 1)
    second_order = OrderSnapshot(22, 2, 9, Side.SELL, 110, 7, 3)
    return EngineSnapshot(
        semantics_version=ATLASLOB_SEMANTICS_VERSION,
        last_sequence=3,
        sequence_exhausted=False,
        engine_config=MultiInstrumentEngineConfig(10),
        active_order_count=2,
        catalog=(first_config, second_config),
        instruments=(
            InstrumentSnapshot(
                instrument_id=7,
                active_order_count=1,
                bids=(PriceLevelSnapshot(100, 5, (first_order,)),),
                asks=(),
            ),
            InstrumentSnapshot(
                instrument_id=9,
                active_order_count=1,
                bids=(),
                asks=(PriceLevelSnapshot(110, 7, (second_order,)),),
            ),
        ),
    )


def _commands() -> tuple[NewOrder, ...]:
    return (
        NewOrder(1, 1, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 5),
        NewOrder(2, 2, 9, Side.SELL, OrderType.LIMIT, TimeInForce.GTC, 110, 7),
        NewOrder(3, 3, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 4),
        NewOrder(4, 4, 9, Side.SELL, OrderType.LIMIT, TimeInForce.GTC, 110, 6),
    )


def _generation_spec() -> MultiWorkloadSpec:
    return MultiWorkloadSpec(
        streams=(
            resolve_workload_spec(
                WorkloadProfile.UNIFORM_SYNTHETIC,
                command_count=12,
                instrument_id=7,
            ),
            resolve_workload_spec(
                WorkloadProfile.INVALID_MIX,
                command_count=10,
                instrument_id=9,
            ),
        ),
        engine=MultiInstrumentEngineConfig(2),
    )


def _tiny_spec() -> MultiWorkloadSpec:
    return MultiWorkloadSpec(
        streams=(
            resolve_workload_spec(
                WorkloadProfile.UNIFORM_SYNTHETIC,
                command_count=1,
                instrument_id=7,
            ),
            resolve_workload_spec(
                WorkloadProfile.UNIFORM_SYNTHETIC,
                command_count=1,
                instrument_id=9,
            ),
        ),
        engine=MultiInstrumentEngineConfig(10),
    )


def test_atlsme01_matches_the_shared_independent_cpp_golden() -> None:
    encoded = engine_state_bytes(_golden_snapshot())

    assert encoded.startswith(b"ATLSME01")
    assert engine_state_digest(_golden_snapshot()) == (
        "e0799da2e8fb3148fea7c985e5bf0d3c49238a39b8344608d5c85d00c82bcfe3"
    )
    assert hashlib.sha256(encoded).hexdigest() == engine_state_digest(_golden_snapshot())


def test_v2_generator_is_deterministic_and_does_not_mutate_v1_specs() -> None:
    spec = _generation_spec()

    first_generator = iter_multi_generated(spec, 12345)
    first = tuple(first_generator)
    second_generator = iter_multi_generated(spec, 12345)
    second = tuple(second_generator)

    assert first == second
    assert len(first) == spec.command_count
    assert first_generator.stats == second_generator.stats
    assert first_generator.stats.command_count == spec.command_count
    assert {item.source_instrument_id for item in first} == {7, 9}
    assert MULTI_GENERATOR_VERSION == 2
    assert multi_spec_from_dict(multi_spec_to_dict(spec)) == spec


def test_generated_v2_streams_preserve_router_invariants_across_seeds() -> None:
    spec = _generation_spec()

    for seed in range(16):
        router = ReferenceRouter(spec.catalog, spec.engine)
        commands = tuple(item.command for item in iter_multi_generated(spec, seed))
        sequences = []
        for command in commands:
            result = router.execute(command)
            assert result.batch is not None
            sequences.append(result.batch.command_sequence)
            assert router.validate_invariants()
        assert sequences == list(range(1, spec.command_count + 1))


def test_v2_generator_emits_correlated_global_identity_and_capacity_intents() -> None:
    spec = _generation_spec()
    router = ReferenceRouter(spec.catalog, spec.engine)
    observed: dict[str, ReferenceResult] = {}

    for generated in iter_multi_generated(spec, 9):
        result = router.execute(generated.command)
        observed.setdefault(generated.intent, result)

    expected_rejections = {
        "multi/cross_book_duplicate_id": RejectReason.DUPLICATE_ORDER_ID,
        "multi/routed_cancel_ownership_precedence": RejectReason.OWNERSHIP_MISMATCH,
        "multi/routed_cancel_instrument_mismatch": RejectReason.INSTRUMENT_MISMATCH,
        "multi/routed_replace_ownership_precedence": RejectReason.OWNERSHIP_MISMATCH,
        "multi/routed_replace_instrument_mismatch": RejectReason.INSTRUMENT_MISMATCH,
        "multi/global_capacity_exceeded": RejectReason.CAPACITY_EXCEEDED,
    }
    for intent, reason in expected_rejections.items():
        result = observed[intent]
        batch = result.batch
        assert batch is not None
        event = batch.events[0]
        assert isinstance(event, RejectedEvent)
        assert event.reason == reason

    reused = observed["multi/cross_instrument_terminal_id_reuse"]
    assert reused.committed
    assert router.validate_invariants()


def test_v2_command_and_workload_mappings_are_canonical() -> None:
    spec = _generation_spec()
    generator = iter_multi_generated(spec, 77)
    generated = tuple(generator)
    commands = tuple(item.command for item in generated)

    for command in commands:
        assert command_from_dict(command_to_dict(command)) == command

    encoded = workload_stream_bytes(spec, commands)
    assert encoded.endswith(b"\n")
    assert b'"schema":"atlas_workload_stream_v2"' in encoded.splitlines()[0]
    assert len(encoded.splitlines()) == spec.command_count + 1
    assert b" " not in encoded

    manifest = build_multi_manifest(spec, 77, commands, generator.stats)
    mapping = multi_manifest_to_dict(manifest)
    assert mapping["schema"] == MULTI_WORKLOAD_MANIFEST_SCHEMA
    assert multi_manifest_from_dict(mapping) == manifest


def test_v2_workload_stream_matches_the_frozen_fixture() -> None:
    commands = (
        NewOrder(1, 11, 7, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 5),
        CancelOrder(1, 11, 7),
    )

    assert workload_stream_bytes(_tiny_spec(), commands).decode("ascii") == (
        '{"catalog":[{"instrument_id":"7","max_active_orders":"128",'
        '"max_order_quantity":"1000","tick_increment":"1"},'
        '{"instrument_id":"9","max_active_orders":"128",'
        '"max_order_quantity":"1000","tick_increment":"1"}],'
        '"max_total_active_orders":"10","schema":"atlas_workload_stream_v2"}\n'
        '{"command":{"client_id":"1","instrument_id":"7","limit_price":"100",'
        '"order_id":"11","order_type":1,"quantity":"5","side":1,'
        '"time_in_force":1,"type":"new"},"index":"0",'
        '"schema":"atlas_workload_command_v2"}\n'
        '{"command":{"client_id":"1","instrument_id":"7","order_id":"11",'
        '"type":"cancel"},"index":"1","schema":"atlas_workload_command_v2"}\n'
    )


def test_v2_capture_has_explicit_schema_and_canonical_final_state() -> None:
    snapshot = _golden_snapshot()
    capture = capture_reference_router(
        snapshot.catalog,
        _commands(),
        engine_config=snapshot.engine_config,
    )
    mapping = multi_capture_to_dict(capture)

    assert mapping["schema"] == MULTI_DIFFERENTIAL_CAPTURE_SCHEMA
    assert mapping["semantics_version"] == ATLASLOB_SEMANTICS_VERSION
    assert len(mapping["records"]) == len(_commands())  # type: ignore[arg-type]
    final = mapping["final"]
    assert isinstance(final, dict)
    assert final["state_digest"] == capture.final_state_digest
    assert normalized_economic_state(capture.final_snapshot)


def test_independent_instrument_reinterleaving_normalizes_priorities() -> None:
    catalog = _golden_snapshot().catalog
    commands = _commands()
    reordered = reinterleave_independent(catalog, commands, 2)

    assert set(reordered) == set(commands)
    for seed in range(64):
        assert reinterleaving_is_equivalent(catalog, commands, seed)

    original = capture_reference_router(catalog, commands)
    changed = capture_reference_router(catalog, reordered)
    assert normalized_economic_state(original.final_snapshot) == normalized_economic_state(
        changed.final_snapshot
    )
    if reordered != commands:
        assert original.final_state_digest != changed.final_state_digest


def test_reinterleaving_rejects_nonindependent_workloads() -> None:
    catalog = _golden_snapshot().catalog
    commands = _commands()
    shared_identity = (
        commands[0],
        NewOrder(2, 1, 9, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 1),
    )
    cross_route = (
        commands[0],
        CancelOrder(1, 1, 9),
    )
    unknown_route = (NewOrder(1, 5, 99, Side.BUY, OrderType.LIMIT, TimeInForce.GTC, 100, 1),)

    with pytest.raises(ValueError, match="disjoint"):
        reinterleave_independent(catalog, shared_identity, 1)
    with pytest.raises(ValueError, match="disjoint"):
        reinterleave_independent(catalog, cross_route, 1)
    with pytest.raises(ValueError, match="configured"):
        reinterleave_independent(catalog, unknown_route, 1)
    with pytest.raises(ValueError, match="nonbinding"):
        reinterleave_independent(
            catalog,
            commands,
            1,
            engine_config=MultiInstrumentEngineConfig(9),
        )


def test_multi_shrinker_reduces_without_rewriting_instrument_identity() -> None:
    catalog = _golden_snapshot().catalog
    commands = _commands()

    def evaluate(
        candidate: tuple[NewOrder, ...],
        _remaining: float | None,
    ) -> str | None:
        return (
            "instrument-nine" if any(command.instrument_id == 9 for command in candidate) else None
        )

    result = shrink_multi_failure(
        commands,
        evaluate,  # type: ignore[arg-type]
        "instrument-nine",
        catalog=catalog,
    )

    assert len(result.commands) == 1
    assert result.commands[0].instrument_id == 9
    assert "final_deletion" in result.completed_stages
