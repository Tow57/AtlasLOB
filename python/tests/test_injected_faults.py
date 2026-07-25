from __future__ import annotations

import json
import os
from pathlib import Path

import pytest
from atlaslob.differential import FailureSignature
from atlaslob.domain import (
    Command,
    MatchingConfig,
    NewOrder,
    OrderType,
    Side,
    TimeInForce,
)
from atlaslob.faults import (
    INJECTED_FAULTS,
    FaultInjector,
    FaultName,
    injected_fault_signature,
    run_injected_fault,
)
from atlaslob.native import NativeInputConfig
from atlaslob.reporting import (
    BuildMetadata,
    persist_initial_failure,
    persist_minimized_failure,
)
from atlaslob.shrinking import (
    ShrinkBudget,
    ShrinkContext,
    shrink_failure,
)
from atlaslob.workload import open_workload

INSTRUMENT = 7
CONFIG = NativeInputConfig(
    instrument_id=INSTRUMENT,
    engine=MatchingConfig(
        max_order_quantity=1_000,
        tick_increment=1,
        max_active_orders=32,
    ),
    snapshot_interval=1,
)
REGRESSION_DIRECTORY = (
    Path(__file__).resolve().parents[2] / "python" / "corpus" / "regressions" / "phase3-injected"
)
REGRESSION_NAMES = {
    FaultName.NEWEST_AT_PRICE: "newest-at-price.atlas",
    FaultName.INCOMING_TRADE_PRICE: "incoming-trade-price.atlas",
    FaultName.STALE_PARTIAL_AGGREGATE: "stale-partial-aggregate.atlas",
}


def _executable() -> Path:
    configured = os.environ.get("ATLAS_DIFF_NATIVE")
    if configured is not None:
        candidate = Path(configured)
        if candidate.is_file():
            return candidate.resolve()
        raise FileNotFoundError(
            f"ATLAS_DIFF_NATIVE does not name a native evidence executable: {configured}"
        )
    for candidate in (
        Path("build/dev-gcc/atlas_diff_native.exe"),
        Path("build/dev-gcc/atlas_diff_native"),
    ):
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError("build atlas_diff_native or set ATLAS_DIFF_NATIVE")


def _limit(
    order_id: int,
    side: Side,
    price: int,
    quantity: int,
    *,
    client_id: int,
    time_in_force: TimeInForce = TimeInForce.GTC,
) -> NewOrder:
    return NewOrder(
        client_id=client_id,
        order_id=order_id,
        instrument_id=INSTRUMENT,
        side=side,
        order_type=OrderType.LIMIT,
        time_in_force=time_in_force,
        limit_price=price,
        quantity=quantity,
    )


def _market(
    order_id: int,
    side: Side,
    quantity: int,
    *,
    client_id: int,
) -> NewOrder:
    return NewOrder(
        client_id=client_id,
        order_id=order_id,
        instrument_id=INSTRUMENT,
        side=side,
        order_type=OrderType.MARKET,
        time_in_force=TimeInForce.IOC,
        limit_price=None,
        quantity=quantity,
    )


def _original(fault: FaultName) -> tuple[Command, ...]:
    noise = (
        _limit(900, Side.BUY, 10, 9, client_id=90),
        _market(901, Side.SELL, 9, client_id=91),
    )
    if fault == FaultName.NEWEST_AT_PRICE:
        trigger = (
            _limit(100, Side.SELL, 100, 5, client_id=10),
            _limit(200, Side.SELL, 100, 7, client_id=20),
        )
    elif fault == FaultName.INCOMING_TRADE_PRICE:
        trigger = (
            _limit(100, Side.SELL, 100, 5, client_id=10),
            _limit(
                200,
                Side.BUY,
                105,
                3,
                client_id=20,
                time_in_force=TimeInForce.IOC,
            ),
        )
    else:
        trigger = (
            _limit(100, Side.SELL, 100, 5, client_id=10),
            _market(200, Side.BUY, 2, client_id=20),
        )
    return noise + trigger + (_limit(999, Side.BUY, 1, 1, client_id=99),)


