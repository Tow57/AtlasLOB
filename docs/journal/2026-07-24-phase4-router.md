# 2026-07-24 - Phase 4 multi-instrument router

## Outcome

The first Phase 4 slice is implemented and locally validated on `codex/phase4-router`.
It introduces deterministic multi-instrument routing above the existing matching book without
changing semantic version 6 or the frozen `ATLSST01`, `ATLSEV01`, and `ATLAS_DIFF_V1` evidence
contracts.

This is not a Phase 4 completion record. Command logging, replay and inspection, persisted
snapshot/log-suffix recovery, and native Python bindings remain later sequential PRs.

## Phase 3 prerequisite status

Published Phase 3 PR #5 head `29049756e250fef04aac819c457438f0f01149c3` passed the required
hosted GCC/Clang Release, ASan/UBSan, Python 3.11-3.14, formatting, fixed PR corpus, wheel, and
Linux link-safety checks. The implementation and evidence gate is closed.

PR #5 is still open rather than remotely merged because GitHub authentication is unavailable in
this session. Remote `main` therefore remains at Phase 2. The Phase 4 work is stacked locally on
the published Phase 3 head and must retain that ancestry when remote integration becomes
available.

## Router and identity boundary

`MultiInstrumentEngine` owns an immutable sorted catalog, eagerly constructs one independent book
per configured instrument, and assigns one global command sequence across every routed command and
domain rejection. Unknown instruments are deterministic sequenced rejections; parsing remains
outside domain submission.

The coordinator owns an engine-wide `OrderId -> {InstrumentId, ClientId}` directory in addition to
each book's non-owning index. This enforces active-ID uniqueness across instruments and makes
unknown, ownership, instrument-mismatch, and replacement-ID precedence deterministic. Terminal IDs
may be reused later on any configured instrument.

Capacity is projected against the final state at both levels. Passive terminal orders and a
replaced old order are removed before a possible new residual is counted, so an initially full
book or engine may still accept a command that does not increase its final active count.

The existing `MatchingEngine` now delegates through a one-entry multi-instrument coordinator. Its
public API, snapshots, and canonical state digest remain the Phase 2 contract rather than a second
implementation.

## Preparation and failure boundary

`PreparedCommandExecution` is a move-only internal token that owns the immutable precommit event
batch, bound reductions, projected top, and any prepared residual for an externally reserved
sequence. Destruction abandons staged state through RAII. Its one-shot commit performs only
prevalidated, allocation-free book mutation.

`PreparedMultiInstrumentCommand` is the corresponding engine-wide token. Under one exclusive lease
it owns the submitted command, reserved sequence, complete result, prepared book mutation, exact
identity removals, and any possible global identity addition. The addition is preallocated as an
extracted hash-map node after directory capacity is reserved.

Resource failure destroys the engine token, abandons book staging, removes provisional state, and
leaves both the visible snapshot and next sequence unchanged. Its no-throw commit publishes the
preallocated directory delta, prepared book mutation, and global sequence, then checks
allocation-free whole-engine invariants. Domain rejections are successful preparations with no
identity/book mutation and still publish one sequence. Ordinary `execute()` uses this exact
prepare-then-commit path.

The private access seam is ready for later persistence to append between prepare and commit. PR1
does not claim that the write-ahead log or persistence session itself exists yet.

## Deterministic evidence

`EngineSnapshot` records the sorted catalog and policies, one global sequence/exhaustion state,
total active count, and every configured instrument in canonical best-price/FIFO order. The
separate `ATLSME01` encoding hashes those values with the fixed-width big-endian rules established
by ADR 0009.

The Python package adds an independent `ReferenceRouter`, Generator V2, canonical V2 workload and
manifest schemas, multi-engine evidence capture, shrinking primitives, and a constrained
independent-instrument reinterleaving property. Reinterleaving preserves per-instrument command
order and normalizes only absolute global sequence/priority values that are expected to change.
Generator V1 and every Phase 3 campaign/failure schema remain unchanged.

The separate non-installed `atlas_diff_multi_native` executable provides a strict
`ATLAS_DIFF_V2` process boundary. It parses the complete catalog and declared command stream before
constructing/submitting to the engine, then emits versioned `atlas_diff_v2` exact or compact JSON
Lines with per-command event/state digests, optional checkpoints, and an exact final snapshot.
Malformed adapter input therefore consumes no engine sequence.

The Python `atlaslob.multi_native` boundary encodes V2 input, invokes the selected executable,
strictly decodes its closed JSONL schema, binds the transcript to the requested catalog, mode,
command stream, checkpoints, and process exit, recomputes digests/snapshot invariants, and compares
exact results with the independent reference capture.

## Validation status

GCC Debug and Release each pass 333/333 CTest tests, including fixed-seed multi-engine stress and
the frozen V1 adapter. The full Python suite passes 296 tests with two Windows symlink-privilege
skips; the focused Phase 4 router/V2 selection passes 41 tests. Pinned C++ formatting, Ruff, strict
mypy across 41 source files, generated exact/compact cross-language parity, `git diff --check`, and
the machine-path/secret scan pass locally.

Clang, ASan/UBSan, and hosted PR gates still require the hosted environment. No remote merge or
green hosted Phase 4 PR gate is claimed by this journal.

No command count or test duration is a latency, throughput, allocation, memory, scalability,
durability, or production-readiness result.
