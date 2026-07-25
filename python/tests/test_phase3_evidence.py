from __future__ import annotations

import json
import re
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Final, cast

import pytest
from atlaslob.campaigns import nightly_campaign, pr_campaign
from atlaslob.workload import WorkloadManifest, generate_workload

_REPOSITORY_ROOT = Path(__file__).parents[2]
_EVIDENCE_ROOT = _REPOSITORY_ROOT / "docs" / "evidence" / "phase3"
_SHA256 = re.compile(r"[0-9a-f]{64}", re.ASCII)
_NON_EVIDENCE_KEY_FRAGMENTS = ("duration", "elapsed", "latency", "throughput")
_PR_TOP_LEVEL_KEYS: Final = (
    "schema",
    "policy_document",
    "mode",
    "cases_expected",
    "cases_completed",
    "passed",
    "total_commands",
    "cases",
)
_PR_CASE_KEYS: Final = (
    "case_name",
    "status",
    "commands_compared",
    "seed",
    "command_stream_sha256",
    "reference_evidence_sha256",
    "native_evidence_sha256",
    "intended_valid",
    "intended_invalid",
    "new_orders",
    "cancels",
    "replaces",
    "intent_family_count",
)
_NIGHTLY_TOP_LEVEL_KEYS: Final = (
    "schema",
    "policy_document",
    "suite",
    "seed_set_id",
    "epoch",
    "selected_case_index",
    "mode",
    "artifact_policy",
    "passed",
    "case",
)
_NIGHTLY_CASE_KEYS: Final = (
    "case_name",
    "profile",
    "status",
    "commands_compared",
    "snapshot_interval",
    "generator_version",
    "seed",
    "command_stream_sha256",
    "reference_evidence_sha256",
    "native_evidence_sha256",
    "reference_committed",
    "reference_rejected",
    "reference_engine_error",
    "native_committed",
    "native_rejected",
    "native_engine_error",
    "intended_valid",
    "intended_invalid",
    "new_orders",
    "cancels",
    "replaces",
    "intent_family_count",
)
_PR_DIGESTS: Final = {
    "00-uniform_synthetic": (
        "f41b399f1ed6aa42cd074be6d370b71d983135b9138f2e2a011254b617662d30",
        "673dd8c6552c9fda9396c765b820401d4fa52bbf18dc361769dee24779b96b0b",
    ),
    "01-clustered_mid": (
        "f5cf7fe194acbb92b30baa373aa23780466a39124fbf52f3e4bde41ce1dac965",
        "982325be19fe40bf0dd29e3010662fd52edf8293d53c650b1ca67bfc1978ad57",
    ),
    "02-hot_level_contention": (
        "a122a8ea14f9543d915609c1c218495659bf27eba8c9f5126125e5ac3d7121b4",
        "c331463ce621d9ae7c83a344e54e6cbeb44e0efb3c426a917ebf3d0c522e366b",
    ),
    "03-sparse_wide": (
        "ce2085bd259133776421207a5e6cd5cb5dd2f9230686354aba09f5db7dac7d05",
        "ce3191e747a8bba96ac4ceea4e84b9ec9492e60ae08017cb534ec7e3357f31a7",
    ),
    "04-cancel_heavy": (
        "6eadbd2bbc3582368686f55fd6f3cf0a5023ab3a0d24b41a8f43af735ea1d52a",
        "61cd59fbcdcc7d5c1be42ac39dd26a9426195e9451b2936a40f2c1e87836f35d",
    ),
    "05-sweep_heavy": (
        "b8ca5933b86c57fc011b8c316a06f4e2410e555fcd1e574d89cd292a680baffd",
        "60179ed88a51bb3c3afda7b430da17112e426323021ae6e391c5022022853c3a",
    ),
    "06-replace_heavy": (
        "c761b0906105bd0a1e3d5cbd8150744b90bbf0b297f0c58e0654fa0fdd5b2f41",
        "3dcd694181500ff8fe2996a6e2900914b2cc611504a14ab929010830332e9385",
    ),
    "07-invalid_mix": (
        "fe8ddc77e76428c9176756861f30be20ea56a341f48b2c9fb8f7668319b61ee0",
        "c76bf387cc8a67cb934310bb890dc171a6b4d612d258b1995807841aeb43b7a1",
    ),
    "08-trace_driven_synthetic": (
        "2aab45f449fea2dba0e064d8576eefca4825c4c32f400d7347aedf80efea8278",
        "baa32bf3e1cc7c0d9c5f67b17f8249ebcd1466f5acc318ed8075854004a19d06",
    ),
    "09-adversarial_boundary": (
        "0d6248e5ca1c8b21571ad44264aedfd51e241f559a8b4560a9c794c32f164a9e",
        "82a955d7f0ba2d1d6135eba48606e4070986d374b39a9b4bca01d0ec58076096",
    ),
}
_NIGHTLY_COMMAND_SHA256: Final = "26720229a3fbe25438ccc7a6616843c0e4dffbe572da0b2f91584299f83a2a93"
_NIGHTLY_EVIDENCE_SHA256: Final = "686352d33ed77e429ae0e4298ccc2b23422be282acbb5249dd508722252605c5"


