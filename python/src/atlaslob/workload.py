"""Persisted workload, manifest, and campaign-suite schemas."""

from __future__ import annotations

import hashlib
import json
import time
from collections.abc import Iterator, Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Final, Literal, NoReturn, cast

from atlaslob.domain import U64_MAX, Command
from atlaslob.generation import (
    GENERATOR_VERSION,
    GenerationStats,
    WorkloadSpec,
    iter_generated,
    spec_from_dict,
    spec_to_dict,
)
from atlaslob.native import (
    NativeInputConfig,
    OutputMode,
    decode_command,
    decode_header,
    encode_command,
    encode_header,
)

WORKLOAD_MANIFEST_SCHEMA: Final = "atlas_workload_manifest_v1"
CAMPAIGN_SUITE_SCHEMA: Final = "atlas_campaign_suite_v1"
_SHA256_HEX_LENGTH: Final = 64
CampaignTier = Literal["pr", "main", "nightly", "release", "release_sanitizer"]


@dataclass(frozen=True, slots=True)
class WorkloadManifest:
    """Complete provenance for one generated command stream."""

    generator_version: int
    seed: int
    spec: WorkloadSpec
    command_count: int
    command_stream_sha256: str
    stats: GenerationStats

    def __post_init__(self) -> None:
        if self.generator_version != GENERATOR_VERSION:
            raise ValueError("unsupported workload generator version")
        _require_u64("seed", self.seed)
        _require_u64("command_count", self.command_count)
        if self.command_count != self.spec.command_count:
            raise ValueError("manifest command count differs from its resolved spec")
        _require_digest(self.command_stream_sha256, "command_stream_sha256")
        if self.stats.command_count != self.command_count:
            raise ValueError("generation stats do not cover the manifest command count")
        if self.stats.intended_valid + self.stats.intended_invalid != self.command_count:
            raise ValueError("generation validity intents do not cover every command")
        if self.stats.new_orders + self.stats.cancels + self.stats.replaces != self.command_count:
            raise ValueError("generation operation counts do not cover every command")
        if sum(count for _, count in self.stats.intents) != self.command_count:
            raise ValueError("generation intent histogram does not cover every command")
        intent_names = [name for name, _ in self.stats.intents]
        if (
            intent_names != sorted(intent_names)
            or len(intent_names) != len(set(intent_names))
            or any(not name or count < 1 for name, count in self.stats.intents)
        ):
            raise ValueError("generation intent histogram is not canonical")

    @property
    def native_input(self) -> NativeInputConfig:
        return NativeInputConfig(
            instrument_id=self.spec.instrument_id,
            engine=self.spec.engine,
            snapshot_interval=self.spec.snapshot_interval,
        )


@dataclass(frozen=True, slots=True)
class CampaignCase:
    """One named seed/spec pair inside a campaign suite."""

    name: str
    seed: int
    spec: WorkloadSpec

    def __post_init__(self) -> None:
        if not self.name or any(character.isspace() for character in self.name):
            raise ValueError("campaign case name must be nonempty and contain no whitespace")
        _require_u64("seed", self.seed)


@dataclass(frozen=True, slots=True)
class CampaignSuite:
    """A fully resolved collection of deterministic campaigns."""

    name: str
    tier: CampaignTier
    mode: OutputMode
    cases: tuple[CampaignCase, ...]

    def __post_init__(self) -> None:
        if not self.name:
            raise ValueError("campaign suite name must be nonempty")
        if self.tier not in {"pr", "main", "nightly", "release", "release_sanitizer"}:
            raise ValueError("campaign suite tier is outside the V1 vocabulary")
        if self.mode not in ("exact", "compact"):
            raise ValueError("campaign suite mode is outside the native vocabulary")
        if not self.cases:
            raise ValueError("campaign suite must contain at least one case")
        names = [case.name for case in self.cases]
        if len(names) != len(set(names)):
            raise ValueError("campaign case names must be unique")


@dataclass(frozen=True, slots=True)
class WorkloadReader:
    """Verified header plus a repeatable command iterator."""

    path: Path
    config: NativeInputConfig

    def commands(self) -> Iterator[Command]:
        with self.path.open("r", encoding="utf-8", newline="") as input_file:
            header_seen = False
            for physical_line, raw_line in enumerate(input_file, start=1):
                if not raw_line.endswith("\n"):
                    raise ValueError(f"workload line {physical_line} is not LF terminated")
                line = raw_line[:-1]
                if line.endswith("\r"):
                    raise ValueError(f"workload line {physical_line} uses CRLF")
                if not header_seen:
                    decoded = decode_header(line)
                    if decoded != self.config:
                        raise ValueError("workload header changed between reads")
                    header_seen = True
                    continue
                command = decode_command(line)
                if encode_command(command) != line:
                    raise ValueError(f"workload line {physical_line} is not canonical")
                yield command
            if not header_seen:
                raise ValueError("workload is empty")


