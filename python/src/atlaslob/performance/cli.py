"""Command-line interface for Phase 5 performance evidence."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from atlaslob.performance.analysis import (
    analyze_paths,
    load_experiment_plans,
    render_report_markdown,
    render_report_svg,
)
from atlaslob.performance.bundle import verify_bundle, write_inventory
from atlaslob.performance.campaign import (
    CampaignRunners,
    finalize_campaign,
    run_campaign,
    run_ordered_campaign,
)
from atlaslob.performance.environment import (
    capture_environment,
    capture_python_environment,
)
from atlaslob.performance.profiling import capture_profile
from atlaslob.performance.schemas import write_canonical_document
from atlaslob.performance.suite import Runner, run_suite
from atlaslob.performance.workloads import (
    WORKLOAD_IDS,
    load_benchmark_plan,
    materialize_benchmark_plan,
    materialize_workload,
)


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(prog="python -m atlaslob.performance")
    commands = root.add_subparsers(dest="command", required=True)

    materialize = commands.add_parser("materialize")
    materialize.add_argument("--output", type=Path, required=True)
    selection = materialize.add_mutually_exclusive_group()
    selection.add_argument("--plan", type=Path)
    selection.add_argument("--workload", choices=WORKLOAD_IDS, action="append")
    materialize.add_argument("--log-materializer", type=Path)
    materialize.add_argument("--seed", type=_u64)
    materialize.add_argument("--preload", type=_u64)
    materialize.add_argument("--warmup", type=_u64)
    materialize.add_argument("--measured", type=_positive_u64)
    materialize.add_argument("--instruments", type=_positive_u64)
    materialize.add_argument("--active-orders", type=_positive_u64)
    materialize.add_argument("--sweep-depth", choices=(1, 8, 16, 32, 64), type=int)

    environment = commands.add_parser("capture-environment")
    environment.add_argument("--binary", type=Path)
    environment.add_argument("--target-python", type=Path)
    environment.add_argument("--wheel", type=Path)
    environment.add_argument("--python-worker", type=Path)
    environment.add_argument("--build-directory", type=Path, required=True)
    environment.add_argument("--output", type=Path, required=True)
    environment.add_argument("--repository", type=Path, default=Path.cwd())
    environment.add_argument("--host-class", required=True)
    environment.add_argument("--storage-class", required=True)
    environment.add_argument(
        "--smt-sibling-idle",
        choices=("yes", "no", "unknown"),
        required=True,
    )
    environment.add_argument("--official", action="store_true")

    suite = commands.add_parser("run-suite")
    suite.add_argument("--manifest", type=Path, required=True)
    suite.add_argument("--output", type=Path, required=True)
    suite.add_argument("--runner", type=Path, required=True)
    suite.add_argument("--environment", type=Path, required=True)
    suite.add_argument("--wheel", type=Path)
    suite.add_argument("--python-worker", type=Path)
    suite.add_argument("--suite-label", required=True)
    suite.add_argument("--candidate-runner", type=Path)
    suite.add_argument("--candidate-environment", type=Path)
    suite.add_argument("--candidate-wheel", type=Path)
    suite.add_argument(
        "--mode",
        choices=(
            "throughput",
            "latency",
            "allocation",
            "construction",
            "preload",
            "setup-allocation",
            "replay-fast",
            "replay-verify",
            "python-objects",
            "python-columns",
            "python-summary",
        ),
        default="throughput",
    )
    suite.add_argument("--observations", type=_positive_u64)
    suite.add_argument("--block-start", type=_positive_u64, default=1)
    suite.add_argument("--batch-size", type=int, choices=(1, 64, 1024, 65_536))
    suite.add_argument("--timeout", type=_positive_u64, default=900)

    campaign = commands.add_parser("run-campaign")
    campaign.add_argument("--plan", type=Path, required=True)
    campaign.add_argument("--bundle", type=Path, required=True)
    campaign.add_argument("--runner", type=Path, required=True)
    campaign.add_argument("--environment", type=Path, required=True)
    campaign.add_argument("--allocation-runner", type=Path)
    campaign.add_argument("--allocation-environment", type=Path)
    campaign.add_argument("--python-runner", type=Path)
    campaign.add_argument("--python-environment", type=Path)
    campaign.add_argument("--wheel", type=Path)
    campaign.add_argument("--python-worker", type=Path)
    campaign.add_argument("--suite-label", required=True)
    campaign.add_argument("--valid-observations", type=_positive_u64, default=10)
    campaign.add_argument("--max-attempts", type=_positive_u64, default=20)
    campaign.add_argument("--timeout", type=_positive_u64, default=900)
    filters = campaign.add_mutually_exclusive_group()
    filters.add_argument("--point", action="append", default=[])
    filters.add_argument("--tier", action="append", default=[])
    filters.add_argument("--ordered-tiers", action="store_true")
    campaign.add_argument("--checkpoint-directory", type=Path)
    campaign.add_argument("--resume", action="store_true")

    finalize = commands.add_parser("finalize-campaign")
    finalize.add_argument("--plan", type=Path, required=True)
    finalize.add_argument("--bundle", type=Path, required=True)
    finalize.add_argument("--valid-observations", type=_positive_u64, default=10)
    finalize.add_argument("--allow-exploratory", action="store_true")

    profile = commands.add_parser("capture-profile")
    profile.add_argument("--manifest", type=Path, required=True)
    profile.add_argument("--output", type=Path, required=True)
    profile.add_argument("--runner", type=Path, required=True)
    profile.add_argument("--environment", type=Path, required=True)
    profile.add_argument("--perf", type=Path, required=True)
    profile.add_argument("--suite-label", required=True)
    profile.add_argument("--kind", choices=("stat", "record"), required=True)
    profile.add_argument("--observations", type=_positive_u64)
    profile.add_argument("--timeout", type=_positive_u64, default=900)

    analyze = commands.add_parser("analyze")
    analyze.add_argument("input", type=Path)
    analyze.add_argument("--output", type=Path, required=True)
    analyze.add_argument("--manifest", type=Path, action="append", required=True)
    analyze.add_argument("--environment", type=Path, action="append", required=True)
    analyze.add_argument("--experiment-plan", type=Path)
    analyze.add_argument("--markdown", type=Path)
    analyze.add_argument("--svg", type=Path)

    verify = commands.add_parser("verify-bundle")
    verify.add_argument("bundle", type=Path)
    verify.add_argument("--create-inventory", action="store_true")
    return root


def main(arguments: list[str] | None = None) -> int:
    options = parser().parse_args(arguments)
    try:
        if options.command == "materialize":
            return _materialize(options)
        if options.command == "capture-environment":
            return _capture_environment(options)
        if options.command == "run-suite":
            return _run_suite(options)
        if options.command == "run-campaign":
            return _run_campaign(options)
        if options.command == "finalize-campaign":
            return _finalize_campaign(options)
        if options.command == "capture-profile":
            return _capture_profile(options)
        if options.command == "analyze":
            return _analyze(options)
        if options.command == "verify-bundle":
            if options.create_inventory:
                write_inventory(options.bundle)
            summary = verify_bundle(options.bundle)
            print(
                f"verified files={summary.files} workloads={summary.workloads} "
                f"environments={summary.environments} "
                f"observations={summary.observations} reports={summary.reports}"
            )
            return 0
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    raise AssertionError("argparse accepted an unknown command")


def _materialize(options: argparse.Namespace) -> int:
    if options.plan is not None:
        if any(
            value is not None
            for value in (
                options.seed,
                options.preload,
                options.warmup,
                options.measured,
                options.instruments,
                options.active_orders,
                options.sweep_depth,
            )
        ):
            raise ValueError("plan materialization cannot use single-workload shape options")
        plan = load_benchmark_plan(options.plan)
        plan_has_replay = any(point.workload_id == "W10" for point in plan.points)
        if plan_has_replay != (options.log_materializer is not None):
            raise ValueError("a log materializer is required exactly when the plan contains W10")
        artifacts = materialize_benchmark_plan(
            plan,
            options.output,
            log_materializer=options.log_materializer,
        )
        for point, (path, manifest) in zip(plan.points, artifacts, strict=True):
            print(f"{point.point_id} {manifest.workload_id} {path.name} {manifest.stream_sha256}")
        return 0

    workload_ids = WORKLOAD_IDS if options.workload is None else tuple(options.workload)
    has_replay = "W10" in workload_ids
    if has_replay != (options.log_materializer is not None):
        raise ValueError("a log materializer is required exactly when the selection contains W10")
    seed = 1 if options.seed is None else options.seed
    instruments_value = 1 if options.instruments is None else options.instruments
    active_orders = 64 if options.active_orders is None else options.active_orders
    sweep_depth = 16 if options.sweep_depth is None else options.sweep_depth
    for workload_id in workload_ids:
        instruments = instruments_value if workload_id == "W09" else 1
        preload = (
            options.preload
            if options.preload is not None
            else 0
            if workload_id == "W10"
            else active_orders
            if workload_id in {"W04", "W05"}
            else 64
        )
        warmup = (
            options.warmup
            if options.warmup is not None
            else 0
            if workload_id == "W10"
            else 40
            if workload_id == "W04"
            else 2 * (sweep_depth + 1)
            if workload_id == "W05"
            else 64
        )
        measured = (
            options.measured
            if options.measured is not None
            else 200
            if workload_id == "W04"
            else 12 * (sweep_depth + 1)
            if workload_id == "W05"
            else 256
        )
        path, manifest = materialize_workload(
            workload_id,
            options.output,
            seed=seed,
            preload_commands=preload,
            warmup_commands=warmup,
            measured_commands=measured,
            instrument_count=instruments,
            active_order_target=active_orders,
            sweep_depth=sweep_depth,
            log_materializer=(options.log_materializer if workload_id == "W10" else None),
        )
        print(f"{manifest.workload_id} {path.name} {manifest.stream_sha256}")
    return 0


def _capture_environment(options: argparse.Namespace) -> int:
    sibling_idle = (
        None if options.smt_sibling_idle == "unknown" else options.smt_sibling_idle == "yes"
    )
    python_inputs = (
        options.target_python,
        options.wheel,
        options.python_worker,
    )
    if options.binary is not None and any(item is not None for item in python_inputs):
        raise ValueError("--binary is mutually exclusive with target-wheel inputs")
    if options.binary is None and any(item is None for item in python_inputs):
        raise ValueError("supply --binary or all of --target-python, --wheel, and --python-worker")
    if options.binary is not None:
        value = capture_environment(
            options.binary,
            build_directory=options.build_directory,
            host_class=options.host_class,
            storage_class=options.storage_class,
            smt_sibling_idle=sibling_idle,
            repository=options.repository,
            request_official=options.official,
        )
    else:
        assert options.target_python is not None
        assert options.wheel is not None
        assert options.python_worker is not None
        value = capture_python_environment(
            options.target_python,
            options.wheel,
            options.python_worker,
            build_directory=options.build_directory,
            host_class=options.host_class,
            storage_class=options.storage_class,
            smt_sibling_idle=sibling_idle,
            repository=options.repository,
            request_official=options.official,
        )
    digest = write_canonical_document(options.output, value)
    print(f"{value.classification} {digest}")
    return 0


def _run_suite(options: argparse.Namespace) -> int:
    if (options.candidate_runner is None) != (options.candidate_environment is None):
        raise ValueError("candidate runner and environment must be supplied together")
    if options.candidate_runner is None and options.candidate_wheel is not None:
        raise ValueError("candidate wheel requires a candidate runner")
    candidate = (
        None
        if options.candidate_runner is None
        else Runner(options.candidate_runner, options.candidate_environment, "candidate")
    )
    if candidate is not None:
        candidate = Runner(
            candidate.executable,
            candidate.environment_path,
            candidate.variant,
            wheel=options.candidate_wheel,
            worker=options.python_worker,
        )
    paths = run_suite(
        options.manifest,
        options.output,
        baseline=Runner(
            options.runner,
            options.environment,
            "baseline" if candidate is not None else "standalone",
            wheel=options.wheel,
            worker=options.python_worker,
        ),
        candidate=candidate,
        suite_label=options.suite_label,
        mode=options.mode,
        observations=options.observations,
        block_start=options.block_start,
        batch_size=options.batch_size,
        timeout_seconds=options.timeout,
    )
    print(f"retained {len(paths)} observations")
    return 0


def _run_campaign(options: argparse.Namespace) -> int:
    allocation_inputs = (options.allocation_runner, options.allocation_environment)
    if (allocation_inputs[0] is None) != (allocation_inputs[1] is None):
        raise ValueError("allocation runner and environment must be supplied together")
    python_inputs = (
        options.python_runner,
        options.python_environment,
        options.wheel,
        options.python_worker,
    )
    if any(item is not None for item in python_inputs) and any(
        item is None for item in python_inputs
    ):
        raise ValueError("Python campaign inputs must be supplied together")
    runners = CampaignRunners(
        core=Runner(options.runner, options.environment, "standalone"),
        allocation=(
            None
            if options.allocation_runner is None
            else Runner(
                options.allocation_runner,
                options.allocation_environment,
                "standalone",
            )
        ),
        python=(
            None
            if options.python_runner is None
            else Runner(
                options.python_runner,
                options.python_environment,
                "standalone",
                wheel=options.wheel,
                worker=options.python_worker,
            )
        ),
    )
    if options.ordered_tiers:
        if options.checkpoint_directory is None:
            raise ValueError("ordered campaign requires --checkpoint-directory")
        ordered = run_ordered_campaign(
            options.plan,
            options.bundle,
            options.checkpoint_directory,
            runners=runners,
            suite_label=options.suite_label,
            valid_observations=options.valid_observations,
            max_attempts=options.max_attempts,
            timeout_seconds=options.timeout,
            resume=options.resume,
        )
        for tier in ordered.tiers:
            print(
                f"tier={tier.tier} shapes={tier.campaign.shapes} "
                f"attempts={tier.campaign.attempts} valid={tier.campaign.valid} "
                f"invalid={tier.campaign.invalid} elapsed={tier.elapsed_seconds:.3f}s "
                f"checkpoint={tier.checkpoint_path} "
                f"checkpoint_sha256={tier.checkpoint_sha256}"
            )
        summary = ordered.campaign
    else:
        if options.checkpoint_directory is not None:
            raise ValueError("--checkpoint-directory requires --ordered-tiers")
        summary = run_campaign(
            options.plan,
            options.bundle,
            runners=runners,
            suite_label=options.suite_label,
            valid_observations=options.valid_observations,
            max_attempts=options.max_attempts,
            timeout_seconds=options.timeout,
            point_ids=options.point,
            tiers=options.tier,
            resume=options.resume,
        )
    print(
        f"campaign shapes={summary.shapes} attempts={summary.attempts} "
        f"valid={summary.valid} invalid={summary.invalid}"
    )
    return 0


def _finalize_campaign(options: argparse.Namespace) -> int:
    summary = finalize_campaign(
        options.plan,
        options.bundle,
        valid_observations=options.valid_observations,
        allow_exploratory=options.allow_exploratory,
    )
    print(
        f"finalized files={summary.files} workloads={summary.workloads} "
        f"environments={summary.environments} "
        f"observations={summary.observations} reports={summary.reports}"
    )
    return 0


def _capture_profile(options: argparse.Namespace) -> int:
    observations = (
        10
        if options.observations is None and options.kind == "stat"
        else 1
        if options.observations is None
        else options.observations
    )
    summary = capture_profile(
        options.manifest,
        options.output,
        runner=Runner(options.runner, options.environment, "standalone"),
        perf_executable=options.perf,
        suite_label=options.suite_label,
        kind=options.kind,
        observations=observations,
        timeout_seconds=options.timeout,
    )
    print(f"profile kind={summary.kind} captures={summary.captures}")
    return 0


def _analyze(options: argparse.Namespace) -> int:
    paths = tuple(sorted(options.input.glob("observation-*.json")))
    report = analyze_paths(
        paths,
        workload_paths=tuple(options.manifest),
        environment_paths=tuple(options.environment),
        experiment_plans=(
            ()
            if options.experiment_plan is None
            else load_experiment_plans(options.experiment_plan)
        ),
    )
    digest = write_canonical_document(options.output, report)
    if options.markdown is not None:
        options.markdown.write_text(render_report_markdown(report), encoding="ascii", newline="\n")
    if options.svg is not None:
        options.svg.write_text(render_report_svg(report), encoding="ascii", newline="\n")
    print(digest)
    return 0


def _u64(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("value must be a u64") from exc
    if str(parsed) != value or not 0 <= parsed <= (1 << 64) - 1:
        raise argparse.ArgumentTypeError("value must be canonical u64")
    return parsed


def _positive_u64(value: str) -> int:
    parsed = _u64(value)
    if parsed == 0:
        raise argparse.ArgumentTypeError("value must be nonzero")
    return parsed
