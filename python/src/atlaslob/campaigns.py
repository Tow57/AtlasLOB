"""Versioned predefined campaign policy for Phase 3 differential evidence.

Factories in this module return fully resolved workload specifications.  The
main and nightly tiers require an explicit caller-supplied epoch; no wall clock,
process ID, platform entropy, or hidden default participates in seed selection.
"""

from __future__ import annotations

import hashlib
import json
from collections import Counter
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Final, Literal, cast

from atlaslob.domain import U64_MAX, MatchingConfig
from atlaslob.generation import WorkloadProfile, WorkloadSpec, resolve_workload_spec
from atlaslob.workload import (
    CampaignCase,
    CampaignSuite,
    canonical_json,
    suite_from_dict,
    suite_to_dict,
)

CAMPAIGN_POLICY_SCHEMA: Final = "atlas_predefined_campaign_v1"
CAMPAIGN_POLICY_VERSION: Final = 1
ROTATING_SEED_EPOCH_POLICY: Final = "sha256_domain_separated_u64_epoch_v1"
LITERAL_SEED_POLICY: Final = "published_literal_u64_v1"

PR_COMMANDS_PER_CASE: Final = 5_000
PR_CASE_COUNT: Final = 10
PR_SNAPSHOT_INTERVAL: Final = 100

MAIN_COMMANDS_PER_CASE: Final = 100_000
MAIN_CASE_COUNT: Final = 20
MAIN_SNAPSHOT_INTERVAL: Final = 1_000

NIGHTLY_COMMANDS_PER_CASE: Final = 1_000_000
NIGHTLY_CASE_COUNT: Final = 10
NIGHTLY_SNAPSHOT_INTERVAL: Final = 10_000

RELEASE_COMMANDS_PER_CASE: Final = 10_000_000
RELEASE_CASE_COUNT: Final = 10
RELEASE_SNAPSHOT_INTERVAL: Final = 50_000

RELEASE_SANITIZER_COMMANDS_PER_CASE: Final = 100_000
RELEASE_SANITIZER_CASE_COUNT: Final = 10
RELEASE_SANITIZER_SNAPSHOT_INTERVAL: Final = 1_000

REQUIRED_PROFILES: Final = (
    WorkloadProfile.UNIFORM_SYNTHETIC,
    WorkloadProfile.CLUSTERED_MID,
    WorkloadProfile.HOT_LEVEL_CONTENTION,
    WorkloadProfile.SPARSE_WIDE,
    WorkloadProfile.CANCEL_HEAVY,
    WorkloadProfile.SWEEP_HEAVY,
    WorkloadProfile.REPLACE_HEAVY,
    WorkloadProfile.INVALID_MIX,
    WorkloadProfile.TRACE_DRIVEN_SYNTHETIC,
    WorkloadProfile.ADVERSARIAL_BOUNDARY,
)

_CAMPAIGN_ENGINE: Final = MatchingConfig(
    max_order_quantity=1_000,
    tick_increment=1,
    max_active_orders=128,
)
_PR_SEEDS: Final = (
    0x243F6A8885A308D3,
    0x13198A2E03707344,
    0xA4093822299F31D0,
    0x082EFA98EC4E6C89,
    0x452821E638D01377,
    0xBE5466CF34E90C6C,
    0xC0AC29B7C97C50DD,
    0x3F84D5B5B5470917,
    0x9216D5D98979FB1B,
    0xD1310BA698DFB5AC,
)
_RELEASE_SEEDS: Final = (
    0x6A09E667F3BCC908,
    0xBB67AE8584CAA73B,
    0x3C6EF372FE94F82B,
    0xA54FF53A5F1D36F1,
    0x510E527FADE682D1,
    0x9B05688C2B3E6C1F,
    0x1F83D9ABFB41BD6B,
    0x5BE0CD19137E2179,
    0xCBBB9D5DC1059ED8,
    0x629A292A367CD507,
)

SeedKind = Literal["fixed", "rotating_epoch", "published"]


@dataclass(frozen=True, slots=True)
class SeedProvenance:
    """Complete, replayable origin for every seed in one campaign."""

    kind: SeedKind
    seed_set_id: str
    algorithm: str
    epoch: int | None
    seeds: tuple[int, ...]

    def __post_init__(self) -> None:
        if self.kind not in {"fixed", "rotating_epoch", "published"}:
            raise ValueError("seed provenance kind is outside the V1 vocabulary")
        if not self.seed_set_id or any(character.isspace() for character in self.seed_set_id):
            raise ValueError("seed set ID must be nonempty and contain no whitespace")
        if self.kind == "rotating_epoch":
            if self.algorithm != ROTATING_SEED_EPOCH_POLICY:
                raise ValueError("rotating seed provenance uses an unknown algorithm")
            _require_u64("epoch", self.epoch)
        else:
            if self.algorithm != LITERAL_SEED_POLICY:
                raise ValueError("literal seed provenance uses an unknown algorithm")
            if self.epoch is not None:
                raise ValueError("literal seed provenance cannot carry an epoch")
        if not self.seeds:
            raise ValueError("seed provenance must contain at least one seed")
        for seed in self.seeds:
            _require_u64("seed", seed)
        if len(self.seeds) != len(set(self.seeds)):
            raise ValueError("seed provenance contains duplicate seeds")


