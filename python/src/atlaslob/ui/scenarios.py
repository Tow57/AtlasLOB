"""Small frozen demonstration programs, independent of the web framework."""

import hashlib
import json
from dataclasses import asdict, dataclass
from typing import Any, cast

from atlaslob.domain import CancelOrder, Command, NewOrder, ReplaceOrder


def new(
    oid: int,
    side: int,
    price: int | None,
    quantity: int,
    *,
    instrument: int = 7,
    client: int = 1,
    tif: int = 1,
) -> NewOrder:
    return NewOrder(
        client,
        oid,
        instrument,
        side,
        2 if price is None else 1,
        2 if price is None else tif,
        price,
        quantity,
    )


def encode(command: Command) -> dict[str, Any]:
    return {
        "kind": type(command).__name__,
        **{k: None if v is None else str(v) for k, v in asdict(command).items()},
    }


def decode(value: dict[str, Any]) -> Command:
    classes = {"NewOrder": NewOrder, "CancelOrder": CancelOrder, "ReplaceOrder": ReplaceOrder}
    cls = classes[value["kind"]]
    return cast(
        Command, cls(**{k: None if v is None else int(v) for k, v in value.items() if k != "kind"})
    )


@dataclass(frozen=True)
class Scenario:
    id: str
    name: str
    description: str
    steps: tuple[tuple[str, Command], ...]

    def freeze(self) -> dict[str, Any]:
        payload = {
            "id": self.id,
            "version": 1,
            "name": self.name,
            "description": self.description,
            "steps": [{"description": d, "command": encode(c)} for d, c in self.steps],
        }
        return {"payload": payload, "sha256": digest(payload)}


