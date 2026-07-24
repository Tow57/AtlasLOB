from __future__ import annotations

from typing import cast

import pytest
from atlaslob.domain import (
    AcceptedEvent,
    EventBatch,
    EventHeader,
    MatchingConfig,
    NewOrder,
    Side,
    command_type,
)


def test_matching_config_rejects_invalid_resource_limits() -> None:
    with pytest.raises(ValueError, match="max_order_quantity"):
        MatchingConfig(max_order_quantity=0)
    with pytest.raises(ValueError, match="tick_increment"):
        MatchingConfig(tick_increment=0)
    with pytest.raises(ValueError, match="max_active_orders"):
        MatchingConfig(max_active_orders=-1)


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("max_order_quantity", cast(int, True)),
        ("max_order_quantity", cast(int, 1.5)),
        ("tick_increment", cast(int, False)),
        ("tick_increment", cast(int, 0.5)),
        ("max_active_orders", cast(int, True)),
        ("max_active_orders", cast(int, 1.5)),
    ],
)
def test_matching_config_requires_genuine_integer_scalars(field: str, value: int) -> None:
    with pytest.raises(ValueError, match=field):
        MatchingConfig(**{field: value})


def test_raw_invalid_enums_remain_representable_in_commands() -> None:
    command = NewOrder(
        client_id=1,
        order_id=2,
        instrument_id=7,
        side=255,
        order_type=254,
        time_in_force=253,
        limit_price=100,
        quantity=5,
    )

    assert command.side == 255
    assert command_type(command).value == 1


def test_event_batch_requires_contiguous_headers() -> None:
    first = AcceptedEvent(EventHeader(1, 0, 7), command_type(NewOrder(1, 1, 7, 1, 1, 1, 100, 1)))
    wrong_index = AcceptedEvent(
        EventHeader(1, 2, 7), command_type(NewOrder(1, 2, 7, 1, 1, 1, 100, 1))
    )

    with pytest.raises(ValueError, match="contiguous"):
        EventBatch((first, wrong_index))


def test_event_batch_exposes_classification_and_identity() -> None:
    batch = EventBatch(
        (
            AcceptedEvent(
                EventHeader(9, 0, 7), command_type(NewOrder(1, 1, 7, Side.BUY, 1, 1, 100, 1))
            ),
        )
    )

    assert batch.command_sequence == 9
    assert batch.instrument_id == 7
    assert batch.committed
    assert not batch.rejected
