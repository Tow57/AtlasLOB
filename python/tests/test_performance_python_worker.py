from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import importlib.util
import os
import sys
import types
import zipfile
from collections.abc import Callable, Iterable, MutableMapping, Sequence
from pathlib import Path
from typing import cast

import atlaslob
import pytest

_WORKER = Path(__file__).parents[2] / "benchmarks" / "python" / "atlas_python_bench_worker.py"


class _Distribution(importlib.metadata.Distribution):
    def __init__(self, root: Path) -> None:
        self._root = root

    @property
    def version(self) -> str:
        return "0.2.0"

    @property
    def files(self) -> list[importlib.metadata.PackagePath]:
        return [
            importlib.metadata.PackagePath("atlaslob/__init__.py"),
            importlib.metadata.PackagePath("atlaslob/engine.py"),
            importlib.metadata.PackagePath("atlaslob/_native_engine.test.so"),
            importlib.metadata.PackagePath("atlaslob/_native_engine.pyi"),
        ]

    def locate_file(self, path: str | os.PathLike[str]) -> Path:
        return self._root / str(path)

    def read_text(self, filename: str) -> str | None:
        del filename
        return None


def _load_worker() -> types.ModuleType:
    spec = importlib.util.spec_from_file_location("atlas_python_bench_worker_test", _WORKER)
    if spec is None or spec.loader is None:
        raise AssertionError("worker module could not be loaded")
    module = importlib.util.module_from_spec(spec)
    old_path = list(sys.path)
    old_pythonpath = os.environ.get("PYTHONPATH")
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path[:] = old_path
        if old_pythonpath is None:
            os.environ.pop("PYTHONPATH", None)
        else:
            os.environ["PYTHONPATH"] = old_pythonpath
    return module


def _wheel_fixture(tmp_path: Path) -> tuple[Path, _Distribution, Path, Path]:
    installed = tmp_path / "installed"
    package = installed / "atlaslob"
    package.mkdir(parents=True)
    (package / "__init__.py").write_bytes(b"__version__ = '0.2.0'\n")
    wrapper = package / "engine.py"
    extension = package / "_native_engine.test.so"
    type_stub = package / "_native_engine.pyi"
    wrapper.write_bytes(b"# wrapper\n")
    extension.write_bytes(b"native-extension")
    type_stub.write_bytes(b"# native typing surface\n")
    wheel = tmp_path / "atlaslob-0.2.0-cp312-cp312-manylinux.whl"
    with zipfile.ZipFile(wheel, "w") as archive:
        archive.writestr("atlaslob/__init__.py", (package / "__init__.py").read_bytes())
        archive.writestr("atlaslob/engine.py", wrapper.read_bytes())
        archive.writestr("atlaslob/_native_engine.test.so", extension.read_bytes())
        archive.writestr("atlaslob/_native_engine.pyi", type_stub.read_bytes())
        archive.writestr(
            "atlaslob-0.2.0.dist-info/METADATA",
            "Metadata-Version: 2.1\nName: atlaslob\nVersion: 0.2.0\n",
        )
    return wheel, _Distribution(installed), wrapper, extension


def test_worker_wheel_identity_binds_imported_bytes_to_supplied_wheel(
    tmp_path: Path,
) -> None:
    worker = _load_worker()
    verify = cast(
        Callable[[Path, importlib.metadata.Distribution, Path, Path], str],
        worker._verify_wheel_install,
    )
    wheel, distribution, wrapper, extension = _wheel_fixture(tmp_path)

    assert verify(wheel, distribution, wrapper, extension) == "0.2.0"

    extension.write_bytes(b"different-extension")
    with pytest.raises(ValueError, match="differs"):
        verify(wheel, distribution, wrapper, extension)


@pytest.mark.parametrize(
    "header",
    (
        "ATLAS_DIFF_V2 1 4097 1 0\n",
        "ATLAS_DIFF_V2 1 1 100000001 0\n",
        "ATLAS_DIFF_V2 1 1 1 1\n",
    ),
)
def test_worker_rejects_catalog_and_command_count_bombs(
    tmp_path: Path,
    header: str,
) -> None:
    worker = _load_worker()
    parse = cast(
        Callable[[Path, str], object],
        worker._parse_workload,
    )
    workload = tmp_path / "bomb.atlas"
    workload.write_text(header, encoding="ascii", newline="\n")
    digest = hashlib.sha256(workload.read_bytes()).hexdigest()

    with pytest.raises(ValueError, match="bound|checkpoint"):
        parse(workload, digest)


