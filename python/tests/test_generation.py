from __future__ import annotations

from dataclasses import replace

import pytest
from atlaslob.domain import I64_MAX, U64_MAX, MatchingConfig, NewOrder, ReplaceOrder
from atlaslob.generation import (
    SplitMix64,
    WorkloadProfile,
    iter_generated,
    resolve_workload_spec,
    spec_from_dict,
    spec_to_dict,
)
from atlaslob.reference import ReferenceEngine


def test_splitmix64_has_frozen_cross_version_vectors() -> None:
    generator = SplitMix64(0)
    assert [generator.next_u64() for _ in range(5)] == [
        0xE220A8397B1DCDAF,
        0x6E789E6AA1B965F4,
        0x06C45D188009454F,
        0xF88BB8A8724C81EC,
        0x1B39896A51A8749B,
    ]


def test_seed_and_resolved_spec_replay_exactly() -> None:
    spec = resolve_workload_spec(
        WorkloadProfile.TRACE_DRIVEN_SYNTHETIC,
        command_count=1_000,
    )
    first = tuple(iter_generated(spec, 0x123456789ABCDEF0))
    second = tuple(iter_generated(spec, 0x123456789ABCDEF0))
    different = tuple(iter_generated(spec, 0x123456789ABCDEF1))

    assert first == second
    assert first != different


@pytest.mark.parametrize("profile", list(WorkloadProfile))
def test_every_profile_produces_accurate_valid_and_invalid_intents(
    profile: WorkloadProfile,
) -> None:
    spec = resolve_workload_spec(
        profile,
        command_count=1_000,
        invalid_basis_points=2_500,
    )
    engine = ReferenceEngine(spec.instrument_id, spec.engine)
    generated = tuple(iter_generated(spec, 37))

    for item in generated:
        result = engine.execute(item.command)
        assert item.intended_valid is not result.rejected
        engine.assert_invariants()

    intended_invalid = sum(not item.intended_valid for item in generated)
    assert 200 <= intended_invalid <= 300
    assert any(item.intended_valid for item in generated)
    assert any(not item.intended_valid for item in generated)


def test_adversarial_profile_reaches_legal_domain_boundaries() -> None:
    spec = resolve_workload_spec(
        WorkloadProfile.ADVERSARIAL_BOUNDARY,
        command_count=5_000,
    )
    commands = [item.command for item in iter_generated(spec, 99) if item.intended_valid]
    quantities = [
        command.quantity if isinstance(command, NewOrder) else command.new_quantity
        for command in commands
        if isinstance(command, (NewOrder, ReplaceOrder))
    ]
    prices = [
        command.limit_price
        for command in commands
        if isinstance(command, NewOrder) and command.limit_price is not None
    ] + [command.new_limit_price for command in commands if isinstance(command, ReplaceOrder)]
    order_ids = [command.order_id for command in commands if isinstance(command, NewOrder)] + [
        command.new_order_id for command in commands if isinstance(command, ReplaceOrder)
    ]

    assert min(quantities) == 1
    assert max(quantities) == spec.engine.max_order_quantity
    assert min(prices) == spec.engine.tick_increment
    assert max(prices) == I64_MAX
    assert any(order_id > U64_MAX - 1_024 for order_id in order_ids)


def test_resolved_spec_round_trips_without_profile_defaults() -> None:
    spec = resolve_workload_spec(
        WorkloadProfile.INVALID_MIX,
        command_count=12_345,
        invalid_basis_points=4_321,
        snapshot_interval=777,
    )
    encoded = spec_to_dict(spec)

    assert spec_from_dict(encoded) == spec
    assert encoded["invalid_basis_points"] == 4_321
    assert encoded["snapshot_interval"] == "777"
    assert "operation_weights" in encoded
    assert "price_model" in encoded


def test_invalid_mix_covers_shape_state_ownership_tick_and_range_families() -> None:
    spec = resolve_workload_spec(
        WorkloadProfile.INVALID_MIX,
        command_count=5_000,
        engine=MatchingConfig(
            max_order_quantity=1_000,
            tick_increment=5,
            max_active_orders=128,
        ),
        invalid_basis_points=5_000,
        mid_price=10_000,
    )
    generator = iter_generated(spec, 1_234)
    tuple(generator)
    intent_names = {name for name, _ in generator.stats.intents}

    assert {
        "invalid_new_tick",
        "invalid_new_quantity_range",
        "invalid_new_duplicate_id",
        "invalid_cancel_unknown_id",
        "invalid_cancel_ownership",
        "invalid_cancel_instrument_mismatch",
        "invalid_replace_duplicate_new_id",
        "invalid_replace_ownership",
        "invalid_replace_instrument_mismatch",
    } <= intent_names


def test_spec_validation_rejects_unaligned_midpoint_and_stale_schema() -> None:
    spec = resolve_workload_spec(
        WorkloadProfile.UNIFORM_SYNTHETIC,
        command_count=10,
    )
    with pytest.raises(ValueError, match="mid_price"):
        replace(spec, engine=replace(spec.engine, tick_increment=3))

    encoded = spec_to_dict(spec)
    encoded["schema"] = "atlas_workload_spec_v0"
    with pytest.raises(ValueError, match="schema"):
        spec_from_dict(encoded)
