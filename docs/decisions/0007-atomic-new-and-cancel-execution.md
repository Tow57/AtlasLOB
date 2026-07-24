# ADR 0007: Atomic New and Cancel execution

- Status: accepted
- Date: 2026-07-24

## Context

ADR 0006 established value-only planning, exact event allocation, and prepared residual storage,
but stopped before applying a command. A New order may reduce several passive nodes and levels,
publish a residual, and emit a multi-event result. Cancel must remove one exact active node and
publish its current residual. Returning an error after any of those writes would expose a partial
command.

The executor must also preserve the submitted instrument on malformed rejections, calculate
capacity from the final state, and prevent a restored book's priority values from diverging from
the authoritative sequencer.

## Decision

### Executor and sequencing ownership

- A private single-writer `CommandExecutor` owns `CommandAdmission` and mutates one
  `InstrumentBook`.
- Executor construction rejects an outstanding prepared-book transaction; hidden pending priority
  may not bypass sequencer attachment checks.
- The default sequence-one constructor requires an empty valid book.
- A nonempty restored book requires an explicit first sequence strictly greater than every active
  priority. Durable replay must eventually persist the authoritative next sequence because
  terminal commands can be newer than all active nodes.
- Domain rejection is a successful execution result containing exactly one `RejectedEvent`.
  Internal failure has no batch. Allocation failure propagates and may leave a consumed sequence,
  but never a visible semantic mutation.

### Rejection and event envelope

- Every batch uses the submitted instrument value. Instrument zero is legal only for an exactly
  one-event rejected batch; accepted and multi-event zero-instrument batches are invalid.
- Active-order capacity is checked against the complete match plan. Fully terminal passive orders
  are subtracted and only a GTC residual adds a new active order.
- Projected resting-level aggregate overflow is rejected as `capacity_exceeded` before mutation.
  Allocation failure is never converted to that reason.
- New event order is `Accepted`, zero or more `Trade`, one `Rested` or `Done`, then at most one
  final `BookChanged`.
- Cancel event order is `Accepted`, `Canceled`, `Done(canceled)`, then at most one final
  `BookChanged`.
- A final `BookChanged` appears only when the complete best bid/ask price-and-aggregate tuple
  differs. It is never emitted for an intermediate state.

### Exact binding and projection

- After planning, execution re-walks the current opposite side in natural best-price/FIFO order.
  Each planned trade must match that exact node's ID, client, price, priority, starting quantity,
  execution quantity, passive remainder, and aggressor remainder chain.
- Binding owns only a temporary vector of non-owning node pointers plus copied expected values.
  Allocation completes before residual preparation and mutation.
- Allocation-free projection derives the final top of book for all planned passive reductions, a
  possible same-side residual, or one cancellation. Checked underflow, overflow, stale values, or
  a crossed final projection fails before mutation.

### Mutation boundary

- `InstrumentBook::apply_prevalidated_batch` revalidates the whole book, every exact pointer/value
  binding, uniqueness, arithmetic, residual ownership, appendability, and final uncrossing before
  its first write.
- The mutation loop performs partial reductions or terminal removal in the established order:
  level unlink, index removal, empty-level cleanup, then storage destruction.
- A prepared residual is published without allocation after opposite passive reductions.
- Returnable errors are preflight errors and leave the book unchanged. Once mutation starts,
  impossible component or postcondition failures terminate rather than returning partial state.
- Exactly one whole-book postcondition check closes the batch boundary.

## Consequences

The baseline makes extra read-only passes and allocates a value plan, pointer bindings, an event
vector, and possibly a prepared residual before committing. This is a deliberate correctness
baseline; no latency or throughput claim follows from it.

IOC and market residuals produce `Done` and never rest. Passive terminal state is visible through
`Trade.resting_remaining == 0`; no passive `Done` is emitted. Self-trade is not specially
prohibited in this educational MVP.

Replace remains outside this record. Its old same-side removal requires a specialized preparation
preflight when the new residual joins the same level and the old quantity frees aggregate
capacity.

## Evidence

The local implementation and tests prove:

- end-to-end New tests cover empty/better/equal/nonbest rests, partial/full fills, FIFO and
  multi-level sweeps, inclusive limits, both sides, GTC residuals, IOC, and market outcomes;
- capacity tests cover initial-capacity terminal commands, final residual counts, and aggregate
  overflow without mutation;
- Cancel tests cover current residuals, best aggregate changes, nonbest suppression, terminal
  cleanup, ownership rejection, and ID reuse;
- all event headers, ordering, payloads, and final top-of-book values are verified;
- deterministic exception injection after residual preparation proves exact rollback with a
  consumed sequence;
- an independent review found no blocker/high issue;
- Debug and Release suites pass 204/204 tests;
- the production-only build and pinned formatting gate pass.

The local Windows environment does not provide `clang++`, so local Clang and ASan/UBSan presets
cannot configure. Hosted GCC, Clang, and sanitizer evidence remains required on the pull request
and is not claimed by this record.
