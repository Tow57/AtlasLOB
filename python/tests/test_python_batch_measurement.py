from __future__ import annotations

import subprocess
import sys

_NATIVE_EQUIVALENCE = r"""
from pathlib import Path
from tempfile import TemporaryDirectory

from atlaslob.domain import (
    InstrumentConfig,
    MatchingConfig,
    NewOrder,
    OrderType,
    Side,
    TimeInForce,
)
from atlaslob.engine import ColumnBatch, Engine, ObjectBatch, SummaryBatch


def commands(count):
    return tuple(
        NewOrder(
            client_id=1 + index % 8,
            order_id=1 + index,
            instrument_id=1,
            side=Side.BUY if index % 2 == 0 else Side.SELL,
            order_type=OrderType.LIMIT,
            time_in_force=TimeInForce.GTC,
            limit_price=9_900 if index % 2 == 0 else 10_100,
            quantity=1,
        )
        for index in range(count)
    )


def chunks(values, size):
    for begin in range(0, len(values), size):
        yield values[begin : begin + size]


catalog = (InstrumentConfig(1, MatchingConfig(max_active_orders=256)),)
for batch_size in (1, 64, 1024, 65_536):
    for output in ("objects", "columns", "summary"):
        public_engine = Engine(catalog, max_total_active_orders=256)
        measurement_engine = Engine(catalog, max_total_active_orders=256)
        values = commands(80)
        observed_batch_lengths = []
        for batch in chunks(values, batch_size):
            public = public_engine.submit_batch(batch, output=output)
            measured = measurement_engine._submit_batch_for_measurement(batch, output=output)
            observed_batch_lengths.append(measured.submitted_count)
            assert measured.submitted_count == public.submitted_count == len(batch)
            assert measured.processed_count == public.processed_count
            assert measured.committed_count == public.committed_count
            assert measured.rejected_count == public.rejected_count
            assert measured.terminal_error == public.terminal_error
            assert type(measured.payload) is type(public.payload)
            if output == "objects":
                assert isinstance(measured.payload, ObjectBatch)
                assert isinstance(public.payload, ObjectBatch)
                assert measured.payload.results == public.payload.results
            elif output == "columns":
                assert isinstance(measured.payload, ColumnBatch)
                assert isinstance(public.payload, ColumnBatch)
                assert measured.payload.columns.keys() == public.payload.columns.keys()
                for name in measured.payload.columns:
                    assert (
                        measured.payload.columns[name].typecode
                        == public.payload.columns[name].typecode
                    )
                    assert (
                        measured.payload.columns[name].tolist()
                        == public.payload.columns[name].tolist()
                    )
            else:
                assert isinstance(measured.payload, SummaryBatch)
                assert isinstance(public.payload, SummaryBatch)
            assert not hasattr(measured, "final_state_digest")
            assert public.final_state_digest == public_engine.state_digest()
            assert public.final_state_digest == measurement_engine.state_digest()
        expected_lengths = [
            min(batch_size, len(values) - begin)
            for begin in range(0, len(values), batch_size)
        ]
        assert observed_batch_lengths == expected_lengths
        assert public_engine.state_digest() == measurement_engine.state_digest()

with TemporaryDirectory() as temporary:
    logged = Engine.create_logged(
        Path(temporary) / "measurement-path.atlslg",
        (InstrumentConfig(1, MatchingConfig(max_active_orders=4)),),
        max_total_active_orders=4,
        durability="buffered",
    )
    try:
        logged._submit_batch_for_measurement(commands(1), output="summary")
    except RuntimeError as error:
        assert "live in-memory" in str(error)
    else:
        raise AssertionError("logged engine accepted the private measurement path")
"""


def test_native_measurement_batches_preserve_payloads_boundaries_and_digests() -> None:
    completed = subprocess.run(
        [sys.executable, "-c", _NATIVE_EQUIVALENCE],
        check=False,
        capture_output=True,
        text=True,
    )

    assert completed.returncode == 0, completed.stderr
    assert completed.stdout == ""
    assert completed.stderr == ""
