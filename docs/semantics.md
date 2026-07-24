# Semantic Contract v0.6

This document defines AtlasLOB's implemented command vocabulary, pure validation behavior, stable
order-node ownership, ordered resting book, direct cancellation, command admission, value-only
event batches, read-only matching plans, atomic New/Cancel/Replace execution, and public
single-instrument engine boundary with canonical deterministic evidence.

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
- `CancelOrder` carries the client, order, and instrument IDs used for ownership checks.
- `ReplaceOrder` carries the client, old and new order IDs, instrument, new limit price, and new
  quantity.

Replace is implemented as an atomic cancel-and-new GTC limit operation. It retains the original
side, client, and instrument; requires a distinct inactive new ID; may trade immediately; and
receives a new priority sequence.

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
- Command admission assigns one nonzero monotonically increasing sequence to every command that
  enters domain processing, including commands that receive pure or stateful domain rejections.
- A successfully resting new or replacement order uses that command sequence as its immutable
  priority sequence.
- Sequence zero means unassigned. The maximum 64-bit value is issued once; later allocation
  reports sticky internal exhaustion and never rolls over or becomes a client rejection.
- Order IDs are unique while active and may be reused only after the previous order is terminal.
- Cancel and Replace require both client and instrument identity to match.
- A default `CommandExecutor` starts at sequence one and therefore requires an empty book. Resumed
  books require an explicit first sequence greater than every active priority; persisted replay
  state must additionally preserve the authoritative next sequence across terminal orders.
- Executor attachment is rejected while a prepared residual transaction is outstanding, because
  pending nodes are intentionally hidden from visible active counts and priority traversal.

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
- Replace: accepted, replaced, canceled-old, done-old with reason `replaced`, then the new-order
  trade/rest/done chain.

`Done.remaining_quantity` is the unexecuted quantity made terminal: zero for a fill, otherwise the
residual canceled by cancel, replacement, IOC completion, or market exhaustion. Passive fills do
not emit passive `Done` events.

`EventBatch` owns one command's immutable, nonempty event vector. Its events share one nonzero
sequence and one submitted instrument value and have contiguous zero-based indices. Instrument
zero is preserved only for the canonical one-event rejection of a structurally invalid command;
zero-instrument accepted or multi-event batches are invalid. The exact-slot builder allocates the
complete vector before mutation and overwrites caller-supplied headers with authoritative values.
New, Cancel, and Replace execution publish these batches.

## Stable order storage and FIFO price levels

`HeapOrderStorage` is the sole baseline owner of every `OrderNode`. It stores `std::unique_ptr`
values with a storage-only deleter in an order-ID hash table. Rehashing may reorganize the owning
pointers but does not move their pointees, so a node address remains stable until explicit storage
destruction.

All queue pointers are non-owning. A node's `previous`, `next`, and `PriceLevel` backlink are null
while it is not resting. A `PriceLevel` owns no nodes; it records only its price, aggregate
remaining quantity, order count, and non-owning head/tail pointers. Nodes, levels, and storage
implementations are non-copyable and non-movable.

Storage creation and destruction are checked:

- Duplicate creation fails without changing storage.
- A fresh node starts with null queue links and no level backlink.
- Destruction requires the exact owned node and requires that node to be unlinked. Borrowers
  cannot invoke `OrderNode` destruction or construct its owning deleter.
- Allocation failure propagates as a resource failure rather than becoming a client rejection.

Price-level mutation is checked before changing quantities or links:

- Append requires a positive remaining quantity, matching price, an unlinked node, and a priority
  sequence strictly greater than the current tail's sequence.
- Aggregate overflow and every structural precondition fail without mutation.
- Erase directly handles a known singleton, head, middle, or tail member, updates aggregate and
  count, and clears every queue link on the removed node.
- Partial reduction requires `0 < reduction < remaining`. Full removal uses `erase`.