def generate_workload(
    spec: WorkloadSpec,
    seed: int,
    output: Path,
) -> WorkloadManifest:
    """Write a canonical LF-only native workload and return its resolved manifest."""

    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.tmp")
    digest = hashlib.sha256()
    generator = iter_generated(spec, seed)
    config = NativeInputConfig(
        instrument_id=spec.instrument_id,
        engine=spec.engine,
        snapshot_interval=spec.snapshot_interval,
    )
    with temporary.open("wb") as output_file:
        header = (encode_header(config) + "\n").encode("ascii")
        output_file.write(header)
        digest.update(header)
        for generated in generator:
            encoded = (encode_command(generated.command) + "\n").encode("ascii")
            output_file.write(encoded)
            digest.update(encoded)
        output_file.flush()
    temporary.replace(output)
    return WorkloadManifest(
        generator_version=GENERATOR_VERSION,
        seed=seed,
        spec=spec,
        command_count=spec.command_count,
        command_stream_sha256=digest.hexdigest(),
        stats=generator.stats,
    )


def open_workload(path: Path) -> WorkloadReader:
    """Open a canonical workload without retaining its command stream."""

    path = Path(path)
    with path.open("r", encoding="utf-8", newline="") as input_file:
        first = input_file.readline()
    if not first:
        raise ValueError("workload is empty")
    if not first.endswith("\n"):
        raise ValueError("workload header is not LF terminated")
    line = first[:-1]
    if line.endswith("\r"):
        raise ValueError("workload header uses CRLF")
    config = decode_header(line)
    if encode_header(config) != line:
        raise ValueError("workload header is not canonical")
    return WorkloadReader(path.resolve(), config)


def verify_workload(
    path: Path,
    manifest: WorkloadManifest,
    *,
    deadline: float | None = None,
) -> WorkloadReader:
    """Verify the byte digest, header, command count, and typed command grammar."""

    path = Path(path)
    digest = hashlib.sha256()
    with path.open("rb") as input_file:
        while chunk := input_file.read(1024 * 1024):
            _check_deadline(deadline)
            digest.update(chunk)
    if digest.hexdigest() != manifest.command_stream_sha256:
        raise ValueError("workload digest differs from its manifest")
    reader = open_workload(path)
    if reader.config != manifest.native_input:
        raise ValueError("workload header differs from its manifest")
    count = 0
    for _ in reader.commands():
        _check_deadline(deadline)
        count += 1
    if count != manifest.command_count:
        raise ValueError("workload command count differs from its manifest")
    return reader


def manifest_to_dict(manifest: WorkloadManifest) -> dict[str, object]:
    return {
        "schema": WORKLOAD_MANIFEST_SCHEMA,
        "generator_version": manifest.generator_version,
        "seed": str(manifest.seed),
        "spec": spec_to_dict(manifest.spec),
        "command_count": str(manifest.command_count),
        "command_stream_sha256": manifest.command_stream_sha256,
        "intent_counts": {
            "intended_valid": str(manifest.stats.intended_valid),
            "intended_invalid": str(manifest.stats.intended_invalid),
            "new_orders": str(manifest.stats.new_orders),
            "cancels": str(manifest.stats.cancels),
            "replaces": str(manifest.stats.replaces),
            "intents": {name: str(count) for name, count in manifest.stats.intents},
        },
    }


def manifest_from_dict(value: Mapping[str, object]) -> WorkloadManifest:
    expected = {
        "schema",
        "generator_version",
        "seed",
        "spec",
        "command_count",
        "command_stream_sha256",
        "intent_counts",
    }
    _require_keys(value, expected, "workload manifest")
    if value["schema"] != WORKLOAD_MANIFEST_SCHEMA:
        raise ValueError("unsupported workload manifest schema")
    counts = _mapping(value["intent_counts"], "intent_counts")
    _require_keys(
        counts,
        {
            "intended_valid",
            "intended_invalid",
            "new_orders",
            "cancels",
            "replaces",
            "intents",
        },
        "intent_counts",
    )
    command_count = _decimal(value["command_count"], "command_count")
    stats = GenerationStats(
        command_count=command_count,
        intended_valid=_decimal(counts["intended_valid"], "intended_valid"),
        intended_invalid=_decimal(counts["intended_invalid"], "intended_invalid"),
        new_orders=_decimal(counts["new_orders"], "new_orders"),
        cancels=_decimal(counts["cancels"], "cancels"),
        replaces=_decimal(counts["replaces"], "replaces"),
        intents=tuple(
            sorted(
                (
                    name,
                    _decimal(count, f"intent_counts.intents.{name}"),
                )
                for name, count in _mapping(
                    counts["intents"],
                    "intent_counts.intents",
                ).items()
            )
        ),
    )
    return WorkloadManifest(
        generator_version=_json_int(value["generator_version"], "generator_version"),
        seed=_decimal(value["seed"], "seed"),
        spec=spec_from_dict(_mapping(value["spec"], "spec")),
        command_count=command_count,
        command_stream_sha256=_string(
            value["command_stream_sha256"],
            "command_stream_sha256",
        ),
        stats=stats,
    )


