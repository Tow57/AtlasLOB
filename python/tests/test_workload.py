from __future__ import annotations

import hashlib
import json
from dataclasses import replace
from pathlib import Path

import pytest
from atlaslob.generation import WorkloadProfile, resolve_workload_spec
from atlaslob.workload import (
    CampaignCase,
    CampaignSuite,
    canonical_json,
    generate_workload,
    manifest_from_dict,
    manifest_to_dict,
    mapping_digest,
    open_workload,
    read_manifest,
    read_suite,
    suite_from_dict,
    suite_to_dict,
    verify_workload,
    write_manifest,
    write_suite,
)


def test_generated_workload_and_manifest_are_canonical_and_repeatable(
    tmp_path: Path,
) -> None:
    spec = resolve_workload_spec(
        WorkloadProfile.INVALID_MIX,
        command_count=500,
        snapshot_interval=25,
    )
    first_path = tmp_path / "first.atlas"
    second_path = tmp_path / "second.atlas"

    first = generate_workload(spec, 123, first_path)
    second = generate_workload(spec, 123, second_path)

    assert first == second
    assert first_path.read_bytes() == second_path.read_bytes()
    assert b"\r" not in first_path.read_bytes()
    assert first_path.read_bytes().endswith(b"\n")
    reader = verify_workload(first_path, first)
    assert reader.config == first.native_input
    assert len(tuple(reader.commands())) == 500


def test_manifest_round_trip_and_digest_cover_all_resolved_fields(
    tmp_path: Path,
) -> None:
    spec = resolve_workload_spec(
        WorkloadProfile.HOT_LEVEL_CONTENTION,
        command_count=100,
    )
    workload_path = tmp_path / "workload.atlas"
    manifest_path = tmp_path / "manifest.json"
    manifest = generate_workload(spec, 456, workload_path)

    encoded = manifest_to_dict(manifest)
    assert manifest_from_dict(encoded) == manifest
    write_manifest(manifest_path, manifest)
    assert read_manifest(manifest_path) == manifest
    assert manifest_path.read_text(encoding="ascii") == canonical_json(encoded)
    assert mapping_digest(encoded) == mapping_digest(
        json.loads(manifest_path.read_text(encoding="ascii"))
    )


def test_manifest_reader_rejects_noncanonical_duplicate_and_unicode_forms(
    tmp_path: Path,
) -> None:
    spec = resolve_workload_spec(
        WorkloadProfile.UNIFORM_SYNTHETIC,
        command_count=2,
    )
    manifest = generate_workload(spec, 456, tmp_path / "workload.atlas")
    encoded = manifest_to_dict(manifest)
    manifest_path = tmp_path / "manifest.json"

    manifest_path.write_text(json.dumps(encoded, indent=2), encoding="ascii")
    with pytest.raises(ValueError, match="not canonical"):
        read_manifest(manifest_path)

    canonical = canonical_json(encoded)
    manifest_path.write_text(
        canonical.replace("{", '{"schema":"duplicate",', 1),
        encoding="ascii",
    )
    with pytest.raises(ValueError, match="duplicate"):
        read_manifest(manifest_path)

    unicode_seed = dict(encoded)
    unicode_seed["seed"] = "\u0661"
    manifest_path.write_text(canonical_json(unicode_seed), encoding="ascii")
    with pytest.raises(ValueError, match="canonical unsigned decimal"):
        read_manifest(manifest_path)


def test_workload_verification_detects_tampering_and_noncanonical_lines(
    tmp_path: Path,
) -> None:
    spec = resolve_workload_spec(
        WorkloadProfile.UNIFORM_SYNTHETIC,
        command_count=10,
    )
    workload_path = tmp_path / "workload.atlas"
    manifest = generate_workload(spec, 789, workload_path)

    tampered = bytearray(workload_path.read_bytes())
    tampered[-2] = ord("9") if tampered[-2] != ord("9") else ord("8")
    workload_path.write_bytes(tampered)
    with pytest.raises(ValueError, match="digest"):
        verify_workload(workload_path, manifest)

    crlf_path = tmp_path / "crlf.atlas"
    crlf_path.write_bytes(workload_path.read_bytes().replace(b"\n", b"\r\n"))
    with pytest.raises(ValueError, match="CRLF"):
        tuple(open_workload(crlf_path).commands())


@pytest.mark.parametrize("line_index", [0, 1], ids=["header", "command"])
def test_workload_verification_rejects_digest_valid_noncanonical_whitespace(
    tmp_path: Path,
    line_index: int,
) -> None:
    spec = resolve_workload_spec(
        WorkloadProfile.UNIFORM_SYNTHETIC,
        command_count=2,
    )
    workload_path = tmp_path / "workload.atlas"
    manifest = generate_workload(spec, 790, workload_path)
    lines = workload_path.read_bytes().splitlines(keepends=True)
    lines[line_index] = lines[line_index].replace(b" ", b"  ", 1)
    noncanonical = b"".join(lines)
    workload_path.write_bytes(noncanonical)
    matching_manifest = replace(
        manifest,
        command_stream_sha256=hashlib.sha256(noncanonical).hexdigest(),
    )

    with pytest.raises(ValueError, match="canonical"):
        verify_workload(workload_path, matching_manifest)


def test_campaign_suite_round_trip_is_fully_resolved(tmp_path: Path) -> None:
    cases = tuple(
        CampaignCase(
            name=f"seed-{index}",
            seed=index,
            spec=resolve_workload_spec(
                profile,
                command_count=5_000,
                snapshot_interval=100,
            ),
        )
        for index, profile in enumerate(list(WorkloadProfile), start=1)
    )
    suite = CampaignSuite("phase3-pr-smoke-v1", "pr", "exact", cases)
    path = tmp_path / "suite.json"

    encoded = suite_to_dict(suite)
    assert suite_from_dict(encoded) == suite
    write_suite(path, suite)
    assert read_suite(path) == suite
    encoded_cases = encoded["cases"]
    assert isinstance(encoded_cases, list)
    assert len(encoded_cases) == 10


def test_campaign_suite_rejects_duplicate_names_and_stale_fields() -> None:
    spec = resolve_workload_spec(
        WorkloadProfile.UNIFORM_SYNTHETIC,
        command_count=10,
    )
    case = CampaignCase("same", 1, spec)
    with pytest.raises(ValueError, match="unique"):
        CampaignSuite("duplicate", "pr", "exact", (case, case))

    encoded = suite_to_dict(CampaignSuite("valid", "pr", "exact", (case,)))
    encoded["unexpected"] = True
    with pytest.raises(ValueError, match="fields"):
        suite_from_dict(encoded)
