# Semantic Contract v0.2

This document defines AtlasLOB's implemented command vocabulary and pure validation behavior. It
also freezes sequencing, identity, event, and future matching rules before mutable order-book state
is introduced.

## Values and enumerations

`ClientId`, `OrderId`, `InstrumentId`, `Sequence`, `PriceTicks`, and `Quantity` are distinct C++
types. Zero is invalid for client-supplied IDs, prices, and quantities; zero is unassigned for the
engine-owned sequence. Prices use integer ticks; floating-point prices are excluded from the core
API.

Domain enums have explicit nonzero values. Zero and unknown underlying values are invalid rather
than accidental defaults. `RejectReason` uses a 16-bit representation; the Phase 0 rejection
codes retain their original numeric values and new reasons are appended.

## Commands

- `NewOrder` carries client, order, and instrument IDs, side, order type, time in force, optional
  limit price, and quantity.
- `CancelOrder` carries the client, order, and instrument IDs required for future ownership checks.
- `ReplaceOrder` carries the client, old and new order IDs, instrument, new limit price, and new
  quantity.

Replace is reserved as an atomic cancel-and-new GTC limit operation. It retains the original side,
client, and instrument; requires a distinct inactive new ID; may trade immediately; and receives a
new priority sequence.

## Supported new-order combinations

| Order type | Time in force | Pure validation behavior |
| --- | --- | --- |
| Limit | GTC | Valid with a positive limit price and quantity. |
| Limit | IOC | Valid with a positive limit price and quantity. |
| Market | IOC | Valid without a limit price. Residual quantity will never rest. |
| Limit or market | FOK | Recognized but rejected as unsupported. |
| Market | GTC | Rejected as an invalid combination. |

## Deterministic pure-validation order

When several fields are invalid, the first applicable rule determines the rejection:

1. New: client ID, order ID, instrument ID, quantity, side, order type, time in force, unsupported
   time in force, price requirements, then type/time-in-force compatibility.
2. Cancel: client ID, order ID, then instrument ID.
3. Replace: client ID, old order ID, new order ID, distinct replacement ID, instrument ID, new
   quantity, then new price.

Pure validation checks command shape only. Unknown instruments, duplicate/unknown active IDs,
ownership mismatch, instrument mismatch, tick alignment, configured ranges, and capacity are
stateful decisions represented in the rejection vocabulary but not produced by these functions.

## Sequencing and identity

- Representation and numeric-conversion failures occur outside the domain and consume no engine
  sequence.
- A future engine assigns one nonzero monotonically increasing sequence to every well-formed
  command that enters domain processing, including commands that receive domain rejections.
- A successfully resting new or replacement order uses that command sequence as its immutable
  priority sequence.
- Sequence zero means unassigned. Wraparound is an internal fatal/capacity condition.
- Order IDs are unique while active and may be reused only after the previous order is terminal.
- Future cancel/replace handling requires both client and instrument identity to match.

## Normalized events

Every event carries a command sequence, contiguous zero-based event index, and instrument ID. The
event variant alternative is the single event-type discriminator, so a header cannot contradict
its payload. The event vocabulary is accepted, rejected, trade, rested, canceled, replaced, done,
and book-changed. Each present best bid or ask couples its price with its aggregate quantity; an
absent side has neither value.

Event payloads use value types only. They never expose pointers, container positions, allocator
handles, machine addresses, wall-clock timestamps, or unordered iteration order.

Reserved event order:

- Valid new: accepted, zero or more trades, then rested or done.
- Rejected command: one rejected event.
- Cancel: accepted, canceled, done.
- Replace: accepted, replaced, canceled-old, then the new-order trade/rest/done chain.

Event generation begins with the matching-engine phase. This release freezes the schema and
ordering contract without claiming execution support.

## Error boundaries

- Parse errors describe malformed adapter input and are not domain rejections.
- Domain rejections describe expected invalid command values or state.
- Storage/price-level errors describe internal component precondition or capacity failures.
- Invariant failures indicate implementation bugs and are not converted into client rejections.

## Matching rules reserved for later phases

- Best price, then earliest priority sequence at that price.
- Execution at the resting order's price.
- Limit GTC residuals rest at the tail of their price level.
- IOC and market residuals terminate without resting.
- Partial execution does not reset a resting order's priority.
- Replace uses atomic cancel-and-new semantics and resets priority.