@dataclass(frozen=True, slots=True)
class PredefinedCampaign:
    """One strictly validated, fully resolved Phase 3 campaign suite."""

    policy_version: int
    provenance: SeedProvenance
    suite: CampaignSuite

    def __post_init__(self) -> None:
        if self.policy_version != CAMPAIGN_POLICY_VERSION:
            raise ValueError("unsupported predefined campaign policy version")
        _validate_predefined_campaign(self)


@dataclass(frozen=True, slots=True)
class _TierPolicy:
    case_count: int
    commands_per_case: int
    snapshot_interval: int
    mode: Literal["exact", "compact"]
    profile_repetitions: int
    seed_kind: SeedKind
    seed_set_id: str


_TIER_POLICIES: Final = {
    "pr": _TierPolicy(
        PR_CASE_COUNT,
        PR_COMMANDS_PER_CASE,
        PR_SNAPSHOT_INTERVAL,
        "exact",
        1,
        "fixed",
        "phase3-pr-fixed-v1",
    ),
    "main": _TierPolicy(
        MAIN_CASE_COUNT,
        MAIN_COMMANDS_PER_CASE,
        MAIN_SNAPSHOT_INTERVAL,
        "exact",
        2,
        "rotating_epoch",
        "phase3-main-rotating-v1",
    ),
    "nightly": _TierPolicy(
        NIGHTLY_CASE_COUNT,
        NIGHTLY_COMMANDS_PER_CASE,
        NIGHTLY_SNAPSHOT_INTERVAL,
        "compact",
        1,
        "rotating_epoch",
        "phase3-nightly-rotating-v1",
    ),
    "release": _TierPolicy(
        RELEASE_CASE_COUNT,
        RELEASE_COMMANDS_PER_CASE,
        RELEASE_SNAPSHOT_INTERVAL,
        "compact",
        1,
        "published",
        "phase3-release-published-v1",
    ),
    "release_sanitizer": _TierPolicy(
        RELEASE_SANITIZER_CASE_COUNT,
        RELEASE_SANITIZER_COMMANDS_PER_CASE,
        RELEASE_SANITIZER_SNAPSHOT_INTERVAL,
        "exact",
        1,
        "published",
        "phase3-release-published-v1",
    ),
}


def pr_campaign() -> PredefinedCampaign:
    """Return the fixed 10-by-5,000 exact pull-request campaign."""

    return _build_campaign("pr", epoch=None)


def main_campaign(epoch: int) -> PredefinedCampaign:
    """Resolve the 20-by-100,000 main campaign for an explicit rotation epoch."""

    _require_u64("epoch", epoch)
    return _build_campaign("main", epoch=epoch)


def nightly_campaign(epoch: int) -> PredefinedCampaign:
    """Resolve the 10-by-1,000,000 compact nightly campaign for an epoch."""

    _require_u64("epoch", epoch)
    return _build_campaign("nightly", epoch=epoch)


def release_campaign() -> PredefinedCampaign:
    """Return the published 10,000,000-command-per-seed release soak."""

    return _build_campaign("release", epoch=None)


def release_sanitizer_campaign() -> PredefinedCampaign:
    """Return the exact sanitizer subset of the published release seed set."""

    return _build_campaign("release_sanitizer", epoch=None)


def checked_campaigns() -> tuple[tuple[str, PredefinedCampaign], ...]:
    """Return the small checked JSON corpus, including epoch-zero rotation examples."""

    return (
        ("pr.json", pr_campaign()),
        ("main-epoch-0.json", main_campaign(0)),
        ("nightly-epoch-0.json", nightly_campaign(0)),
        ("release.json", release_campaign()),
        ("release-sanitizer.json", release_sanitizer_campaign()),
    )