Explicit invariant validation uses cycle-safe traversal and verifies empty-state boundaries,
head/tail termination, reciprocal links, level backlinks, price equality, positive quantities,
strictly increasing priority, order count, and recomputed aggregate quantity.
Storage and level teardown enforce the unlinked/empty lifetime boundary in every build; the
configurable full traversal checks are additional mutation-time diagnostics.
Deliberate-corruption access is compiled only into a separate test core; the production
`atlas_core` class definitions contain no test friendship.

Ordered bid and ask sides own stable `PriceLevel` objects in best-price-first maps. A checked
non-owning active-order index provides direct ID lookup. `InstrumentBook` keeps storage, index, and
FIFO traversal in exact one-to-one correspondence and validates identity, linkage, cardinality,
uncrossed best prices, and component-local invariants.

The terminal-removal order is:

1. Unlink the node from its price level.
2. Remove its pointer from the global active-order index.
3. Remove the price level if it is empty.
4. Destroy the node through owning storage.

No step may destroy an object while a later step can still dereference it. Executable cancellation
uses this path and removes empty levels immediately.

A prepared GTC residual allocates its node, storage/index entries, private staging level, and a
detached fallback map node before matching mutation. The one pending node is hidden from public
active lookup/counts but remains fully covered by whole-book invariants. Abandonment restores the
exact visible state. Commit chooses an existing level or the detached fallback and performs no
allocation. A replacement preparation stores the old order's stable ID rather than its address and
pins that exact active order against direct reduction, removal, or cancellation until transaction
commit or rollback. Unrelated passive orders remain mutable. Preparation guards must not outlive
their owning book.

## Error boundaries

- Parse errors describe malformed adapter input and are not domain rejections.
- Domain rejections describe expected invalid command values or state.
- Storage/price-level errors describe internal component precondition or arithmetic failures and
  are never converted into public `RejectReason` values.
- A projected final active-order limit or projected resting-level aggregate overflow produces
  `capacity_exceeded` before mutation. Allocation failure itself is never translated to that
  rejection.
- Allocation failure is a propagated resource failure, not ordinary command rejection.
- Invariant failures indicate implementation bugs and are not converted into client rejections.

## Matching planning and execution boundary

- Best price, then earliest priority sequence at that price.
- Execution at the resting order's price.
- Limit GTC residuals rest at the tail of their price level.
- IOC and market residuals terminate without resting.
- Partial execution does not reset a resting order's priority.
- Replace uses atomic cancel-and-new semantics and resets priority.

The read-only planner implements these traversal and residual rules for supported new orders. It
performs a counting pass before allocating a value-only trade plan, records expected passive
identity/price/quantity/priority, and leaves the book unchanged.

New execution then:

1. Assigns a sequence and emits a singleton rejection for expected pure/state failure.
2. Plans, checks the final active count, and re-walks the opposite book in exact price/FIFO order
   to bind every planned value to its current node.
3. Projects the final top of book and prepares any GTC residual.
4. Allocates and finishes the complete event batch.
5. Revalidates the complete reduction set, applies partial/full passive reductions, and publishes
   the prepared residual through a no-allocation mutation boundary.
6. Verifies the actual final top against the projection.

The New event order is accepted, zero or more trades, rested or done, then at most one final
book-changed event. IOC and market residuals produce done events and never rested or canceled
events. Passive fills are represented by each trade's zero resting remainder and do not produce
passive done events.

Cancel execution re-finds the owned active node, projects its full removal, prebuilds accepted,
canceled, and done events plus an optional final book-changed event, then applies one exact
prevalidated terminal reduction. The canceled and done quantities both equal the order's current
remaining quantity.

Replace execution:

1. Assigns one sequence and validates the exact old owner/instrument plus a distinct inactive new
   ID.
2. Synthesizes a limit GTC order retaining the old client, instrument, and side, then plans and
   binds its passive trades.
3. Projects final capacity after removing the old order and terminal passives, and projects final
   top of book after the complete replacement.
