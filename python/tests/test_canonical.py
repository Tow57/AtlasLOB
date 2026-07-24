from __future__ import annotations

from atlaslob.canonical import event_digest, state_digest
from atlaslob.domain import (
    ATLASLOB_SEMANTICS_VERSION,
    AcceptedEvent,
    BookChangedEvent,
    BookSnapshot,
    CanceledEvent,
    CommandType,
    DoneEvent,
    DoneReason,
    EventBatch,
    EventHeader,
    OrderSnapshot,
    PriceLevelSnapshot,
    RejectedEvent,
    RejectReason,
    ReplacedEvent,
    RestedEvent,
    Side,
    TopOfBookLevel,
    TradeEvent,
)


def _header(index: int) -> EventHeader:
    return EventHeader(command_sequence=41, event_index=index, instrument_id=7)


def test_state_digest_matches_independent_empty_and_representative_goldens() -> None:
    empty = BookSnapshot(
        semantics_version=ATLASLOB_SEMANTICS_VERSION,
        instrument_id=7,
        last_sequence=0,
        sequence_exhausted=False,
        active_order_count=0,
        bids=(),
        asks=(),
    )
    representative = BookSnapshot(
        semantics_version=ATLASLOB_SEMANTICS_VERSION,
        instrument_id=7,
        last_sequence=5,
        sequence_exhausted=False,
        active_order_count=3,
        bids=(
            PriceLevelSnapshot(
                price=101,
                aggregate_quantity=7,
                orders=(OrderSnapshot(2, 12, 7, Side.BUY, 101, 7, 2),),
            ),
            PriceLevelSnapshot(
                price=100,
                aggregate_quantity=16,
                orders=(
                    OrderSnapshot(1, 11, 7, Side.BUY, 100, 5, 1),
                    OrderSnapshot(3, 13, 7, Side.BUY, 100, 11, 3),
                ),
            ),
        ),
        asks=(),
    )

    assert state_digest(empty) == (
        "19a8ffaeb1bee1b8aa87123c3508af1bfa87e3d634a09ba491e1b85fe597b219"
    )
    assert state_digest(representative) == (
        "fe84a7515664b05af4f390ea77f040883c13b2ac5ce1867f8d69b8a37ccbd16f"
    )


def test_state_digest_encodes_negative_price_as_twos_complement() -> None:
    snapshot = BookSnapshot(
        semantics_version=ATLASLOB_SEMANTICS_VERSION,
        instrument_id=7,
        last_sequence=1,
        sequence_exhausted=False,
        active_order_count=1,
        bids=(
            PriceLevelSnapshot(
                price=-1,
                aggregate_quantity=2,
                orders=(OrderSnapshot(1, 1, 7, Side.BUY, -1, 2, 1),),
            ),
        ),
        asks=(),
    )

    assert state_digest(snapshot) == (
        "b5880e8068c991792de0b598f6afb3b57c7cd35ffce405275008c9e503d54ad6"
    )


def test_event_digest_matches_every_variant_golden() -> None:
    batch = EventBatch(
        (
            AcceptedEvent(_header(0), CommandType.REPLACE),
            RejectedEvent(_header(1), CommandType.CANCEL, RejectReason.UNKNOWN_ORDER_ID, 99),
            TradeEvent(_header(2), 101, 102, 11, 12, Side.SELL, 1001, 13, 14, 15),
            RestedEvent(_header(3), 103, 16, Side.BUY, 1002, 17),
            CanceledEvent(_header(4), 104, 18),
            ReplacedEvent(_header(5), 105, 106),
            DoneEvent(_header(6), 107, DoneReason.REPLACED, 19),
            BookChangedEvent(_header(7), TopOfBookLevel(1000, 20), None),
        )
    )

    assert event_digest(batch) == (
        "df99ffaa7ee15c5de7a8106be34c3e0b2d6be79d8284b3aa76d5d9f63540312e"
    )
