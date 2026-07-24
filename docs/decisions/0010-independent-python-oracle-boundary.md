# ADR 0010: Independent Python oracle and native evidence boundary

- Status: accepted
- Date: 2026-07-24

## Context

Phase 2 compares the optimized engine with a structurally separate C++ map/deque model. That
closes the matching implementation loop, but both implementations still share one language,
compiler process, domain headers, and test binary. Phase 3 needs a second implementation that can
reproduce normalized events and complete visible state without importing private C++ transition
logic.

The existing `engine-fixture` is intentionally human-readable. It prints event and state digests,
but not complete event payloads or snapshots, uses only the default engine policy, and cannot
submit unknown enum representations. Extending it into a testing transport would weaken its
stable golden output and confuse a development fixture with a protocol.

Python bindings remain a later infrastructure phase. Pulling pybind11 into the correctness oracle
would couple oracle delivery to native packaging and make it easier for reference code to call the
implementation under test accidentally.

## Decision

### Native evidence adapter

Add a non-installed `atlas_diff_native` executable when `BUILD_TESTING` is enabled. It links only
the public `AtlasLOB::core` and `AtlasLOB::domain` targets and does not include private core
headers.

The adapter accepts a versioned numeric whitespace format. One header defines the routed
instrument, maximum order quantity, tick increment, maximum active orders, and snapshot interval.
Numeric commands preserve raw enum representations and explicit optional-price presence so every
typed domain value can cross the process boundary. Malformed adapter input is a harness failure,
not a domain rejection, and is never submitted to the engine.

The adapter writes versioned JSON Lines. Records contain command identity, authoritative outcome,
complete normalized values or their canonical digest, public observers, and checkpoint snapshots.
All signed and unsigned 64-bit domain values are decimal strings so readers that represent JSON
numbers as floating point cannot lose precision.

Exact mode includes complete ordered events and requested snapshots. Compact mode emits bounded
per-command evidence for long campaigns. A compact divergence is rerun through the first failing
command in exact mode. This format is a testing interface, not a wire protocol, persistence
format, installed CLI, or performance path.

### Python oracle

Create an internal Python 3.11-through-3.14 `atlaslob` correctness package with a
standard-library-only runtime. The oracle uses dictionaries, deques, and explicit price sorting.
It independently implements validation precedence, sequencing, matching, cancellation,
replacement, event construction, snapshots, and the canonical encodings frozen by ADR 0009.

The oracle may share documented numeric vocabulary and value schemas. It must not import a native
extension, parse C++ headers, call `atlas_diff_native` as transition logic, or reuse optimized
planner, mutation, event-builder, snapshot, or digest code. Native execution is accessed only by
the differential runner after the reference result has been produced.

Hand-derived expected cases and independently packed SHA-256 golden values form a third anchor so
agreement between two implementations is not the only evidence.

### Compatibility and failure boundary

Phase 3 does not change `MatchingEngine`, its command/event types, semantic version 6, or the
`ATLSST01` and `ATLSEV01` byte layouts. Adapter schema changes use their own version.

Normal rejections remain successful sequenced event batches. An unexpected native engine error,
adapter failure, Python internal error, or process failure is a harness failure and stops the
comparison. Allocation failure is not translated into a client rejection.

The Python model publishes ordinary dictionary/deque mutations directly. If a `MemoryError` or
another unexpected internal exception escapes after sequencing begins, that model instance is
poisoned and rejects every later observer or command. Phase 3 never treats Python exception
recovery as atomic evidence or continues a comparison from possibly partial oracle state.

The public engine has no restored initial-sequence seam. The Python model tests sticky sequence
exhaustion independently, while cross-language exhaustion comparison remains deferred until
restored engine state exists.

The native core preflights the `u32` event-index limit. A Python command that would require more
than `2^32` events is outside the Phase 3 process-resource envelope; generated campaigns cap
active orders and per-command event counts many orders of magnitude below that boundary. Native
internal-failure behavior at the theoretical limit is not modeled as an ordinary Python event
batch.

## Alternatives considered

- Extending `engine-fixture` was rejected because its stable human output and symbolic grammar do
  not carry the complete or raw evidence required for differential diagnosis.
- Adding pybind11 now was rejected because bindings, batching, native-backed distribution
  packaging, and GIL policy belong to Phase 4 and would couple the oracle to the implementation
  process.
- Reusing or exporting the Phase 2 C++ reference model was rejected because shared language,
  headers, and transition code weaken independence.
- Introducing a JSON parser in C++ was rejected. The fixed numeric input grammar is smaller,
  bounded, and can represent every required typed value without a new runtime dependency.

## Consequences

Cross-language campaigns can compare exact semantic values while the deterministic core remains
independent of Python and I/O. Failures can be persisted and reproduced without exposing mutable
internals.

The cost is deliberately duplicated semantic logic and a separate evidence schema that must be
kept compatible with the written contract. Every semantic change must update both implementations
and hand-derived cases deliberately; simple mechanical regeneration is not sufficient review.

The adapter and snapshot paths allocate and serialize complete values. No latency, throughput,
memory, or production-protocol claim follows from Phase 3 evidence.

## Evidence

- Native adapter schema and golden integration tests.
- Python domain, digest, and reference-engine unit tests.
- Named cross-language matching cases.
- Fixed-seed differential corpus, shrinker demonstrations, and long-campaign summaries delivered
  by later Phase 3 slices.
