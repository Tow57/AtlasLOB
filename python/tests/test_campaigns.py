from __future__ import annotations

import json
from copy import deepcopy
from pathlib import Path
from typing import cast

import pytest
from atlaslob.campaigns import (
    MAIN_CASE_COUNT,
    MAIN_COMMANDS_PER_CASE,
    MAIN_SNAPSHOT_INTERVAL,
    NIGHTLY_CASE_COUNT,
    NIGHTLY_COMMANDS_PER_CASE,
    NIGHTLY_SNAPSHOT_INTERVAL,
    PR_CASE_COUNT,
    PR_COMMANDS_PER_CASE,
    PR_SNAPSHOT_INTERVAL,
    RELEASE_CASE_COUNT,
    RELEASE_COMMANDS_PER_CASE,
    RELEASE_SANITIZER_CASE_COUNT,
    RELEASE_SANITIZER_COMMANDS_PER_CASE,
    RELEASE_SANITIZER_SNAPSHOT_INTERVAL,
    RELEASE_SNAPSHOT_INTERVAL,
    REQUIRED_PROFILES,
    PredefinedCampaign,
    campaign_from_dict,
    campaign_from_json,
    campaign_json,
    campaign_to_dict,
    checked_campaigns,
    main_campaign,
    nightly_campaign,
    pr_campaign,
    release_campaign,
    release_sanitizer_campaign,
)
from atlaslob.domain import U64_MAX
from atlaslob.generation import PriceModel

_CAMPAIGN_ROOT = Path(__file__).parents[1] / "campaigns" / "v1"


@pytest.mark.parametrize(
    (
        "campaign",
        "expected_tier",
        "expected_mode",
        "expected_case_count",
        "expected_commands",
        "expected_snapshot_interval",
        "expected_repetitions",
    ),
    (
        (
            pr_campaign(),
            "pr",
            "exact",
            PR_CASE_COUNT,
            PR_COMMANDS_PER_CASE,
            PR_SNAPSHOT_INTERVAL,
            1,
        ),
        (
            main_campaign(7),
            "main",
            "exact",
            MAIN_CASE_COUNT,
            MAIN_COMMANDS_PER_CASE,
            MAIN_SNAPSHOT_INTERVAL,
            2,
        ),
        (
            nightly_campaign(7),
            "nightly",
            "compact",
            NIGHTLY_CASE_COUNT,
            NIGHTLY_COMMANDS_PER_CASE,
            NIGHTLY_SNAPSHOT_INTERVAL,
            1,
        ),
        (
            release_campaign(),
            "release",
            "compact",
            RELEASE_CASE_COUNT,
            RELEASE_COMMANDS_PER_CASE,
            RELEASE_SNAPSHOT_INTERVAL,
            1,
        ),
        (
            release_sanitizer_campaign(),
            "release_sanitizer",
            "exact",
            RELEASE_SANITIZER_CASE_COUNT,
            RELEASE_SANITIZER_COMMANDS_PER_CASE,
            RELEASE_SANITIZER_SNAPSHOT_INTERVAL,
            1,
        ),
    ),
)
def test_tier_factories_resolve_the_complete_predefined_policy(
    campaign: PredefinedCampaign,
    expected_tier: str,
    expected_mode: str,
    expected_case_count: int,
    expected_commands: int,
    expected_snapshot_interval: int,
    expected_repetitions: int,
) -> None:
    suite = campaign.suite

    assert suite.tier == expected_tier
    assert suite.mode == expected_mode
    assert len(suite.cases) == expected_case_count
    assert campaign.provenance.seeds == tuple(case.seed for case in suite.cases)
    assert len(set(campaign.provenance.seeds)) == expected_case_count
    assert tuple(case.spec.profile for case in suite.cases) == (
        REQUIRED_PROFILES * expected_repetitions
    )
    assert all(case.spec.command_count == expected_commands for case in suite.cases)
    assert all(case.spec.snapshot_interval == expected_snapshot_interval for case in suite.cases)

    first_profile_round = suite.cases[: len(REQUIRED_PROFILES)]
    distribution_signatures = {
        (
            case.spec.price_model,
            case.spec.operation_weights,
            case.spec.invalid_basis_points,
            case.spec.aggressive_basis_points,
            case.spec.market_basis_points,
            case.spec.boundary_quantity_basis_points,
        )
        for case in first_profile_round
    }
    assert {case.spec.price_model for case in first_profile_round} == set(PriceModel)
    assert len(distribution_signatures) == len(REQUIRED_PROFILES)

    for index, case in enumerate(suite.cases):
        assert case.name == f"{index:02d}-{case.spec.profile.value}"
        assert case.spec.instrument_id == 7
        assert case.spec.engine.max_order_quantity == 1_000
        assert case.spec.engine.tick_increment == 1
        assert case.spec.engine.max_active_orders == 128
        assert case.spec.mid_price == 10_000
        assert case.spec.price_span_ticks == 128
        assert case.spec.active_order_target == 64
        assert case.spec.client_count == 16


