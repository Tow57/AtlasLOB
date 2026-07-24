# ADR 0009: Canonical snapshots, digests, and deterministic evidence

- Status: accepted
- Date: 2026-07-24

## Context

The Phase 2 engine now executes New, Cancel, and Replace atomically, but ordinary unit assertions
are not sufficient replay evidence. A command-stream runner needs a compact way to prove both the
complete event result and the complete visible resting state after every submitted command.
Those values must not depend on object addresses, native endianness, structure padding, unordered
container iteration, compiler choice, or text formatting.

The evidence interface also needs a precise compatibility boundary. Hashing an in-memory
structure directly would make harmless representation changes alter results unpredictably, while
an undocumented serializer would make a digest impossible to reproduce independently.

## Decision

### Canonical snapshot

`MatchingEngine::snapshot()` returns one value-only `BookSnapshot` containing:

- semantic version, routed instrument, last issued sequence, and sticky exhaustion state;
- active-order count;
- bid levels in descending price order and ask levels in ascending price order;
- each level's price, aggregate remaining quantity, and orders in FIFO order; and
- each order's ID, client, instrument, side, price, remaining quantity, and priority sequence.

A fresh engine reports last sequence zero. An exhausted engine reports the maximum sequence that
was issued. Snapshot construction first requires a valid book with no pending preparation.
The engine remains single-writer; callers must not observe it concurrently with mutation.

### Scalar encoding

Both digest encodings use fixed-width unsigned integers in network byte order:

| Domain value | Encoding |
| --- | --- |
| enum or boolean | `u8`; booleans are exactly `0` or `1` |
| rejection reason or semantic version | `u16` |
| client ID, instrument ID, event index | `u32` |
| order ID, sequence, quantity, count | `u64` |
| price ticks | signed value represented by its two's-complement `u64` bit pattern |

No native structure bytes, padding, strings, terminators, variable-width integers, or floating
point values enter the encoding. Collection counts precede their elements.

### State digest encoding

The state stream is concatenated in this exact order:

1. eight ASCII bytes `ATLSST01`;
2. semantic version (`u16`);
3. instrument ID (`u32`);
4. last issued sequence (`u64`);
5. sequence-exhausted flag (`u8`);
6. active-order count (`u64`);
7. bid-level count (`u64`) followed by every best-price-first bid level;
8. ask-level count (`u64`) followed by every best-price-first ask level.

Each level encodes price (`i64` bit pattern), aggregate quantity (`u64`), order count (`u64`),
then every FIFO order. Each order encodes order ID (`u64`), client ID (`u32`), instrument ID
(`u32`), side (`u8`), price (`i64` bit pattern), remaining quantity (`u64`), and priority
sequence (`u64`).

### Event digest encoding

The event stream begins with:

1. eight ASCII bytes `ATLSEV01`;
2. semantic version (`u16`);
3. batch command sequence (`u64`);
4. batch instrument ID (`u32`);
5. event count (`u64`).

Every event then encodes its variant-derived event type (`u8`), command sequence (`u64`), event
index (`u32`), instrument ID (`u32`), and the following payload:

| Event | Payload encoding |
| --- | --- |
| Accepted | command type (`u8`) |
| Rejected | command type (`u8`), reason (`u16`), optional order ID |
| Trade | aggressor/resting IDs (`u64` each), aggressor/resting clients (`u32` each), aggressor side (`u8`), execution price (`i64`), execution quantity and both remaining quantities (`u64` each) |
| Rested | order ID (`u64`), client (`u32`), side (`u8`), price (`i64`), remaining quantity (`u64`) |
| Canceled | order ID and canceled quantity (`u64` each) |
| Replaced | old and new order IDs (`u64` each) |
| Done | order ID (`u64`), reason (`u8`), remaining quantity (`u64`) |
| BookChanged | optional best bid followed by optional best ask |

An optional order ID encodes a presence flag plus one `u64`; an absent value uses a zero
placeholder. An optional top level encodes a presence flag, price, and quantity; an absent value
uses zero placeholders. The placeholders make the optional representations fixed-width and
unambiguous.

The resulting byte stream is hashed with SHA-256 and rendered as 64 lowercase hexadecimal
characters. State and event prefixes provide domain separation, and their `01` suffix versions
the byte layouts independently of the semantic version field. These digests are deterministic
comparison and replay evidence, not authentication or a wire protocol.

### Executable fixture

`atlas_cli engine-fixture <instrument_id> <path>` reuses the canonical development command
grammar. Syntax and numeric-conversion failures are reported before submission and consume no
sequence. Every submitted command prints:

- source line and authoritative batch sequence;
- committed or rejected classification;
- ordered event-type names;
- complete event digest; and
- complete post-command state digest.

The final summary prints command/outcome/error counts, last issued sequence, and final state
digest. Exit codes are `0` for all committed, `1` when any command is rejected, `2` for any
parse/input error, and `3` for any engine failure, with engine error taking highest precedence.
This is a development and regression adapter, not a performance protocol.

### Reference-model evidence

A test-only reference engine uses `std::map<price, std::deque<order>>`, linear identity searches,
and separately written validation, capacity, matching, mutation, and event-order logic. It does
not call private AtlasLOB planners or mutation helpers. For each generated command it compares:

- commit/reject classification and authoritative sequence;
- every event variant, header, payload, and order;
- event digest;
- complete canonical snapshot and state digest; and
- top of book, active count, emptiness, and next sequence.

Four fixed seeds cover 10,000 mixed commands, and one 2,500-command seed is rerun to compare the
complete digest transcript. A 66-command directed scenario reaches the exact active-order limit,
checks atomic `capacity_exceeded` rejection, and proves a terminal market command can still execute.
This C++ model closes the Phase 2 implementation loop quickly. It does not replace Phase 3's
independent Python model, failure shrinking, long campaigns, or fuzzing.

## Consequences

Any semantic field, FIFO order, best-price order, event order, header, optional presence, or
payload change affects the corresponding digest. Representation-only changes behind the public
snapshot do not.

Changing a canonical byte layout requires a new domain-prefix encoding version. Changing engine
semantics requires updating the semantic version and its expected golden digests. Golden values
can be reproduced without compiling AtlasLOB because the encoding is fully specified here.

Snapshot and hashing are evidence-path operations with allocations and full-book traversal. No
throughput or latency claim is attached to them.

## Evidence

- Published SHA-256 empty and `abc` vectors pass.
- Empty and representative state hashes plus an all-event-variant hash match values generated by
  Node's independent `crypto` SHA-256 and a separately written big-endian buffer encoder.
- Tests prove sensitivity to every snapshot field, FIFO ordering, every event alternative, batch
  identity, headers, payloads, and optional values.
- Golden engine fixtures cover committed, domain-rejected, wrong-route, and malformed commands,
  including proof that parse failures do not consume a sequence.
- The reference model compares 12,566 executed commands including the deterministic rerun and
  directed exact-capacity scenario.
- Local Debug and Release suites pass 254/254 tests, and the production-only build succeeds.
  Hosted GCC, Clang, ASan, and UBSan results remain a pull-request requirement.