def test_worker_removes_source_shadowing_path() -> None:
    worker = _load_worker()
    remove = cast(Callable[[], None], worker._remove_source_shadowing)
    shadow = _WORKER.parents[2] / "python" / "src"
    old_path = list(sys.path)
    old_pythonpath = os.environ.get("PYTHONPATH")
    try:
        sys.path.insert(0, str(shadow))
        os.environ["PYTHONPATH"] = str(shadow)
        remove()
        assert shadow.resolve() not in {Path(value).resolve() for value in sys.path if value}
    finally:
        sys.path[:] = old_path
        if old_pythonpath is None:
            os.environ.pop("PYTHONPATH", None)
        else:
            os.environ["PYTHONPATH"] = old_pythonpath


def test_worker_prepares_chunks_before_timing_and_destroys_results_before_stop(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    worker = _load_worker()
    run_observation = cast(
        Callable[[argparse.Namespace], dict[str, object]],
        worker._run_observation,
    )
    final_digest = "f" * 64
    event_digest = hashlib.sha256(b"").hexdigest()
    commands = (object(), object())
    state = {"prepared": False, "destroyed": 0, "clock_calls": 0}

    class BatchResult:
        committed_count = 1
        rejected_count = 0
        processed_count = 1
        terminal_error = None

        def __del__(self) -> None:
            state["destroyed"] += 1

    class Engine:
        def __init__(
            self,
            catalog: Iterable[object],
            *,
            max_total_active_orders: int,
        ) -> None:
            tuple(catalog)
            assert max_total_active_orders == 10

        def submit_batch(
            self,
            batch: Sequence[object],
            *,
            output: str,
        ) -> BatchResult:
            assert len(batch) == 1
            assert output == "summary"
            return BatchResult()

        def state_digest(self) -> str:
            return final_digest

    def chunks(values: Sequence[object], size: int) -> Iterable[Sequence[object]]:
        state["prepared"] = True
        for begin in range(0, len(values), size):
            yield values[begin : begin + size]

    def clock() -> int:
        state["clock_calls"] += 1
        assert state["prepared"]
        if state["clock_calls"] == 2:
            assert state["destroyed"] == 2
        return 100 * state["clock_calls"]

    module_values = cast(MutableMapping[str, object], atlaslob.__dict__)
    monkeypatch.setitem(module_values, "Engine", Engine)
    monkeypatch.setattr(worker, "_parse_workload", lambda *_: ((10, (object(),)), commands))
    monkeypatch.setattr(worker, "_submit_prefix", lambda *_: None)
    monkeypatch.setattr(
        worker,
        "_validate_results",
        lambda *_: (2, 0, 2, 0, 0, event_digest, final_digest),
    )
    monkeypatch.setattr(worker, "_chunks", chunks)
    monkeypatch.setattr(worker, "_rss_bytes", lambda: 1_000)
    monkeypatch.setattr(worker, "_peak_rss_bytes", lambda: 1_000)
    monkeypatch.setattr(worker.time, "perf_counter_ns", clock)
    options = argparse.Namespace(
        workload=Path("unused"),
        workload_id="W04",
        workload_sha256="a" * 64,
        workload_manifest_sha256="b" * 64,
        binary_sha256="c" * 64,
        environment_sha256="d" * 64,
        host_context_sha256="e" * 64,
        suite_label="worker01",
        run_label="worker01-run",
        variant="standalone",
        block_index=0,
        block_position=0,
        preload_count=0,
        warmup_count=0,
        measured_count=2,
        expected_events=0,
        expected_committed=2,
        expected_rejected=0,
        expected_engine_errors=0,
        expected_event_digest=event_digest,
        expected_final_digest=final_digest,
        instrument_count="1",
        measured_start_active_order_count="0",
        sweep_depth="0",
        batch_size=1,
        output_mode="summary",
    )

    observation = run_observation(options)

    assert observation["valid"] is True
    assert observation["elapsed_ns"] == "100"
    assert state == {"prepared": True, "destroyed": 2, "clock_calls": 2}

    options.instrument_count = "2"
    with pytest.raises(ValueError, match="workload catalog"):
        run_observation(options)