def digest(payload: dict[str, Any]) -> str:
    return hashlib.sha256(
        json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def market_walk() -> Scenario:
    """Build a repeatable 600-command walk without consulting an engine or clock.

    Each 30-command chapter touches one instrument. Its two small reference
    orders remain throughout the run; all other liquidity is explicitly built,
    traded, replaced or canceled in the visible timeline. The prior chapter's
    two working quotes are canceled before rebuilding the next price region.
    IDs are allocated from command positions, including gaps for cancellations.
    """
    steps: list[tuple[str, Command]] = []
    movement = (0, 2, 4, 6, 8, 6, 4, 2, 0, -2)
    previous_quotes: dict[int, tuple[int, int]] = {}
    for chapter in range(20):
        instrument = 7 if chapter % 2 == 0 else 42
        label = "DEMO-A" if instrument == 7 else "DEMO-B"
        center = 100 if instrument == 7 else 200
        mid = center + movement[chapter // 2]
        base = chapter * 30

        def add(
            description: str, command: Command, prefix: str = f"{label}, chapter {chapter // 2 + 1}"
        ) -> None:
            steps.append((f"{prefix}: {description}", command))

        def quote(
            offset: int,
            side: int,
            price: int | None,
            quantity: int,
            *,
            client: int = 1,
            tif: int = 1,
            base: int = base,
            instrument: int = instrument,
        ) -> NewOrder:
            return new(
                base + offset,
                side,
                price,
                quantity,
                instrument=instrument,
                client=client,
                tif=tif,
            )

        if instrument in previous_quotes:
            bid_id, ask_id = previous_quotes[instrument]
            add(
                "Cancel the previous chapter's remaining working bid.",
                CancelOrder(1, bid_id, instrument),
            )
            add(
                "Cancel the previous chapter's remaining working ask.",
                CancelOrder(2, ask_id, instrument),
            )
        else:
            add(
                "Rest a one-unit reference bid below the walk's price range.",
                quote(1, 1, center - 20, 1),
            )
            add(
                "Rest a one-unit reference ask above the walk's price range.",
                quote(2, 2, center + 20, 1, client=2),
            )

        add(f"Rest 10 units at the inner bid, {mid - 1} ticks.", quote(3, 1, mid - 1, 10))
        add(f"Rest 10 units at a second bid, {mid - 2} ticks.", quote(4, 1, mid - 2, 10))
        add(f"Rest 10 units at the inner ask, {mid + 1} ticks.", quote(5, 2, mid + 1, 10, client=2))
        add(f"Rest 10 units at a second ask, {mid + 2} ticks.", quote(6, 2, mid + 2, 10, client=2))
        add(
            "Market buy 15: sweep two ask levels, leaving 5 at the second.",
            quote(7, 1, None, 15, client=3),
        )
        add(
            "Market sell 15: sweep two bid levels, leaving 5 at the second.",
            quote(8, 2, None, 15, client=3),
        )
        add(
            "Replace the remaining ask with 8 units at the same price.",
            ReplaceOrder(2, base + 6, base + 9, instrument, mid + 2, 8),
        )
        add(
            "Client 1 joins that ask price with 6 units, behind the replacement.",
            quote(10, 2, mid + 2, 6),
        )
        add(
            "Replace client 2's ask again: its priority now moves behind client 1.",
            ReplaceOrder(2, base + 9, base + 11, instrument, mid + 2, 8),
        )
        add(
            "Market buy 7: fill client 1's 6 first, then 1 from the replaced ask.",
            quote(12, 1, None, 7, client=3),
        )
        add(
            "Sell 8 with limit IOC: fill the remaining bid's 5 and cancel residual 3.",
            quote(13, 2, mid - 2, 8, client=3, tif=2),
        )
        add(
            "Buy 9 with limit IOC: fill the remaining ask's 7 and cancel residual 2.",
            quote(14, 1, mid + 2, 9, client=3, tif=2),
        )
        add("Replenish a 14-unit bid at the chapter's center price.", quote(15, 1, mid, 14))
        add(
            "Replenish a 14-unit ask two ticks above the center.",
            quote(16, 2, mid + 2, 14, client=2),
        )
        add(
            "Replace the bid with 5 units at the ask: replacement trades immediately.",
            ReplaceOrder(1, base + 15, base + 17, instrument, mid + 2, 5),
        )
        add(
            "Cancel the remaining 9 units of the replenished ask.",
            CancelOrder(2, base + 16, instrument),
        )
        add("Replenish 12 units at the inner bid.", quote(19, 1, mid - 1, 12))
        add("Replenish 12 units at the inner ask.", quote(20, 2, mid + 1, 12, client=2))
        add(
            "Market buy 4: record an upward execution and leave 8 ask units.",
            quote(21, 1, None, 4, client=3),
        )
        add(
            "Market sell 4: record a downward execution and leave 8 bid units.",
            quote(22, 2, None, 4, client=3),
        )
        add(
            "Replace the bid one tick lower with 11 units.",
            ReplaceOrder(1, base + 19, base + 23, instrument, mid - 2, 11),
        )
        add(
            "Replace the ask one tick higher with 11 units.",
            ReplaceOrder(2, base + 20, base + 24, instrument, mid + 2, 11),
        )
        add(
            "Buy exactly 11 at the new ask price, consuming that quote.",
            quote(25, 1, mid + 2, 11, client=3),
        )
        add(
            "Sell exactly 11 at the new bid price, consuming that quote.",
            quote(26, 2, mid - 2, 11, client=3),
        )
        add("Leave a fresh 20-unit working bid for inspection.", quote(27, 1, mid - 1, 20))
        add(
            "Leave a fresh 20-unit working ask for inspection.", quote(28, 2, mid + 1, 20, client=2)
        )
        add(
            "Market sell 4, retaining a partially filled 16-unit working bid.",
            quote(29, 2, None, 4, client=3),
        )
        add(
            "Market buy 4, retaining a partially filled 16-unit working ask.",
            quote(30, 1, None, 4, client=3),
        )
        previous_quotes[instrument] = (base + 27, base + 28)

    return Scenario(
        "market-walk",
        "Market Walk",
        "A 600-command journey through two instruments: rising and falling prices, "
        "multi-level sweeps, FIFO priority, IOC residuals, and replenished liquidity.",
        tuple(steps),
    )


SCENARIOS: tuple[Scenario, ...] = (
    Scenario(
        "fifo",
        "Price-time priority",
        "Watch the oldest order at a price fill first.",
        (
            ("Client 1 rests the first ask at 101.", new(1, 2, 101, 6)),
            ("Client 2 joins the same price behind client 1.", new(2, 2, 101, 8, client=2)),
            ("Add a bid at 99 to establish a spread.", new(3, 1, 99, 10)),
            ("Buy 9: fill order 1 first, then 3 units of order 2.", new(4, 1, 101, 9, client=3)),
            ("Buy the remaining 5 units with a market IOC.", new(5, 1, None, 5, client=3)),
        ),
    ),
    Scenario(
        "sweep",
        "Multi-level sweep",
        "Follow an incoming order through several price levels.",
        (
            ("Rest 4 units at ask 101.", new(1, 2, 101, 4)),
            ("Rest 6 units at ask 102.", new(2, 2, 102, 6, client=2)),
            ("Rest 10 units at ask 103.", new(3, 2, 103, 10)),
            (
                "Buy 14 up to 103: two full levels, then a partial fill.",
                new(4, 1, 103, 14, client=3),
            ),
            ("Buy 9 up to 103: fill 6 and rest a residual bid of 3.", new(5, 1, 103, 9, client=3)),
        ),
    ),
    Scenario(
        "replace",
        "Cancel and replace",
        "See cancellation and the priority cost of replacement.",
        (
            ("Client 1 places the oldest bid at 100.", new(1, 1, 100, 6)),
            ("Client 2 joins the bid queue.", new(2, 1, 100, 8, client=2)),
            ("Place an ask above the market.", new(3, 2, 103, 5)),
            (
                "Replace order 1 with order 4 at the same price: priority resets.",
                ReplaceOrder(1, 1, 4, 7, 100, 6),
            ),
            ("Sell 9: order 2 now fills before replacement order 4.", new(5, 2, 100, 9, client=3)),
            ("Cancel the remaining quantity of order 4.", CancelOrder(1, 4, 7)),
        ),
    ),
    Scenario(
        "ioc",
        "IOC and rejections",
        "Explore immediate-or-cancel residuals and rejected commands.",
        (
            ("Rest 3 units at ask 101.", new(1, 2, 101, 3)),
            ("Buy 8 with limit IOC: fill 3 and cancel the residual 5.", new(2, 1, 101, 8, tif=2)),
            ("A market buy encounters an empty ask book and never rests.", new(3, 1, None, 4)),
            (
                "Submit zero quantity deliberately: rejection still consumes a sequence.",
                new(4, 1, 100, 0),
            ),
            ("A valid order can follow a normal domain rejection.", new(5, 1, 100, 6)),
        ),
    ),
    Scenario(
        "instruments",
        "Two instruments",
        "Independent books share a single command timeline.",
        (
            ("DEMO-A receives a bid at 100.", new(1, 1, 100, 8)),
            ("DEMO-B receives an ask at 201.", new(2, 2, 201, 5, instrument=42)),
            ("DEMO-A receives an ask at 102.", new(3, 2, 102, 4)),
            (
                "A DEMO-B market buy changes only DEMO-B.",
                new(4, 1, None, 3, instrument=42, client=2),
            ),
            ("A DEMO-A market sell changes only DEMO-A.", new(5, 2, None, 2, client=3)),
        ),
    ),
)


SCENARIOS += (market_walk(),)


def first_demo() -> tuple[Command, ...]:
    result: list[Command] = []
    for instrument, mid in ((7, 100), (42, 200)):
        for distance in range(1, 7):
            for side in (1, 2):
                price = mid - distance if side == 1 else mid + distance
                result.append(
                    new(
                        len(result) + 1,
                        side,
                        price,
                        8 + distance * 3,
                        instrument=instrument,
                        client=(distance % 3) + 1,
                    )
                )
    return tuple(result)