def _load_evidence(name: str) -> dict[str, object]:
    text = (_EVIDENCE_ROOT / name).read_text(encoding="ascii")
    parsed = cast(dict[str, object], json.loads(text))
    assert text == json.dumps(parsed, ensure_ascii=True, indent=2) + "\n"
    return parsed


def _mapping(value: object) -> dict[str, object]:
    assert isinstance(value, dict)
    return cast(dict[str, object], value)


def _mappings(value: object) -> list[dict[str, object]]:
    assert isinstance(value, list)
    return [_mapping(item) for item in value]


def _text(mapping: dict[str, object], key: str) -> str:
    value = mapping[key]
    assert isinstance(value, str)
    return value


def _count(mapping: dict[str, object], key: str) -> int:
    value = _text(mapping, key)
    assert value.isdecimal()
    return int(value)


def _assert_sha256(mapping: dict[str, object], key: str) -> str:
    value = _text(mapping, key)
    assert _SHA256.fullmatch(value) is not None
    return value


def _assert_portable_correctness_evidence(value: object) -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            assert isinstance(key, str)
            normalized = key.lower()
            assert all(fragment not in normalized for fragment in _NON_EVIDENCE_KEY_FRAGMENTS)
            _assert_portable_correctness_evidence(child)
    elif isinstance(value, list):
        for child in value:
            _assert_portable_correctness_evidence(child)
    elif isinstance(value, str):
        assert not PurePosixPath(value).is_absolute()
        assert not PureWindowsPath(value).is_absolute()


def _assert_generated_manifest(
    manifest: WorkloadManifest,
    evidence: dict[str, object],
) -> None:
    assert manifest.seed == _count(evidence, "seed")
    assert manifest.command_count == _count(evidence, "commands_compared")
    assert manifest.command_stream_sha256 == _text(evidence, "command_stream_sha256")
    assert manifest.stats.intended_valid == _count(evidence, "intended_valid")
    assert manifest.stats.intended_invalid == _count(evidence, "intended_invalid")
    assert manifest.stats.new_orders == _count(evidence, "new_orders")
    assert manifest.stats.cancels == _count(evidence, "cancels")
    assert manifest.stats.replaces == _count(evidence, "replaces")
    assert len(manifest.stats.intents) == _count(evidence, "intent_family_count")


def test_checked_pr_evidence_matches_the_fixed_campaign_policy() -> None:
    evidence = _load_evidence("pr-smoke.json")
    campaign = pr_campaign()
    cases = _mappings(evidence["cases"])

    assert tuple(evidence) == _PR_TOP_LEVEL_KEYS
    assert evidence["schema"] == "atlas_phase3_pr_evidence_v1"
    assert evidence["policy_document"] == "python/campaigns/v1/pr.json"
    assert (_REPOSITORY_ROOT / _text(evidence, "policy_document")).is_file()
    assert evidence["mode"] == campaign.suite.mode == "exact"
    assert evidence["passed"] is True
    assert _count(evidence, "cases_expected") == len(campaign.suite.cases)
    assert _count(evidence, "cases_completed") == len(campaign.suite.cases)
    assert len(cases) == len(campaign.suite.cases)

    total_commands = 0
    for actual, expected in zip(cases, campaign.suite.cases, strict=True):
        commands = _count(actual, "commands_compared")
        expected_command_digest, expected_evidence_digest = _PR_DIGESTS[expected.name]

        assert tuple(actual) == _PR_CASE_KEYS
        assert actual["status"] == "passed"
        assert actual["case_name"] == expected.name
        assert _count(actual, "seed") == expected.seed
        assert commands == expected.spec.command_count
        assert _count(actual, "intended_valid") + _count(actual, "intended_invalid") == commands
        assert (
            _count(actual, "new_orders") + _count(actual, "cancels") + _count(actual, "replaces")
            == commands
        )
        assert _count(actual, "intent_family_count") == 35
        assert _assert_sha256(actual, "command_stream_sha256") == expected_command_digest
        assert (
            _assert_sha256(actual, "reference_evidence_sha256")
            == _assert_sha256(
                actual,
                "native_evidence_sha256",
            )
            == expected_evidence_digest
        )
        total_commands += commands

    assert _count(evidence, "total_commands") == total_commands == 50_000
    _assert_portable_correctness_evidence(evidence)


