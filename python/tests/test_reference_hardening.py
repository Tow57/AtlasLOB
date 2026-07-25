from __future__ import annotations

import pytest
from atlaslob.domain import NewOrder, OrderType, Side, TimeInForce
from atlaslob.reference import ReferenceEngine


def test_poisoned_reference_rejects_instrument_observation(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    engine = ReferenceEngine(7)
    command = NewOrder(
        client_id=1,
        order_id=1,
        instrument_id=7,
        side=Side.BUY,
        order_type=OrderType.LIMIT,
        time_in_force=TimeInForce.GTC,
        limit_price=100,
        quantity=1,
    )

    def fail_match(*_args: object) -> int:
        raise MemoryError("injected")

    monkeypatch.setattr(ReferenceEngine, "_match", fail_match)
    with pytest.raises(MemoryError, match="injected"):
        engine.execute(command)

    with pytest.raises(RuntimeError, match="poisoned"):
        _ = engine.instrument_id
    assert not engine.validate_invariants()
