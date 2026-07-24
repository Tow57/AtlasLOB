# Semantic Contract v0.5

This document defines AtlasLOB's implemented command vocabulary, pure validation behavior, stable
order-node ownership, ordered resting book, direct cancellation, command admission, value-only
event batches, read-only matching plans, and atomic New/Cancel execution. Replace execution remains
the next layer.

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
- Command admission assigns one nonzero monotonically increasing sequence to every command that
  enters domain processing, including commands that receive pure or stateful domain rejections.
- A successfully resting new or replacement order uses that command sequence as its immutable
  priority sequence.
- Sequence zero means unassigned. The maximum 64-bit value is issued once; later allocation
  reports sticky internal exhaustion and never rolls over or becomes a client rejection.
- Order IDs are unique while active and may be reused only after the previous order is terminal.
- Future cancel/replace handling requires both client and instrument identity to match.
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
New and Cancel execution now publish these batches.

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
allocation. Preparation guards must not outlive their owning book.

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

All returnable execution failures occur before the first semantic write. Once batch mutation
starts, a component failure is an impossible internal contract violation and terminates rather
than returning a partially changed book. A test-only failpoint proves that an exception after GTC
residual preparation consumes the sequence but rolls storage, index, staging FIFO, and visible
top of book back exactly.