def campaign_to_dict(campaign: PredefinedCampaign) -> dict[str, object]:
    """Serialize a campaign with no implicit seed or workload parameters."""

    provenance = campaign.provenance
    return {
        "schema": CAMPAIGN_POLICY_SCHEMA,
        "policy_version": campaign.policy_version,
        "provenance": {
            "kind": provenance.kind,
            "seed_set_id": provenance.seed_set_id,
            "algorithm": provenance.algorithm,
            "epoch": None if provenance.epoch is None else str(provenance.epoch),
            "seeds": [str(seed) for seed in provenance.seeds],
        },
        "suite": suite_to_dict(campaign.suite),
    }


def campaign_from_dict(value: Mapping[str, object]) -> PredefinedCampaign:
    """Strictly decode and policy-check a predefined campaign."""

    _require_keys(
        value,
        {"schema", "policy_version", "provenance", "suite"},
        "predefined campaign",
    )
    if value["schema"] != CAMPAIGN_POLICY_SCHEMA:
        raise ValueError("unsupported predefined campaign schema")
    provenance_value = _mapping(value["provenance"], "provenance")
    _require_keys(
        provenance_value,
        {"kind", "seed_set_id", "algorithm", "epoch", "seeds"},
        "seed provenance",
    )
    kind_value = _string(provenance_value["kind"], "seed provenance kind")
    if kind_value not in {"fixed", "rotating_epoch", "published"}:
        raise ValueError("seed provenance kind is outside the V1 vocabulary")
    raw_epoch = provenance_value["epoch"]
    epoch = None if raw_epoch is None else _decimal(raw_epoch, "epoch")
    raw_seeds = provenance_value["seeds"]
    if not isinstance(raw_seeds, list):
        raise ValueError("seed provenance seeds must be an array")
    provenance = SeedProvenance(
        kind=cast(SeedKind, kind_value),
        seed_set_id=_string(provenance_value["seed_set_id"], "seed_set_id"),
        algorithm=_string(provenance_value["algorithm"], "seed algorithm"),
        epoch=epoch,
        seeds=tuple(_decimal(seed, f"seeds[{index}]") for index, seed in enumerate(raw_seeds)),
    )
    return PredefinedCampaign(
        policy_version=_json_int(value["policy_version"], "policy_version"),
        provenance=provenance,
        suite=suite_from_dict(_mapping(value["suite"], "suite")),
    )


def campaign_json(campaign: PredefinedCampaign) -> str:
    """Return canonical, LF-terminated ASCII JSON."""

    return canonical_json(campaign_to_dict(campaign))


def campaign_from_json(text: str) -> PredefinedCampaign:
    """Decode one JSON campaign and enforce the complete predefined policy."""

    try:
        value = json.loads(
            text,
            object_pairs_hook=_unique_object,
            parse_constant=_reject_json_constant,
        )
    except (json.JSONDecodeError, UnicodeError, RecursionError) as exc:
        raise ValueError("predefined campaign is not valid JSON") from exc
    campaign = campaign_from_dict(_mapping(value, "predefined campaign document"))
    if text != campaign_json(campaign):
        raise ValueError("predefined campaign JSON is not canonical")
    return campaign


def _build_campaign(
    tier: Literal["pr", "main", "nightly", "release", "release_sanitizer"],
    *,
    epoch: int | None,
) -> PredefinedCampaign:
    policy = _TIER_POLICIES[tier]
    profiles = REQUIRED_PROFILES * policy.profile_repetitions
    seeds = _resolved_seeds(tier, epoch, policy.case_count)
    cases = tuple(
        CampaignCase(
            name=f"{index:02d}-{profile.value}",
            seed=seed,
            spec=_resolved_spec(
                profile,
                command_count=policy.commands_per_case,
                snapshot_interval=policy.snapshot_interval,
            ),
        )
        for index, (profile, seed) in enumerate(zip(profiles, seeds, strict=True))
    )
    provenance = SeedProvenance(
        kind=policy.seed_kind,
        seed_set_id=policy.seed_set_id,
        algorithm=(
            ROTATING_SEED_EPOCH_POLICY
            if policy.seed_kind == "rotating_epoch"
            else LITERAL_SEED_POLICY
        ),
        epoch=epoch,
        seeds=seeds,
    )
    return PredefinedCampaign(
        policy_version=CAMPAIGN_POLICY_VERSION,
        provenance=provenance,
        suite=CampaignSuite(
            name=_suite_name(tier, epoch),
            tier=tier,
            mode=policy.mode,
            cases=cases,
        ),
    )


def _resolved_spec(
    profile: WorkloadProfile,
    *,
    command_count: int,
    snapshot_interval: int,
) -> WorkloadSpec:
    return resolve_workload_spec(
        profile,
        command_count=command_count,
        instrument_id=7,
        engine=_CAMPAIGN_ENGINE,
        invalid_basis_points=None,
        mid_price=10_000,
        price_span_ticks=128,
        active_order_target=64,
        client_count=16,
        snapshot_interval=snapshot_interval,
    )


