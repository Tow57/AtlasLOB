# ADR 0008: Atomic Replace and the public matching engine

- Status: accepted
- Date: 2026-07-24

## Context

ADR 0007 executes New and Cancel through an all-preflight mutation boundary. Replace is more
demanding: its old order is active while the new order is planned, the replacement may immediately
trade, and a residual may return to the old price after the old quantity and priority are removed.
A naïve cancel followed by New would expose an intermediate book, consume two sequences, permit a
failure after cancellation, and produce the wrong event envelope.

The core also needs one supported public entry point. Mutable nodes, levels, indexes, plans,
preparations, and internal error details must remain implementation concerns rather than becoming
part of the library contract.

## Decision

### Replace semantics

- One admitted `ReplaceOrder` consumes one command sequence.
- The old active order must match the submitted client and instrument. The new ID must be distinct
  and inactive.
- The replacement retains the old client, instrument, and side. It is synthesized as a limit GTC
  order with the submitted price and quantity.
- The replacement receives the command sequence as new priority. Same-price replacement therefore
  moves to the FIFO tail; it never preserves the old position.
- Matching uses the ordinary opposite-side best-price/FIFO planner and executes at passive prices.
- The final active-order capacity projection removes the old order and every fully filled passive,
  then adds only a resting replacement residual.
- Self-trade remains allowed in this educational MVP.

The normalized event order is:

1. `Accepted(replace)`
2. `Replaced(old_id, new_id)`
3. `Canceled(old_id, old_remaining)`
4. `Done(old_id, replaced, old_remaining)`
5. zero or more replacement `Trade` events
6. one replacement `Rested` or `Done(filled)` event
7. at most one coalesced final `BookChanged`

There is no observable canceled-and-empty intermediate state.

### Projection and preparation

- Replace top-of-book projection removes the exact copied old target, applies the immutable
  opposite-side plan, and adds an optional replacement residual without mutating the book.
- Same-level checked arithmetic subtracts the old quantity before adding the new residual. This
  permits a valid net-neutral replacement when the visible level aggregate is already at its
  representational maximum.
- `prepare_replace_rest` allocates and indexes the hidden replacement node and its detached
  fallback level before event allocation. It records the old order's stable ID, and the book pins
  that exact active identity against direct reduction, removal, or cancellation until commit or
  rollback.
- A replacement preparation cannot be committed directly and cannot enter the ordinary New batch
  API. Abandonment removes only the hidden replacement and leaves the old order and passives
  exactly unchanged.

### Atomic mutation

- `apply_prevalidated_replace_batch` accepts one mandatory full old-order reduction, zero or more
  exact opposite-side passive reductions, and an optional replacement preparation.
- It validates the whole book, every pointer/value binding, uniqueness, old/new identity
  relationship, same-side aggregate and priority relief, residual appendability, and final
  uncrossing before its first write.
- The no-allocation mutation boundary removes the old order, applies passive reductions, and then
  publishes the optional residual.
- Every terminal node follows the established invalidation order: level unlink, index removal,
  empty-level removal, then storage destruction.
- A deterministic exception after replacement preparation and before event allocation consumes
  the command sequence but rolls the hidden node and all staging state back exactly.

### Public API

- `atlaslob::MatchingEngine` is the supported single-instrument, single-writer facade.
- A private PImpl owns one `InstrumentBook` followed by its `CommandExecutor`; callers cannot
  observe internal pointers or mutable containers.
- Typed New, Cancel, and Replace overloads plus a `Command` variant overload return an
  `EngineResult`.
- A sequenced domain rejection is a successful result containing one `RejectedEvent`. Sequence
  exhaustion and other internal failures return no event batch and are distinguished by
  `EngineError`. Allocation failure continues to propagate.
- `EngineResult` stores a private mutually exclusive batch/error state. Named factories reject
  invalid error vocabulary, observers are read-only, and move operations leave an explicit inert
  source rather than an apparently successful result.
- Read-only observers expose instrument ID, active count, emptiness, final top of book, next
  sequence, and sticky sequence exhaustion.
- The facade is non-copyable and non-movable. Multi-instrument routing and durable restored
  sequencing remain later infrastructure.

## Consequences

Replace performs extra read-only validation and allocates a plan, bindings, event storage, and
possibly a hidden residual before commit. This is intentional evidence-first design, not a
performance claim.

The public API freezes value-level behavior without freezing internal book representation.
Callers receive normalized events and top-of-book values, while implementation-only failure
vocabulary remains private.

## Evidence

Local tests cover:

- same-price priority reset and unchanged top of book;
- better/worse price replacement, opposite-side multi-level execution, residual rest, and full
  fill;
- exact old cancellation and replacement event payloads;
- unknown/foreign old IDs, active new IDs, distinct-ID validation, sequence consumption, and
  terminal ID reuse;
- capacity-neutral replacement and same-level aggregate relief at the maximum 64-bit quantity;
- malformed/stale projection inputs and both buy/sell top transitions;
- guarded preparation misuse, foreign bindings, singleton-level recreation, rollback, and
  allocation-free commit;
- deterministic post-preparation allocation failure with exact state restoration;
- public typed and variant dispatch, policy enforcement, rejection/commit classification, and
  observers.

The final Debug and Release suites pass 235/235 tests; the production-only build, pinned
formatting gate, and repository hygiene checks pass. Two independent reviews found no blocker or
high-severity issue. Their one medium finding—a moved-from public result that could still appear
engaged—was fixed with source-invalidating move operations and direct regression coverage. A later
hardening review removed public construction of contradictory batch/error states and eliminated a
raw replacement-old pointer from prepared transactions. The published Phase 2 head passed hosted
GCC, Clang, ASan/UBSan, and pinned formatting; follow-up commits must repeat those gates.
