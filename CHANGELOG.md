# Changelog

All notable changes to AtlasLOB will be documented here.

The format is based on Keep a Changelog, and public releases will follow semantic versioning.

## [Unreleased]

### Changed

- Made public `EngineResult` states mutually exclusive through validated factories and read-only
  observers.
- Replaced prepared replacement raw-pointer identity with a pinned active `OrderId`; direct
  mutation of the old order is rejected until commit or rollback.
- Successful `BookSide::PreparedLevel` commit now invalidates its guard completely.
- Kept the expensive invariant toggle private to the core build instead of exporting it to
  consumers.
- Replaced two tautological canonical-digest assertions with an independently reproduced Python
  rejection hash and the existing cross-instance/snapshot comparisons.
- Made native adapter selection fail closed: an explicit `ATLAS_DIFF_NATIVE` never falls back, and
  a complete evidence run without any adapter now fails instead of skipping process parity.
- Hardened the native adapter's process boundary so read, snapshot, digest, serialization, output,
  and terminal-flush failures return the documented fatal exit without appending a second record
  after partial output.
- Bound decoded native transcripts to the requested mode, configuration, command stream,
  checkpoint cadence, contiguous sequence timeline, process exit, and final state; impossible
  event envelopes, snapshots, and malformed numeric JSON now fail as protocol errors.
- Aligned the internal oracle package and CI support window to Python 3.11 through 3.14.

### Added

- C++20 domain library, CLI, and unit-test foundation.
- Strong identifier, price, quantity, and sequence values.
- Deterministic new-order validation and demonstration command.
- Client identity plus new, cancel, and replace command variants.
- Explicit command, event, completion, and rejection vocabularies.
- Normalized value-only event schemas and deterministic sequence/identity policy.
- Canonical text command fixtures with separate parse and domain-validation outcomes.
- Pinned GoogleTest 1.17.0 test infrastructure.
- Private `atlas_core` implementation layer for mutable book internals.
- Stable heap-owned order nodes with checked creation and destruction.
- Checked, non-owning intrusive FIFO price levels with aggregate quantity and order count.
- Cycle-safe price-level invariants and fixed-seed storage/level stress coverage.
- Ordered bid and ask maps with stable level addresses, best-price access, and guarded level
  preparation.
- Checked active-order indexing, direct indexed cancellation, empty-level cleanup, safe live-book
  teardown, and full storage/index/FIFO invariants.
- Fixed-seed 10,000-operation model stress coverage for mixed resting, reduction, and cancellation.
- Monotonic command sequencing, deterministic pure/state admission, and explicit internal failure
  boundaries.
- Read-only best-price/FIFO match planning with final active-order capacity projection.
- Immutable command-owned event batches with exact preallocated slots and contiguous headers.
- Allocation-before-mutation preparation for a future GTC residual, including rollback and
  allocation-free publication at existing or detached price levels.
- Atomic New execution for limit GTC, limit IOC, and market IOC with passive-price trades,
  price-time sweeps, residual handling, and coalesced final top-of-book events.
- Sequenced Cancel execution with current-residual cancellation events and direct terminal
  removal.
- All-preflight passive reduction batches and allocation-free final top-of-book projection.
- Deterministic exception evidence for exact prepared-residual rollback after the command sequence
  has been consumed.
- Atomic Replace execution with retained identity/side, new FIFO priority, passive-price matching,
  final-state capacity checks, and one normalized event batch.
- Replacement-specific residual preparation and all-preflight mutation with same-level aggregate
  relief and exact exception rollback.
- Public non-owning-detail-free `MatchingEngine` PImpl with typed/variant command execution,
  policy configuration, sequence observers, active count, and top-of-book values.
- Exact value-only book snapshots in canonical best-price/FIFO order.
- Versioned, fixed-width, big-endian state and event encodings with domain-separated SHA-256
  digests and independently generated golden values.
- Deterministic `engine-fixture` execution with per-command event/state digests, outcome summaries,
  strict instrument parsing, and explicit exit-code precedence.
- Independent map/deque matching model coverage for 10,000 mixed commands plus a 2,500-command
  deterministic transcript rerun.
- Regression coverage for preparation-stage allocation failures, replacement pin move/rollback,
  passive terminal-ID reuse, SHA-256 padding boundaries, and signed-price canonical encoding.
- ADR 0003 documenting node ownership, pointer invalidation, and internal error boundaries.
- ADRs 0004 through 0009 documenting ordered-side, indexed-book, admission, planning,
  execution-preparation, atomic New/Cancel, atomic Replace, and public API boundaries.
- GCC, Clang, formatting, ASan, and UBSan workflow definitions.
- Initial semantic contract, roadmap, and architecture decision record.
- A non-installed `atlas_diff_native` adapter with strict numeric input, exact/compact JSONL
  evidence, complete event serialization, checkpoint snapshots, and fatal harness boundaries.
- A typed Python 3.11-through-3.14 internal `atlaslob` correctness package with no runtime
  dependencies.
- Independent ADR 0009 state/event encoders that reproduce the frozen C++ golden hashes.
- A plain dictionary/deque Python matching oracle with validation, sequencing, New/Cancel/Replace,
  invariants, capacity/overflow checks, and fatal poisoning after internal exceptions.
- Strict native JSONL decoding that revalidates canonical digests, snapshot aggregates, ordering,
  active identities/priorities, instrument consistency, top of book, sequence observers, and the
  adapter error-code vocabulary.
- Named exact cross-language scenarios comparing every command result, event, snapshot, observer,
  and digest.
- Pinned pytest, Hypothesis, Ruff, and mypy top-level development tooling plus Python 3.11-3.14
  CI; Hypothesis remains reserved for the next seeded-generation slice.
- A normal-wheel build/install smoke gate that imports the oracle without development
  dependencies and verifies its `py.typed` marker.
- Native process fault-injection and black-box CLI coverage for throwing/partial output, terminal
  flush failure, every documented mode spelling, and invalid usage.
- ADR 0010 and a versioned differential-interface reference.