def _resolved_seeds(
    tier: str,
    epoch: int | None,
    count: int,
) -> tuple[int, ...]:
    if tier == "pr":
        return _PR_SEEDS
    if tier in {"release", "release_sanitizer"}:
        return _RELEASE_SEEDS
    if epoch is None:
        raise ValueError("rotating campaign requires an explicit epoch")
    return _rotating_seeds(tier, epoch, count)


def _rotating_seeds(tier: str, epoch: int, count: int) -> tuple[int, ...]:
    seeds = tuple(
        int.from_bytes(
            hashlib.sha256(f"atlaslob:phase3:v1:{tier}:{epoch}:{index}".encode("ascii")).digest()[
                :8
            ],
            "big",
        )
        for index in range(count)
    )
    if len(seeds) != len(set(seeds)):
        raise RuntimeError("rotating seed derivation produced a collision")
    return seeds


def _suite_name(tier: str, epoch: int | None) -> str:
    normalized_tier = tier.replace("_", "-")
    if epoch is None:
        return f"phase3-{normalized_tier}-v1"
    return f"phase3-{normalized_tier}-epoch-{epoch}-v1"


def _validate_predefined_campaign(campaign: PredefinedCampaign) -> None:
    suite = campaign.suite
    policy = _TIER_POLICIES[suite.tier]
    provenance = campaign.provenance
    if suite.mode != policy.mode:
        raise ValueError("campaign mode differs from its predefined tier")
    if len(suite.cases) != policy.case_count:
        raise ValueError("campaign case count differs from its predefined tier")
    if suite.name != _suite_name(suite.tier, provenance.epoch):
        raise ValueError("campaign name differs from its predefined tier")
    if provenance.kind != policy.seed_kind or provenance.seed_set_id != policy.seed_set_id:
        raise ValueError("campaign seed provenance differs from its predefined tier")
    expected_algorithm = (
        ROTATING_SEED_EPOCH_POLICY if policy.seed_kind == "rotating_epoch" else LITERAL_SEED_POLICY
    )
    if provenance.algorithm != expected_algorithm:
        raise ValueError("campaign seed algorithm differs from its predefined tier")

    expected_profiles = REQUIRED_PROFILES * policy.profile_repetitions
    actual_profiles = tuple(case.spec.profile for case in suite.cases)
    if actual_profiles != expected_profiles:
        raise ValueError("campaign profile mapping differs from its predefined tier")
    profile_counts = Counter(actual_profiles)
    if any(profile_counts[profile] != policy.profile_repetitions for profile in REQUIRED_PROFILES):
        raise ValueError("campaign does not cover every required profile equally")

    expected_seeds = _resolved_seeds(suite.tier, provenance.epoch, policy.case_count)
    case_seeds = tuple(case.seed for case in suite.cases)
    if provenance.seeds != expected_seeds or case_seeds != expected_seeds:
        raise ValueError("campaign seeds differ from their declared provenance")

    for index, (case, profile) in enumerate(zip(suite.cases, expected_profiles, strict=True)):
        if case.name != f"{index:02d}-{profile.value}":
            raise ValueError("campaign case name does not match its deterministic mapping")
        expected_spec = _resolved_spec(
            profile,
            command_count=policy.commands_per_case,
            snapshot_interval=policy.snapshot_interval,
        )
        if case.spec != expected_spec:
            raise ValueError("campaign case spec differs from its predefined distribution")


def _require_keys(value: Mapping[str, object], expected: set[str], name: str) -> None:
    if set(value) != expected:
        raise ValueError(f"{name} fields do not match the V1 schema")


def _mapping(value: object, name: str) -> Mapping[str, object]:
    if not isinstance(value, dict) or any(not isinstance(key, str) for key in value):
        raise ValueError(f"{name} must be an object with string keys")
    return value


def _string(value: object, name: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{name} must be a string")
    return value


def _decimal(value: object, name: str) -> int:
    text = _string(value, name)
    if not text or not text.isascii() or not text.isdecimal() or (text != "0" and text[0] == "0"):
        raise ValueError(f"{name} must be canonical unsigned decimal")
    parsed = int(text)
    _require_u64(name, parsed)
    return parsed


def _json_int(value: object, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{name} must be a JSON integer")
    return value


def _require_u64(name: str, value: object) -> None:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= U64_MAX:
        raise ValueError(f"{name} must be a u64")


def _unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"predefined campaign JSON contains duplicate key {key!r}")
        value[key] = item
    return value


def _reject_json_constant(value: str) -> object:
    raise ValueError(f"predefined campaign JSON contains non-standard constant {value!r}")