def test_rotating_epoch_seed_policy_is_explicit_deterministic_and_domain_separated() -> None:
    first_main = main_campaign(17)
    repeated_main = main_campaign(17)
    next_main = main_campaign(18)
    first_nightly = nightly_campaign(17)

    assert first_main == repeated_main
    assert first_main.provenance.kind == "rotating_epoch"
    assert first_main.provenance.epoch == 17
    assert first_nightly.provenance.kind == "rotating_epoch"
    assert first_nightly.provenance.epoch == 17
    assert first_main.provenance.seeds != next_main.provenance.seeds
    assert set(first_main.provenance.seeds).isdisjoint(next_main.provenance.seeds)
    assert set(first_main.provenance.seeds).isdisjoint(first_nightly.provenance.seeds)

    assert tuple(case.name for case in first_main.suite.cases) == tuple(
        case.name for case in next_main.suite.cases
    )
    assert tuple(case.spec for case in first_main.suite.cases) == tuple(
        case.spec for case in next_main.suite.cases
    )


@pytest.mark.parametrize("invalid_epoch", (-1, U64_MAX + 1, True))
def test_rotating_campaigns_reject_invalid_epochs(invalid_epoch: int) -> None:
    with pytest.raises(ValueError, match="epoch"):
        main_campaign(invalid_epoch)
    with pytest.raises(ValueError, match="epoch"):
        nightly_campaign(invalid_epoch)


def test_release_sanitizer_is_an_exact_subset_of_the_published_seed_set() -> None:
    release = release_campaign()
    sanitizer = release_sanitizer_campaign()

    assert release.provenance.kind == "published"
    assert sanitizer.provenance.kind == "published"
    assert sanitizer.provenance == release.provenance
    assert RELEASE_COMMANDS_PER_CASE >= 10_000_000
    assert RELEASE_SANITIZER_COMMANDS_PER_CASE < RELEASE_COMMANDS_PER_CASE
    assert tuple(case.name for case in sanitizer.suite.cases) == tuple(
        case.name for case in release.suite.cases
    )
    assert tuple(case.spec.profile for case in sanitizer.suite.cases) == tuple(
        case.spec.profile for case in release.suite.cases
    )
    assert tuple(case.seed for case in sanitizer.suite.cases) == tuple(
        case.seed for case in release.suite.cases
    )


def test_checked_campaigns_round_trip_as_canonical_ascii_without_workload_streams() -> None:
    for filename, campaign in checked_campaigns():
        encoded = campaign_json(campaign)

        assert filename.endswith(".json")
        assert campaign_from_json(encoded) == campaign
        assert campaign_from_dict(campaign_to_dict(campaign)) == campaign
        assert encoded.endswith("\n")
        assert "\r" not in encoded
        assert encoded.isascii()
        assert len(encoded) < 100_000
        assert '"commands"' not in encoded


