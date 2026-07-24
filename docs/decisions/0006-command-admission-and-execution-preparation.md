# ADR 0006: Command admission and allocation-before-mutation execution

- Status: accepted
- Date: 2026-07-24

## Context

The resting book can now locate and remove active orders without scanning. Matching adds a
different failure boundary: one command may touch several passive orders, remove price levels,
publish a residual, and emit multiple normalized events. If a vector, hash entry, price level, or
order node allocates after the first fill, resource failure could leave a partially executed
command with no complete result.

Commands also need one authoritative sequence and deterministic state validation. Parse failures
remain adapter concerns, while pure and stateful domain rejections must enter the same sequenced
event stream as accepted commands.

## Decision

### Admission and sequencing

- A single-writer `CommandSequencer` issues `Sequence{1}` first and increases monotonically.
  `UINT64_MAX` is issued once. The next allocation reports a sticky internal
  `sequence_exhausted` condition; zero and rollover are never produced.
- Domain submission allocates the sequence before pure or state validation. Pure and stateful
  rejections therefore consume exactly one sequence. Parsing remains upstream and consumes none.
- Sequence exhaustion produces neither a domain rejection nor an event and never mutates the
  book. It is an internal availability condition, not `RejectReason::capacity_exceeded`.
- One deterministic `ExecutionPolicy` defines maximum order quantity, positive tick increment,
  and maximum active orders. Allocation failure is never translated to a policy-capacity reject.
- After pure validation, state-validation precedence is:
  - New: instrument route, configured quantity range, tick alignment, then active duplicate ID.
  - Cancel: instrument route, unknown ID, then client ownership.
  - Replace: instrument route, configured quantity range, tick alignment, unknown old ID, client
    ownership, then active new ID.
- A stored node whose instrument differs from its owning single-instrument book is corruption and
  produces an internal invariant error. `RejectReason::instrument_mismatch` remains reserved for
  later multi-instrument/global-index routing rather than disguising corruption as client input.
- Active-order capacity is evaluated only after planning. The final count subtracts fully filled
  passive orders and the old replacement order, then adds one only for a resting GTC residual.
  Market/IOC orders and fully filled GTC orders are therefore not rejected merely because the
  initial book is at its configured active-order limit.

### Value-only planning

- A read-only match planner walks the opposite side in best-price and FIFO order and records only
  IDs, clients, prices, quantities, and expected priority values. It never stores a book pointer
  or iterator.
- Limit-price comparison is inclusive. Execution price is the passive order's price. Market
  orders cross every available opposite level.
- The planner performs a counting pass, reserves exact trade capacity, then populates the plan.
  Allocation failure occurs before semantic mutation and propagates to the caller.
- The plan classifies the final aggressor quantity as filled, rest, IOC canceled, or market
  exhausted. FOK remains rejected before planning.
- Execution must re-find and verify every planned passive identity, priority, price, and remaining
  quantity before entering the mutation boundary.

### Event batches and top-of-book changes

- One `EventBatch` owns the complete `std::vector<Event>` result for a command and exposes it
  read-only. Events remain value-only and contain no timestamp or pointer.
- An exact-capacity builder allocates all event slots before the first book mutation. It stamps
  the authoritative command sequence, instrument, and contiguous zero-based index on every
  payload, overwriting any placeholder header supplied by the caller. Underfill or overflow is an
  internal builder error.
- A command result follows these orders:
  - Rejected: `Rejected`.
  - New: `Accepted`, zero or more `Trade`, then `Rested` or `Done`.
  - Cancel: `Accepted`, `Canceled`, `Done(canceled)`.
  - Replace: `Accepted`, `Replaced`, `Canceled(old)`, `Done(old, replaced)`, the new order's zero
    or more `Trade`, then `Rested` or `Done`.
- A final `BookChanged` is appended at most once and only when the final best-bid/best-ask
  price-and-aggregate tuple differs from the tuple before the command. Intermediate replacement
  states are not published.
- `Done.remaining_quantity` is the unexecuted quantity made terminal: zero for a fill, and the
  canceled residual for cancel, IOC residual, market exhaustion, or replacement of the old order.
  Passive fills do not emit a passive `Done`.

### Prepared passive residual

- A GTC residual is prepared before matching starts. Preparation allocates its order node, active
  index entry, storage entry, private staging level, and a detached fallback price level while the
  public active book remains valid and unchanged. At most one pending transaction exists per
  instrument book.
- The pending node is linked as a valid singleton in its private staging level and is hidden from
  public lookup and active counts. Whole-book invariants account for its exact storage/index
  membership separately from visible FIFO traversal.
- A prepared object is move-only RAII state. Abandoning it unlinks the staging node, removes its
  index entry, destroys its storage, and drops the detached level without changing the visible
  book. Like other preparation guards, it must not outlive its owning book.
- Commit occurs only after planned opposite-side fills (and the old-order removal for replace).
  It selects an existing target level if one still exists, otherwise installs the preallocated
  detached level, then publishes FIFO membership and makes the already-staged storage/index entry
  active without allocation.
- Preparation may observe a currently marketable price. Commit must verify that matching has made
  the residual nonmarketable; violation is an internal fatal contract breach.
- Preparing a fallback level even when the price currently exists handles replacement of the
  same-side singleton whose removal erases that level before residual commit.

## Consequences

Expected domain failures produce deterministic sequenced batches. Resource failure may create a
sequence gap, but cannot create a partially mutated semantic book. Planning and allocation add
extra passes and temporary value storage; this is an intentional correctness baseline that later
profiling may measure before any optimization.

The final apply step remains single-writer and intentionally fail-fast on an impossible component
failure after mutation begins. Internal corruption is never converted into a recoverable client
rejection.

## Evidence

The local implementation and tests prove:

- first/max/sticky-exhausted sequence behavior and one sequence per pure/state rejection;
- the complete state-validation precedence matrix and active-only ID reuse;
- owned batches with exact contiguous headers and every event alternative;
- top-of-book snapshots for empty, one-sided, two-sided, aggregate-only, and non-best changes;
- read-only full/partial, multi-order, multi-level, limit, IOC, and market planning;
- prepared residual abandonment and allocation-free commit at existing and new levels;
- no visible book mutation for every returned planning/preparation failure; and
- Debug and Release suites with 157/157 passing tests, the production-only build, and the pinned
  formatting gate.

An independent implementation review found no blocker or high-severity issue. Hosted GCC, Clang,
and sanitizer evidence remains required on the pull request and is not claimed by this record.