def test_initial_failure_persistence_resets_only_runner_owned_bundle_files(
    tmp_path: Path,
) -> None:
    executable = _executable()
    initial = run_injected_fault(
        executable,
        CONFIG,
        _original(FaultName.NEWEST_AT_PRICE),
        FaultName.NEWEST_AT_PRICE,
        tmp_path / "evaluation",
    )
    assert initial.divergence is not None
    bundle_root = tmp_path / "bundle"
    persist_initial_failure(initial, bundle_root, executable)

    manual = bundle_root / "reproduction" / "notes.txt"
    manual.parent.mkdir()
    manual.write_text("keep me", encoding="ascii")
    owned_names = (
        "original.atlas",
        "manifest.json",
        "source-manifest.json",
        "reference-original.jsonl",
        "native-original.jsonl",
        "report.json",
        "minimized.atlas",
        "reference-minimized.jsonl",
        "native-minimized.jsonl",
    )
    for name in owned_names:
        if name not in {"original.atlas", "report.json"}:
            (bundle_root / name).write_text("stale", encoding="ascii")
        (bundle_root / f".{name}.tmp").write_text("stale temporary", encoding="ascii")

    bundle = persist_initial_failure(
        initial,
        bundle_root,
        executable,
        retain_transcripts=False,
    )

    assert manual.read_text(encoding="ascii") == "keep me"
    assert bundle.original_workload_path.is_file()
    assert bundle.report_path.is_file()
    assert not (bundle_root / "manifest.json").exists()
    assert not (bundle_root / "source-manifest.json").exists()
    assert not (bundle_root / "reference-original.jsonl").exists()
    assert not (bundle_root / "native-original.jsonl").exists()
    assert not (bundle_root / "minimized.atlas").exists()
    assert not (bundle_root / "reference-minimized.jsonl").exists()
    assert not (bundle_root / "native-minimized.jsonl").exists()
    assert not tuple(bundle_root.glob(".*.tmp"))
    report = json.loads(bundle.report_path.read_text(encoding="ascii"))
    assert report["files"]["manifest"] is None
    assert report["files"]["source_manifest"] is None
    assert report["files"]["minimized_workload"] is None


def test_initial_failure_persistence_rejects_an_owned_directory_before_mutation(
    tmp_path: Path,
) -> None:
    executable = _executable()
    initial = run_injected_fault(
        executable,
        CONFIG,
        _original(FaultName.NEWEST_AT_PRICE),
        FaultName.NEWEST_AT_PRICE,
        tmp_path / "evaluation",
    )
    assert initial.divergence is not None
    bundle_root = tmp_path / "bundle"
    bundle_root.mkdir()
    original = bundle_root / "original.atlas"
    original.write_text("prior bundle", encoding="ascii")
    (bundle_root / "minimized.atlas").mkdir()

    with pytest.raises(ValueError, match="unexpected type"):
        persist_initial_failure(initial, bundle_root, executable)

    assert original.read_text(encoding="ascii") == "prior bundle"
    assert (bundle_root / "minimized.atlas").is_dir()


def test_initial_failure_persistence_rejects_a_symlinked_output_ancestor(
    tmp_path: Path,
) -> None:
    executable = _executable()
    initial = run_injected_fault(
        executable,
        CONFIG,
        _original(FaultName.NEWEST_AT_PRICE),
        FaultName.NEWEST_AT_PRICE,
        tmp_path / "evaluation",
    )
    assert initial.divergence is not None
    target = tmp_path / "protected"
    target.mkdir()
    sentinel = target / "sentinel.txt"
    sentinel.write_text("do not delete", encoding="ascii")
    linked_parent = tmp_path / "linked-parent"
    try:
        linked_parent.symlink_to(target, target_is_directory=True)
    except OSError as exc:
        pytest.skip(f"directory symlinks are unavailable: {exc}")

    with pytest.raises(ValueError, match="link or junction"):
        persist_initial_failure(initial, linked_parent / "bundle", executable)

    assert sentinel.read_text(encoding="ascii") == "do not delete"
    assert not (target / "bundle").exists()


