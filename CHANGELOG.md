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
- Extended per-command state evidence with exact bid/ask level, order, and aggregate summaries.
- Replaced command-sized native transcript buffering in long differential cases with incremental
  strict JSONL decoding, a bounded line queue, disk-spooled reference evidence, and rolling
  digests.
- Made every semantic divergence use a fresh exact prefix or full replay with one snapshot
  checkpoint at the divergence. Capacity-constrained Release and sanitizer shards defer prefixes
  above 1,000,000 commands with `diagnosis.status=deferred_command_limit` rather than materializing
  giant transcripts or treating the case as passing evidence.
- Made reused evidence outputs fail closed around links and unexpected owned-file types, clear only
  runner-owned stale case/failure artifacts, and preserve unrelated manual reproduction content.
- Bound the checked PR and million-command evidence to exact schemas and frozen digests, with a
  marked test that regenerates all 1,050,000 underlying V1 commands and statistics.

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
  CI.
- A normal-wheel build/install smoke gate that imports the oracle without development
  dependencies and verifies its `py.typed` marker.
- Native process fault-injection and black-box CLI coverage for throwing/partial output, terminal
  flush failure, every documented mode spelling, and invalid usage.
- ADR 0010 and a versioned differential-interface reference.
- Generator V1 with frozen SplitMix64 vectors, ten named workload profiles, configurable valid and
  invalid intent, boundary-biased quantities, and fully resolved serializable specifications.
- Canonical workload manifests that bind generator version, seed, engine/distribution policy,
  command-stream SHA-256, and generation statistics.
- Versioned predefined PR, main, nightly, release, and sanitizer campaign policies with fixed,
  explicit rotating-epoch, or published seed provenance.
- A disk-spooled differential runner that completes reference evidence before native execution,
  compares every streamed record, retains structured first-divergence data, and exact-reruns every
  semantic divergence from fresh engines.
- Portable versioned failure bundles with original and minimized fixtures, state/top/depth
  differences, build metadata, artifact digests, and relative-path reproduction. Standard
  diagnostic bundles retain both evidence streams; summary-only large-tier bundles may omit them.
- Deterministic signature-preserving semantic reduction across chunks, commands, identifiers,
  prices, quantities, command types, side, and time in force.
- Three development-only evidence-boundary fault proofs for newest-at-price, incoming trade price,
  and stale partial-fill aggregates, with checked minimized regression fixtures.
- Metamorphic coverage for replay, side/price mirroring, split market orders, far nonmarketable
  levels, and rejection-prefix isolation.
- Bounded Hypothesis byte-stream and valid-sequence mutation fuzzing with minimal, golden,
  boundary, and prior-regression seed files.
- A pull-request 10-by-5,000 exact campaign job plus explicit main and nightly workflows and
  compiler-by-case Release/per-case sanitizer shards with tiered artifact retention. Manual
  Release work requires full GCC and Clang Release build-and-CTest gates first.
- Checked local closure evidence: 244 passed/2 Windows-symlink skips/11 deselected in the default
  Python selection, 11 passed/246 deselected in the marked selection, the 10-by-5,000 exact PR
  corpus, one epoch-0 1,000,000-command compact nightly case, both 288-test GCC configurations,
  production-only and formatting gates, Ruff, strict mypy, and wheel build/install smoke. The
  published Phase 3 PR implementation head passed all required hosted compiler, sanitizer, Python,
  formatting, wheel, PR-corpus, and Linux link-safety checks. Phase 3 is complete; Phase 4 has not
  started.
- ADR 0011, the Phase 3 evidence index, and documented dependency deferrals for Phase 4,
  Phase 6, and future persistence-format fuzzing.