def test_checked_million_command_evidence_matches_the_nightly_policy() -> None:
    evidence = _load_evidence("nightly-one-million.json")
    campaign = nightly_campaign(0)
    expected = campaign.suite.cases[0]
    actual = _mapping(evidence["case"])
    commands = _count(actual, "commands_compared")

    assert tuple(evidence) == _NIGHTLY_TOP_LEVEL_KEYS
    assert evidence["schema"] == "atlas_phase3_nightly_evidence_v1"
    assert evidence["policy_document"] == "python/campaigns/v1/nightly-epoch-0.json"
    assert (_REPOSITORY_ROOT / _text(evidence, "policy_document")).is_file()
    assert evidence["suite"] == campaign.suite.name
    assert evidence["seed_set_id"] == campaign.provenance.seed_set_id
    assert _count(evidence, "epoch") == campaign.provenance.epoch == 0
    assert _count(evidence, "selected_case_index") == 0
    assert evidence["mode"] == campaign.suite.mode == "compact"
    assert evidence["artifact_policy"] == "summary_only"
    assert evidence["passed"] is True

    assert tuple(actual) == _NIGHTLY_CASE_KEYS
    assert actual["case_name"] == expected.name
    assert actual["profile"] == expected.spec.profile.value
    assert actual["status"] == "passed"
    assert actual["generator_version"] == 1
    assert commands == expected.spec.command_count == 1_000_000
    assert _count(actual, "snapshot_interval") == expected.spec.snapshot_interval
    assert _count(actual, "seed") == expected.seed
    assert _count(actual, "reference_engine_error") == 0
    assert _count(actual, "native_engine_error") == 0
    assert _count(actual, "reference_committed") == _count(actual, "native_committed")
    assert _count(actual, "reference_rejected") == _count(actual, "native_rejected")
    assert _count(actual, "reference_committed") + _count(actual, "reference_rejected") == commands
    assert _count(actual, "intended_valid") + _count(actual, "intended_invalid") == commands
    assert _count(actual, "intended_valid") == _count(actual, "reference_committed")
    assert _count(actual, "intended_invalid") == _count(actual, "reference_rejected")
    assert (
        _count(actual, "new_orders") + _count(actual, "cancels") + _count(actual, "replaces")
        == commands
    )
    assert _count(actual, "intent_family_count") == 35
    assert _assert_sha256(actual, "command_stream_sha256") == _NIGHTLY_COMMAND_SHA256
    assert (
        _assert_sha256(actual, "reference_evidence_sha256")
        == _assert_sha256(
            actual,
            "native_evidence_sha256",
        )
        == _NIGHTLY_EVIDENCE_SHA256
    )
    _assert_portable_correctness_evidence(evidence)


@pytest.mark.campaign
def test_checked_evidence_regenerates_from_the_frozen_v1_workloads(tmp_path: Path) -> None:
    pr_evidence = _load_evidence("pr-smoke.json")
    pr_cases = {_text(case, "case_name"): case for case in _mappings(pr_evidence["cases"])}
    for case in pr_campaign().suite.cases:
        manifest = generate_workload(
            case.spec,
            case.seed,
            tmp_path / "pr" / case.name / "workload.atlas",
        )
        _assert_generated_manifest(manifest, pr_cases[case.name])

    nightly_evidence = _mapping(_load_evidence("nightly-one-million.json")["case"])
    nightly_case = nightly_campaign(0).suite.cases[0]
    nightly_manifest = generate_workload(
        nightly_case.spec,
        nightly_case.seed,
        tmp_path / "nightly" / "workload.atlas",
    )
    _assert_generated_manifest(nightly_manifest, nightly_evidence)