@pytest.mark.campaign
@pytest.mark.parametrize("fault", INJECTED_FAULTS, ids=lambda fault: fault.value)
def test_each_known_fault_is_detected_and_semantically_shrunk(
    fault: FaultName,
    tmp_path: Path,
) -> None:
    executable = _executable()
    original = _original(fault)
    evaluation_directory = tmp_path / fault.value

    initial = run_injected_fault(
        executable,
        CONFIG,
        original,
        fault,
        evaluation_directory,
    )
    assert initial.status == "diverged"
    assert initial.divergence is not None
    assert initial.native_evidence_path is not None
    assert initial.native_evidence_path.is_file()
    signature = initial.divergence.signature
    assert signature is not None
    bundle = persist_initial_failure(
        initial,
        tmp_path / "bundle",
        executable,
        build=BuildMetadata(
            revision="test-revision",
            compiler="test-compiler",
            build_type="test",
        ),
        injected_fault=fault.value,
        source_bundle="../source",
    )
    assert bundle.original_workload_path.is_file()
    assert bundle.original_reference_path is not None
    assert bundle.original_native_path is not None
    initial_report = json.loads(bundle.report_path.read_text(encoding="ascii"))
    assert initial_report["stage"] == "initial"
    assert initial_report["source_provenance"] == {
        "manifest": None,
        "manifest_file": None,
        "relationship": "derived_exact_prefix_of_source_bundle",
        "source_bundle": "../source",
    }

    def evaluate(
        commands: tuple[Command, ...],
        remaining: float | None = None,
    ) -> FailureSignature | None:
        return injected_fault_signature(
            executable,
            CONFIG,
            commands,
            fault,
            evaluation_directory,
            timeout=30.0 if remaining is None else min(30.0, remaining),
        )

    result = shrink_failure(
        original,
        evaluate,
        signature,
        context=ShrinkContext(
            routed_instrument=INSTRUMENT,
            tick_increment=CONFIG.engine.tick_increment,
            max_quantity=CONFIG.engine.max_order_quantity,
        ),
        budget=ShrinkBudget(max_evaluations=500, timeout_seconds=30.0),
    )

    assert not result.budget_exhausted
    assert len(result.commands) == 2
    assert result.completed_stages[-1] == "final_deletion"
    assert evaluate(result.commands) == signature
    checked = open_workload(REGRESSION_DIRECTORY / REGRESSION_NAMES[fault])
    assert checked.config == CONFIG
    assert tuple(checked.commands()) == result.commands

    minimized = persist_minimized_failure(
        bundle,
        result,
        executable,
        CONFIG,
        evidence_transformer=FaultInjector(fault),
    )
    assert minimized.divergence is not None
    report_text = bundle.report_path.read_text(encoding="ascii")
    report = json.loads(report_text)
    assert report["stage"] == "minimized"
    assert report["failure"]["first_field"] == signature.field_path
    assert report["shrink"]["minimized_command_count"] == "2"
    assert report["shrink"]["configured_max_evaluations"] == "500"
    assert report["shrink"]["configured_timeout_seconds"] == "30"
    assert float(report["shrink"]["elapsed_seconds"]) >= 0.0
    assert report["shrink"]["status"] == "completed"
    assert report["files"]["original_reference_output"] == "reference-original.jsonl"
    assert report["files"]["original_native_output"] == "native-original.jsonl"
    assert report["files"]["minimized_reference_output"] == "reference-minimized.jsonl"
    assert report["files"]["minimized_native_output"] == "native-minimized.jsonl"
    assert all(
        isinstance(digest, str) and len(digest) == 64
        for digest in report["artifact_sha256"].values()
        if digest is not None
    )
    assert len(report["minimized_digests"]["command_records"]) == 64
    assert str(tmp_path) not in report_text