def test_checked_campaign_files_are_generated_byte_for_byte_from_factories() -> None:
    expected = dict(checked_campaigns())
    actual_paths = tuple(sorted(_CAMPAIGN_ROOT.glob("*.json")))

    assert {path.name for path in actual_paths} == set(expected)
    for path in actual_paths:
        expected_campaign = expected[path.name]
        expected_bytes = campaign_json(expected_campaign).encode("ascii")
        actual_bytes = path.read_bytes()

        assert actual_bytes == expected_bytes
        assert campaign_from_json(actual_bytes.decode("ascii")) == expected_campaign


def test_predefined_decoder_rejects_stale_tampered_and_noncanonical_documents() -> None:
    encoded = campaign_to_dict(pr_campaign())

    stale = deepcopy(encoded)
    stale["unexpected"] = True
    with pytest.raises(ValueError, match="fields"):
        campaign_from_dict(stale)

    wrong_policy = deepcopy(encoded)
    wrong_policy["policy_version"] = 2
    with pytest.raises(ValueError, match="policy version"):
        campaign_from_dict(wrong_policy)

    wrong_mode = deepcopy(encoded)
    _dict_field(wrong_mode, "suite")["mode"] = "compact"
    with pytest.raises(ValueError, match="mode"):
        campaign_from_dict(wrong_mode)

    wrong_seed = deepcopy(encoded)
    seed_values = _list_field(_dict_field(wrong_seed, "provenance"), "seeds")
    seed_values[0] = "1"
    with pytest.raises(ValueError, match="seeds"):
        campaign_from_dict(wrong_seed)

    wrong_command_count = deepcopy(encoded)
    _first_spec(wrong_command_count)["command_count"] = "4999"
    with pytest.raises(ValueError, match="spec"):
        campaign_from_dict(wrong_command_count)

    wrong_profile = deepcopy(encoded)
    _first_spec(wrong_profile)["profile"] = "clustered_mid"
    with pytest.raises(ValueError, match="profile"):
        campaign_from_dict(wrong_profile)

    literal_epoch = deepcopy(encoded)
    _dict_field(literal_epoch, "provenance")["epoch"] = "0"
    with pytest.raises(ValueError, match="epoch"):
        campaign_from_dict(literal_epoch)

    noncanonical_seed = deepcopy(encoded)
    _list_field(_dict_field(noncanonical_seed, "provenance"), "seeds")[0] = "01"
    with pytest.raises(ValueError, match="canonical"):
        campaign_from_dict(noncanonical_seed)

    non_ascii_seed = deepcopy(encoded)
    _list_field(_dict_field(non_ascii_seed, "provenance"), "seeds")[0] = (
        "\N{ARABIC-INDIC DIGIT ONE}"
    )
    with pytest.raises(ValueError, match="canonical"):
        campaign_from_dict(non_ascii_seed)


def test_predefined_json_decoder_rejects_duplicate_keys_and_nonstandard_constants() -> None:
    encoded = campaign_json(pr_campaign())
    duplicate_key = encoded.replace(
        '"policy_version":1,',
        '"policy_version":1,"policy_version":1,',
        1,
    )
    nonstandard_constant = encoded.replace('"policy_version":1', '"policy_version":NaN', 1)

    with pytest.raises(ValueError, match="duplicate key"):
        campaign_from_json(duplicate_key)
    with pytest.raises(ValueError, match="non-standard constant"):
        campaign_from_json(nonstandard_constant)

    noncanonical = json.dumps(campaign_to_dict(pr_campaign()), indent=2)
    with pytest.raises(ValueError, match="not canonical"):
        campaign_from_json(noncanonical)


def _dict_field(value: dict[str, object], key: str) -> dict[str, object]:
    return cast(dict[str, object], value[key])


def _list_field(value: dict[str, object], key: str) -> list[object]:
    return cast(list[object], value[key])


def _first_spec(value: dict[str, object]) -> dict[str, object]:
    suite = _dict_field(value, "suite")
    first_case = cast(dict[str, object], _list_field(suite, "cases")[0])
    return _dict_field(first_case, "spec")