def suite_to_dict(suite: CampaignSuite) -> dict[str, object]:
    return {
        "schema": CAMPAIGN_SUITE_SCHEMA,
        "name": suite.name,
        "tier": suite.tier,
        "mode": suite.mode,
        "cases": [
            {
                "name": case.name,
                "seed": str(case.seed),
                "spec": spec_to_dict(case.spec),
            }
            for case in suite.cases
        ],
    }


def suite_from_dict(value: Mapping[str, object]) -> CampaignSuite:
    _require_keys(value, {"schema", "name", "tier", "mode", "cases"}, "campaign suite")
    if value["schema"] != CAMPAIGN_SUITE_SCHEMA:
        raise ValueError("unsupported campaign suite schema")
    raw_cases = value["cases"]
    if not isinstance(raw_cases, list):
        raise ValueError("campaign cases must be an array")
    cases: list[CampaignCase] = []
    for index, raw_case in enumerate(raw_cases):
        case = _mapping(raw_case, f"cases[{index}]")
        _require_keys(case, {"name", "seed", "spec"}, f"cases[{index}]")
        cases.append(
            CampaignCase(
                name=_string(case["name"], "case name"),
                seed=_decimal(case["seed"], "case seed"),
                spec=spec_from_dict(_mapping(case["spec"], "case spec")),
            )
        )
    tier = _string(value["tier"], "tier")
    if tier not in {"pr", "main", "nightly", "release", "release_sanitizer"}:
        raise ValueError("campaign suite tier is outside the V1 vocabulary")
    mode = _string(value["mode"], "mode")
    if mode not in {"exact", "compact"}:
        raise ValueError("campaign suite mode is outside the native vocabulary")
    return CampaignSuite(
        name=_string(value["name"], "name"),
        tier=cast(CampaignTier, tier),
        mode=cast(OutputMode, mode),
        cases=tuple(cases),
    )


def canonical_json(value: Mapping[str, object]) -> str:
    """Return the stable JSON representation used for manifests and suites."""

    return json.dumps(value, ensure_ascii=True, separators=(",", ":"), sort_keys=True) + "\n"


def write_manifest(path: Path, manifest: WorkloadManifest) -> None:
    _write_json(path, manifest_to_dict(manifest))


def read_manifest(path: Path) -> WorkloadManifest:
    return manifest_from_dict(_read_json(path))


def write_suite(path: Path, suite: CampaignSuite) -> None:
    _write_json(path, suite_to_dict(suite))


def read_suite(path: Path) -> CampaignSuite:
    return suite_from_dict(_read_json(path))


def mapping_digest(value: Mapping[str, object]) -> str:
    return hashlib.sha256(canonical_json(value).encode("ascii")).hexdigest()


def _write_json(path: Path, value: Mapping[str, object]) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_text(canonical_json(value), encoding="ascii", newline="\n")
    temporary.replace(path)


def _read_json(path: Path) -> Mapping[str, object]:
    try:
        text = Path(path).read_text(encoding="ascii")
        value = json.loads(
            text,
            object_pairs_hook=_unique_object,
            parse_constant=_reject_json_constant,
        )
    except (OSError, UnicodeError, json.JSONDecodeError, RecursionError) as exc:
        raise ValueError(f"cannot read canonical JSON from {path}") from exc
    mapping = _mapping(value, "JSON document")
    if text != canonical_json(mapping):
        raise ValueError(f"JSON document is not canonical: {path}")
    return mapping


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
    return int(text)


def _json_int(value: object, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{name} must be a JSON integer")
    return value


def _require_u64(name: str, value: int) -> None:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= U64_MAX:
        raise ValueError(f"{name} must be a u64")


def _require_digest(value: str, name: str) -> None:
    if (
        len(value) != _SHA256_HEX_LENGTH
        or value.lower() != value
        or any(character not in "0123456789abcdef" for character in value)
    ):
        raise ValueError(f"{name} must be lowercase SHA-256 hex")


def _unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    value: dict[str, object] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"canonical JSON contains duplicate key {key!r}")
        value[key] = item
    return value


def _reject_json_constant(value: str) -> NoReturn:
    raise ValueError(f"canonical JSON contains invalid constant {value!r}")


def _check_deadline(deadline: float | None) -> None:
    if deadline is not None and time.monotonic() >= deadline:
        raise TimeoutError("workload verification timed out")
