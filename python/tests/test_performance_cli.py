from __future__ import annotations

from pathlib import Path

import pytest
from atlaslob.performance import cli


def test_materialize_plan_is_mutually_exclusive_with_single_workload() -> None:
    with pytest.raises(SystemExit):
        cli.parser().parse_args(
            (
                "materialize",
                "--output",
                "out",
                "--plan",
                "benchmarks/plans/ci-smoke-v1.json",
                "--workload",
                "W04",
            )
        )


def test_plan_materialization_rejects_single_workload_shape_options(
    capsys: pytest.CaptureFixture[str],
) -> None:
    status = cli.main(
        [
            "materialize",
            "--output",
            "out",
            "--plan",
            "unused.json",
            "--seed",
            "1",
        ]
    )

    assert status == 1
    assert "single-workload shape options" in capsys.readouterr().err


@pytest.mark.parametrize(
    "mode",
    ("python-objects", "python-columns", "python-summary"),
)
def test_run_suite_parser_exposes_every_python_output_boundary(mode: str) -> None:
    options = cli.parser().parse_args(
        (
            "run-suite",
            "--manifest",
            "workload.json",
            "--output",
            "observations",
            "--runner",
            "python",
            "--environment",
            "environment.json",
            "--wheel",
            "atlaslob.whl",
            "--python-worker",
            "worker.py",
            "--suite-label",
            "python01",
            "--mode",
            mode,
            "--batch-size",
            "1024",
        )
    )

    assert options.mode == mode
    assert options.batch_size == 1024


def test_run_suite_parser_exposes_opt_in_native_diagnostic_phases() -> None:
    options = cli.parser().parse_args(
        (
            "run-suite",
            "--manifest",
            "workload.json",
            "--output",
            "observations",
            "--runner",
            "runner",
            "--environment",
            "environment.json",
            "--suite-label",
            "native01",
            "--diagnostic-phases",
        )
    )

    assert options.diagnostic_phases is True


def test_candidate_wheel_requires_candidate_runner(
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    called = False

    def fail_if_called(*_arguments: object, **_keywords: object) -> tuple[Path, ...]:
        nonlocal called
        called = True
        return ()

    monkeypatch.setattr(cli, "run_suite", fail_if_called)
    status = cli.main(
        [
            "run-suite",
            "--manifest",
            "workload.json",
            "--output",
            "observations",
            "--runner",
            "runner",
            "--environment",
            "environment.json",
            "--suite-label",
            "native01",
            "--candidate-wheel",
            "candidate.whl",
        ]
    )

    assert status == 1
    assert not called
    assert "candidate wheel requires" in capsys.readouterr().err


def test_campaign_filters_are_mutually_exclusive() -> None:
    with pytest.raises(SystemExit):
        cli.parser().parse_args(
            (
                "run-campaign",
                "--plan",
                "plan.json",
                "--bundle",
                "bundle",
                "--runner",
                "runner",
                "--environment",
                "environment.json",
                "--suite-label",
                "campaign01",
                "--point",
                "point-a",
                "--tier",
                "study",
            )
        )


def test_ordered_campaign_is_mutually_exclusive_and_requires_checkpoint_directory(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    common = (
        "run-campaign",
        "--plan",
        "plan.json",
        "--bundle",
        "bundle",
        "--runner",
        "runner",
        "--environment",
        "environment.json",
        "--suite-label",
        "campaign01",
    )
    with pytest.raises(SystemExit):
        cli.parser().parse_args((*common, "--ordered-tiers", "--tier", "study"))

    called = False

    def fail_if_called(*_arguments: object, **_keywords: object) -> object:
        nonlocal called
        called = True
        return object()

    monkeypatch.setattr(cli, "run_ordered_campaign", fail_if_called)
    status = cli.main([*common, "--ordered-tiers"])

    assert status == 1
    assert not called
    assert "requires --checkpoint-directory" in capsys.readouterr().err


def test_campaign_rejects_partial_runner_families(
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    called = False

    def fail_if_called(*_arguments: object, **_keywords: object) -> object:
        nonlocal called
        called = True
        return object()

    monkeypatch.setattr(cli, "run_campaign", fail_if_called)
    status = cli.main(
        [
            "run-campaign",
            "--plan",
            "plan.json",
            "--bundle",
            "bundle",
            "--runner",
            "runner",
            "--environment",
            "environment.json",
            "--allocation-runner",
            "allocation-runner",
            "--suite-label",
            "campaign01",
        ]
    )

    assert status == 1
    assert not called
    assert "supplied together" in capsys.readouterr().err


def test_profile_parser_exposes_fixed_capture_modes() -> None:
    options = cli.parser().parse_args(
        (
            "capture-profile",
            "--manifest",
            "workload.json",
            "--output",
            "profile",
            "--runner",
            "atlas_bench_runner",
            "--environment",
            "environment.json",
            "--perf",
            "/usr/bin/perf",
            "--suite-label",
            "profile01",
            "--kind",
            "record",
        )
    )

    assert options.kind == "record"
    assert options.observations is None


@pytest.mark.parametrize(
    "arguments",
    (
        ("--workload", "W10"),
        ("--workload", "W04", "--log-materializer", "materializer"),
    ),
)
def test_materialize_log_materializer_presence_matches_w10_selection(
    arguments: tuple[str, ...],
    capsys: pytest.CaptureFixture[str],
) -> None:
    status = cli.main(["materialize", "--output", "out", *arguments])

    assert status == 1
    assert "required exactly" in capsys.readouterr().err


@pytest.mark.parametrize(
    "value",
    ("-1", "01", "+1", str(1 << 64)),
)
def test_cli_rejects_noncanonical_or_out_of_range_unsigned_values(value: str) -> None:
    with pytest.raises(SystemExit):
        cli.parser().parse_args(
            (
                "materialize",
                "--output",
                "out",
                "--workload",
                "W04",
                "--seed",
                value,
            )
        )