4. Prepares an optional hidden residual with the command sequence as new priority. Same-level
   checks subtract the old quantity before checked addition and place the new node at the FIFO
   tail.
5. Prebuilds accepted, replaced, canceled-old, done-old, trade, new rested/done, and optional
   book-changed events.
6. Atomically removes the old order, applies passive reductions, and publishes the residual
   through a replacement-specific no-allocation boundary.

The old order remains visible until the replacement commits. A replacement preparation cannot be
published through the ordinary New path, and abandonment removes only hidden staging state. A
fully filled replacement leaves both old and new IDs terminal.

All returnable execution failures occur before the first semantic write. Once batch mutation
starts, a component failure is an impossible internal contract violation and terminates rather
than returning a partially changed book. A test-only failpoint proves that an exception after GTC
residual preparation consumes the sequence but rolls storage, index, staging FIFO, and visible
top of book back exactly.

## Public matching engine

`atlaslob::MatchingEngine` is the supported single-instrument, single-writer facade. It owns the
private book and executor behind a PImpl and accepts typed New, Cancel, and Replace values or the
`Command` variant. Its move-only result has private mutually exclusive state created through
validated success/failure factories: either one complete normalized `EventBatch` or a concrete
internal `EngineError`. A normal domain rejection is a valid one-event batch, not an engine
failure. A moved-from result is an explicit inert state and cannot appear successful.

Read-only observers expose the configured instrument, active order count, emptiness, current best
bid/ask price and aggregate, next sequence, and sticky sequence exhaustion. Node addresses,
levels, indexes, planners, prepared transactions, and detailed internal component errors are not
public API. Allocation failure propagates. Multi-instrument routing and durable replay remain
future infrastructure.

## Canonical snapshots and digests

`MatchingEngine::snapshot()` returns the complete visible resting state as values. It records
semantic version, instrument, last issued sequence, exhaustion, active count, bids in descending
price order, asks in ascending price order, and each level's orders in FIFO order. Every order
includes identity, side, price, remaining quantity, and priority sequence. A fresh engine reports
last sequence zero. Snapshot observation requires a valid book with no pending preparation.

State and event digests hash explicitly encoded bytes rather than native C++ object
representations. The state domain begins with ASCII `ATLSST01`; the event domain begins with ASCII
`ATLSEV01`. Integers are fixed-width and big-endian, signed price ticks use their two's-complement
64-bit representation, collection counts precede elements, and optional values encode an explicit
presence byte plus fixed-width payload or zero placeholders. SHA-256 results render as 64
lowercase hexadecimal characters.

The state encoding includes every snapshot field, level aggregate, and FIFO order field. The event
encoding includes semantic version, batch identity, ordered variant tags, every header, every
payload, and optional ID/top-of-book presence. The complete byte layout is frozen in ADR 0009.
These values support deterministic comparison and replay evidence; they are not authentication,
a persistence format, or a performance protocol.

## Deterministic engine fixture

`atlas_cli engine-fixture <instrument_id> <path>` executes the canonical text command grammar
against one matching engine. Each submitted command prints its source line, authoritative
sequence, committed/rejected classification, ordered event types, event digest, and resulting
state digest. The summary reports counts, last sequence, and final state digest.

Malformed syntax and conversion failures remain adapter errors and consume no sequence. Domain
and state rejections consume a sequence and produce a digestible rejection batch. Exit code `0`
means all commands committed, `1` means at least one domain rejection, `2` means an input/parse
error occurred, and `3` means an engine failure occurred; higher-severity categories take
precedence.

A test-only reference engine uses ordered maps, FIFO deques, linear identity searches, and
separately implemented validation, matching, mutation, and event construction. Fixed-seed
command streams compare exact batches, snapshots, observers, and digests after every command.
This is Phase 2 regression evidence, not a substitute for the separately packaged Python oracle,
shrinking, long campaigns, and fuzzing planned for Phase 3.
