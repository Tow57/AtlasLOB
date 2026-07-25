# ADR 0012: Multi-instrument routing and global sequencing

- Status: accepted
- Date: 2026-07-24

## Context

The public engine established by ADRs 0006 through 0009 owns one instrument book, one active-order
index, and one command sequencer. That is sufficient for deterministic matching inside one book,
but it cannot enforce active-order identity across instruments or assign one authoritative order to
interleaved commands for several books.

Phase 4 also introduces a write-ahead command log. Persistence must be able to prepare a complete
semantic result, append it, and only then publish the corresponding book, identity, and sequence
changes. The Phase 2 allowance that a resource failure may leave a sequence gap is therefore no
longer suitable: a failed preparation must not create a sequence that is absent from the
authoritative log.

The new facade must not invalidate the existing single-instrument API or the deterministic evidence
used by the independent Python oracle and differential campaigns. In particular, semantic version
6 and the `ATLSST01`, `ATLSEV01`, and `ATLAS_DIFF_V1` contracts are already frozen.

## Decision

### Catalog and public ownership boundary

- `MultiInstrumentEngine` owns one immutable catalog, one eagerly constructed book per catalog
  entry, one global command sequencer, and one global active-order directory.
- Construction requires a nonempty catalog of nonzero, unique instrument IDs. Catalog entries and
  books are stored in ascending instrument-ID order, independent of caller order.
- Each catalog entry retains its own `MatchingEngineConfig`. Its `max_active_orders` remains a
  per-instrument limit. `MultiInstrumentEngineConfig::max_total_active_orders` is a separate
  engine-wide limit.
- Invalid catalog or policy configuration is rejected by construction with
  `std::invalid_argument`. Allocation failure remains `std::bad_alloc`; it is never translated to
  a client rejection.
- The facade and its implementation are non-copyable and non-movable. Like `MatchingEngine`, it is
  a single-writer object and provides no internal synchronization.
- Instruments do not share price levels, order nodes, or matching state. A command can match only
  within its routed instrument book.

`EngineSnapshot` owns the immutable catalog/configuration, the global sequence state, total active
count, and one `InstrumentSnapshot` for every configured instrument, including empty instruments.
Instrument snapshots contain only visible book state; they do not duplicate the authoritative
global sequence.

### Global sequencing and routing

- The multi-instrument coordinator is the sole owner of the authoritative sequencer. Individual
  books and executors do not allocate sequences.
- Domain submission reserves the next nonzero global sequence before pure or state validation.
  A pure, route, policy, identity, or capacity rejection publishes that sequence and one normalized
  rejection batch.
- Parse, numeric-conversion, and Python-representation failures happen before domain submission and
  consume no sequence.
- `UINT64_MAX` is published once. A later submission returns sticky
  `EngineError::sequence_exhausted`, emits no event, and mutates no state.
- Every header in a batch contains the reserved global sequence, the command's submitted instrument
  ID, and a contiguous zero-based event index. An unknown nonzero instrument therefore still has a
  deterministic sequenced rejection whose header preserves that unknown ID.
- A successfully resting new or replacement order uses the command's global sequence as its
  priority sequence. Global priorities need not be contiguous among active orders because rejected
  and terminal commands retain their places in the command history.

The engine does not expose an API that accepts a caller-selected sequence. Restored sequence state
belongs to the later checked recovery boundary, not ordinary command execution.

### Global active-order identity

The coordinator owns:

```text
OrderId -> {InstrumentId, ClientId}
```

This directory is non-owning metadata and must exactly mirror the union of all per-book active
indexes. Order nodes remain owned by their instrument book's storage.

- An order ID is unique while active across the entire engine.
- A terminal ID may be reused later on any configured instrument; no terminal-ID history is
  retained.
- Cancel and replace first route the submitted instrument, then use the global directory to
  distinguish an unknown order from client-ownership and instrument mismatches.
- Passive fills, cancel, replacement of the old ID, and aggressor completion remove terminal IDs
  from the directory. A resting residual publishes its new identity.

After pure command validation, stateful rejection precedence is:

1. New: configured instrument, instrument quantity/tick policy, globally active duplicate ID,
   then projected per-instrument and global capacity.
2. Cancel: configured instrument, globally unknown ID, client ownership, then instrument mismatch.
3. Replace: configured instrument, instrument quantity/tick policy, globally unknown old ID,
   client ownership, instrument mismatch, globally active new ID, then projected per-instrument and
   global capacity.

Capacity is based on the planned post-command state, not the initial count. Fully filled passive
orders and the replaced old order are subtracted, and a slot is added only for a resting GTC
residual. A market/IOC order or fully filled GTC order may therefore execute while either initial
capacity is full.

### Exclusive preparation and no-throw commit

One command is represented internally by an exclusive, move-only
`PreparedMultiInstrumentCommand`. It owns the book-level `PreparedCommandExecution`; at most one
engine token may exist, and neither token can outlive its owner.

Preparation:

- reserves but does not publish the next sequence;
- performs pure, route, policy, global-identity, and projected-capacity validation;
- creates the complete owned event batch;
- preallocates every order, level, per-book index entry, global-directory entry, and result needed
  by a committed command. A possible directory addition is held as an extracted map node after
  capacity is reserved; and
- records a value-only identity delta for every active ID added or removed by the command.

A domain rejection is also a successful preparation: it contains its rejection batch and an empty
book/identity delta. Abandoning any preparation releases its reservation and staged resources
without changing the visible books, global directory, or published sequence state.

Commit publishes the already verified book and exact identity changes, then publishes the sequence
through a no-allocation, no-throw path and runs allocation-free whole-engine invariants. An
impossible failure after commit begins is an internal fatal contract breach; it is not exposed as a
domain rejection.

Ordinary `execute()` prepares and immediately commits. The later persistence adapter may append and
synchronize a record between those operations. Only the component holding the exclusive token may
commit or abandon it.

This rule supersedes ADR 0006 only where that ADR allowed allocation failure to leave a sequence
gap. Domain rejections still consume a sequence, but allocation and other internal preparation
failures publish no sequence and leave no semantic mutation. ADR 0006's value-only planning,
allocation-before-mutation, event ordering, passive-residual staging, and fail-fast mutation
contracts otherwise remain in force.

### Invariants

Whole-engine validation requires:

- the catalog and book collections have the same sorted, unique instrument IDs;
- every instrument book satisfies its existing invariants with no pending preparation;
- the global directory is the exact union of all per-book active indexes;
- stored client and instrument identities agree in the node, per-book index, and global directory;
- active-order and level counts agree locally and globally;
- no active order ID or priority sequence occurs twice across instruments;
- every active priority is nonzero and no newer than the last published global sequence; and
- sequence-exhaustion state is consistent with the last published sequence.

Invariant failure is an internal engine failure and is never converted to `RejectReason`.

### Compatibility and deterministic evidence

`MatchingEngine`, `EngineResult`, `BookSnapshot`, their rejection precedence, and all existing
single-instrument observer behavior remain source compatible. The one-instrument facade delegates
to the same coordinator/execution path so that Phase 4 does not create a second matching
implementation.

Semantic version remains 6 because routing several unchanged books does not change the matching or
event semantics within any book. The `ATLSST01` state encoding, `ATLSEV01` event encoding, and
`ATLAS_DIFF_V1` adapter schema remain byte-for-byte unchanged.

Multi-instrument state uses a distinct `ATLSME01` SHA-256 input:

1. eight ASCII bytes `ATLSME01`;
2. semantic version (`u16`);
3. maximum total active orders (`u64`);
4. catalog count (`u64`), then each sorted entry as instrument ID (`u32`), maximum order quantity
   (`u64`), tick increment (two's-complement `u64`), and maximum active orders (`u64`);
5. last published global sequence (`u64`);
6. sequence-exhausted flag (`u8`);
7. total active-order count (`u64`);
8. instrument count (`u64`), then every sorted instrument snapshot.

Each instrument snapshot encodes instrument ID (`u32`), active-order count (`u64`), bid-level count
and best-to-worst bid levels, then ask-level count and best-to-worst ask levels. Levels and FIFO
orders use the exact scalar and order encodings frozen by ADR 0009. All integers are fixed-width
big-endian values; the stream contains no native `size_t`, padding, pointer, string, timestamp, or
unordered-container order. The digest is deterministic evidence, not a persistence format or
authentication mechanism.

The in-memory unbounded-capacity sentinel is `SIZE_MAX`, but both global and per-instrument
unbounded capacities encode as `UINT64_MAX`. Finite capacities encode as their exact `u64` value.
This keeps the default snapshot digest identical on 32-bit and 64-bit hosts.

Multi-instrument differential evidence uses a new `ATLAS_DIFF_V2` schema and a separately versioned
generator contract. V1 fixtures, generators, corpora, and reports are not rewritten.
`ReferenceRouter` owns one independent Phase 3 `ReferenceEngine` per sorted catalog entry, one
global sequencer, and an independently maintained global identity directory.

The independent-interleaving property preserves the command order within each instrument while
changing only cross-instrument interleaving. Since interleaving legitimately changes absolute
global sequences and resting priorities, comparison normalizes those absolute values while still
requiring identical per-instrument outcomes, economic state, price-time FIFO order, and
instrument-local event structure. Exact same-stream replay continues to compare unnormalized
sequences, events, snapshots, and digests.

## Alternatives considered

- One `MatchingEngine` and sequencer per instrument was rejected because interleaving would have no
  authoritative global order and active IDs could collide across books.
- Lazy book creation was rejected because a fixed, eagerly validated catalog gives deterministic
  routing, snapshots, configuration digests, and recovery behavior.
- Reusing a per-book index as the global identity source was rejected because cross-instrument
  cancel/replace validation would require scans and could not enforce one active-ID namespace
  atomically.
- Publishing the sequence before fallible preparation was rejected because resource failures would
  create command-log gaps and make write-ahead recovery ambiguous.
- Exposing caller-supplied sequences was rejected because adapters could introduce gaps,
  duplicates, rollover, or priority disagreement.
- Changing `ATLSST01` or `ATLAS_DIFF_V1` in place was rejected because Phase 2 and Phase 3 evidence
  must remain independently reproducible.
- Comparing raw priorities after independent-instrument reordering was rejected because global
  sequence differences are an intended consequence of the transformation rather than a semantic
  divergence.

## Consequences

All active books now share one sequence and identity namespace while retaining independent matching
state. This makes deterministic replay, engine-wide cancellation semantics, and later
snapshot/log recovery well defined.

Preparation requires temporary value state and preallocation above the existing per-book staging
layer. The global directory also duplicates a small amount of identity metadata. These are
intentional correctness costs; Phase 4 makes no latency, throughput, memory, scalability, or
production-readiness claim.

Persistence must use the exclusive prepare/commit boundary rather than execute first and log later.
Snapshot restoration must rebuild both the per-book indexes and the exact global directory before
publishing a recovered engine.

Any change to the `ATLSME01` byte layout requires a new prefix version. A change to matching or
event semantics requires a separate semantic-version decision and does not follow merely from
multi-instrument routing.

## Evidence

Phase 4 router acceptance requires:

- constructor tests for empty, zero-ID, duplicate-ID, unsorted, invalid-policy, and boundary-sized
  catalogs;
- global sequence tests across instruments, pure and state rejections, maximum sequence, and sticky
  exhaustion;
- cross-instrument duplicate, unknown, ownership, instrument-mismatch, ID-reuse, and rejection
  precedence tests;
- per-instrument and global projected-capacity tests, including full books that permit terminal
  commands;
- allocation-failure and abandoned-preparation tests proving no visible book, directory, event, or
  sequence mutation;
- whole-engine invariant and deliberate-corruption coverage;
- frozen Phase 0 through Phase 3 golden evidence;
- independent `ATLSME01` golden vectors and field-sensitivity tests;
- named and fixed-seed `ATLAS_DIFF_V2` comparison against `ReferenceRouter`; and
- independent-interleaving properties plus exact same-stream deterministic replay.
