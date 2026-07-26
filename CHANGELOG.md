# Changelog

All notable changes to AtlasLOB will be documented here.

The format is based on Keep a Changelog, and public releases will follow semantic versioning.

## [Unreleased]

### Changed

- Migrated the `atlaslob` distribution to scikit-build-core 1.0.3 and version 0.2.0 while keeping
  the native extension opt-in for ordinary CMake builds and preserving extension-free imports for
  the independent oracle, generators, shrinkers, and differential tooling.
- Added a lazy `atlaslob.Engine` export with an explicit private binding-ABI check and no silent
  fallback to the Python reference model.
- Routed the existing single-instrument `MatchingEngine` through the shared multi-instrument
  coordinator without changing its public API or frozen Phase 2/3 evidence encodings.
- Moved authoritative publication of command sequences above individual instrument executors.
  Domain rejections still consume a sequence, while an internal preparation/allocation failure
  abandons staged work without publishing that reserved sequence.
- Split the internal executor and coordinator boundaries into move-only prepared commands with an
  inspectable owned batch, one engine-wide lease, exact identity deltas, RAII abandonment, and a
  one-shot allocation-free commit that publishes book, directory, and sequence state.
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
- Made locally built MinGW extension wheels self-contained with respect to the GCC, C++, and
  winpthreads runtime DLLs while retaining the CPython DLL as the intended dynamic dependency.
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

- Resumable Phase 5 plan execution across all 51 frozen measurement shapes, with round-robin
  attempts, canonical unique run identities, retained invalid evidence, non-overwriting resume,
  physical-host consistency checks, boundary-separated deterministic reports, transactional
  retryable finalization, and exact bundle verification.
- Made bounded target-wheel identity capture classify output overflow deterministically even when
  the child exits before the orchestrator observes the overflow signal.
- Separate fixed-counter `perf stat` and DWARF call-graph capture that validates the profiled
  runner result, retains inaccessible-counter failures, summarizes counter availability and
  distributions, sanitizes text reports, and keeps profiling outside headline timing evidence.
- A bare-metal Ubuntu 24.04 native-host runbook covering CPU/SMT selection, Clang/GCC and CPython
  builds, official environment qualification, the 510-observation baseline, W04/W05 profiles,
  bundle verification, and local checkpointing without changing host or kernel policy.
- ADR 0016 and a Phase 5 methodology contract separating C++ core, replay, Python batch,
  allocation, memory, and future gateway evidence; defining native-host qualification,
  deterministic sampling, process-level observations, robust statistics, optimization acceptance
  gates, privacy rules, and the exact-tag release workflow.
- An opt-in benchmark-contract foundation for deterministic workload/environment manifests,
  strict observation/report schemas, core and allocation runners, Google Benchmark microcases,
  standard-library Python orchestration and analysis, and threshold-free Ubuntu smoke validation.
- A private pybind11 3.0.4 `_native_engine` module and public immutable Python engine values for
  live execution, write-ahead logged execution, clean writable recovery, torn valid-prefix
  read-only recovery, top-of-book inspection, snapshots, state digests, and snapshot publication.
- Strict Python catalog and command conversion with exact object types, explicit `bool`
  rejection, checked integer ranges, raw in-range enum forwarding, Unicode path support, and
  complete finite-batch preflight before the first command is submitted.
- Prefix-committing native batch submission with owned object, typed-column, and summary payload
  modes, consistent outcome accounting and final digests, terminal engine-error stopping, and
  structured persistence-failure prefix evidence.
- Per-engine mutex serialization with one lock held across a complete batch, released-GIL
  Python-free C++ execution, and caller-owned results that remain valid after later engine
  mutation or destruction.
- CPython 3.11-3.14 manylinux x86-64 wheel and PEP 517 source-distribution builds using
  cibuildwheel 4.1.0, with clean-environment smoke, wheel-content, dynamic-dependency, exact
  one-symbol extension-export, import isolation, binding parity, recovery, concurrency, and
  ownership coverage.
- ADR 0015 documenting the native import/ABI boundary, strict preflight, batch semantics,
  persistence errors, read-only recovery, GIL/mutex ordering, result ownership, and packaging
  policy.
- A non-copyable `MultiInstrumentEngine` facade with an immutable sorted catalog, eager independent
  books, global sequence observers, per-instrument top/snapshot access, and complete engine
  snapshots.
- An engine-wide active-order identity directory, deterministic cross-instrument
  unknown/ownership/instrument/duplicate precedence, active-ID reuse after terminal state, and
  projected per-instrument plus global capacity.
- Whole-engine invariants covering catalog/book correspondence, local indexes, the global
  active-ID directory, a transactional reverse active-priority directory, active counts, globally
  unique priorities, and sequence bounds without a pairwise priority scan.
- Canonical big-endian `ATLSME01` multi-engine state encoding and SHA-256 digest while preserving
  semantic version 6, `ATLSST01`, `ATLSEV01`, and `ATLAS_DIFF_V1`.
- Independent Python `ReferenceRouter`, Generator V2, canonical V2 workload/manifest schemas,
  multi-engine evidence capture, deterministic shrinking primitives, and constrained
  cross-instrument reinterleaving checks.
- A non-installed `atlas_diff_multi_native` adapter that parses a complete strict
  `ATLAS_DIFF_V2` catalog/command stream before submission and emits versioned exact or compact
  `atlas_diff_v2` JSON Lines, plus a strict Python encoder/runner/decoder and exact reference
  parity checker.
- ADR 0012 documenting multi-instrument routing, global sequencing and identity, preparation
  compatibility, invariants, and deterministic V2 evidence.
- ADR 0013 and a byte-level command-log reference freezing `ATLSLG01` V1, `ATLSCF01`,
  fixed command records, CRC32C coverage, bounded scanner classifications, write-ahead durability
  and poisoning, copy-only tail repair, replay modes, deterministic report schemas, and the fatal
  impossible-commit boundary.
- A separate `AtlasLOB::persistence` target with canonical command-log codecs, checked big-endian
  arithmetic, Castagnoli CRC32C, deterministic configuration hashing, a bounded scanner, structured
  diagnostics, and exact valid-prefix identity checks across replay passes.
- A file-backed write-ahead `LoggedEngine` with buffered, flush-per-record, and sync-per-record
  durability modes; sequenced rejection logging; allocation-free core publication; and sticky
  poisoning after partial write, flush, or sync failure.
- `atlas_inspect` and `atlas_replay` tools with stable exit codes, deterministic text/JSON reports,
  clean-log refusal, safe torn-tail repair, strict/valid-prefix policies, and
  fast/verify/diagnostic replay.
- Header and record libFuzzer targets, retained canonical seed generation, and a hosted bounded
  ASan/UBSan fuzz-smoke job.
- ADR 0014 and a byte-level persisted-snapshot reference freezing `ATLSSN01` V1, the 256 MiB
  default bound, whole-file CRC32C, exact hierarchy and digest validation, canonical names,
  no-overwrite publication, log-boundary pairing, and snapshot-aware report schemas.
- A bounded persisted-snapshot codec with checked lengths/counts, a reviewed byte-exact golden,
  deterministic truncation/corruption classification, configuration/state digest verification,
  exact field/entry diagnostic offsets across value and byte entry points, and a dedicated
  snapshot decoder fuzz target plus retained canonical seed.
- Bounded all-or-nothing bulk restoration that reserves storage and local/global directories,
  allocates every level, node, and index entry before linking, then reconstructs exact FIFO state,
  active counts, priorities, authoritative global sequence, and exhaustion through a private
  temporary-engine boundary.
- `LoggedEngine::write_snapshot`, standalone snapshot inspection, explicit or newest-valid
  directory recovery, exact log-suffix replay, deterministic candidate-skip diagnostics, and
  `ATLAS_SNAPSHOT_REPORT_V1`/`ATLAS_REPLAY_REPORT_V2` tooling without changing log-only report V1.
- `LoggedEngine::recover`, `recover_from_snapshot`, and `recover_from_snapshot_directory` factories
  that retain one source across validation, require a clean tail, and reopen only the exact
  validated extent with existing-only append semantics. Torn-tail valid-prefix replay remains
  non-resumable until safe copy-only repair creates a clean log.
- Snapshot fault evidence for every injected restore allocation and publication stage, sticky
  poisoning on pre-snapshot log-sync failure, no-overwrite behavior, preservation of previous good
  snapshots, cleanup-error reporting, invalid-newer/valid-older selection, non-followed canonical
  symlinks, candidate-local I/O skipping, and full-log fallback.
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
  published Phase 3 PR #5 head `29049756` passed all required hosted compiler, sanitizer, Python,
  formatting, wheel, PR-corpus, and Linux link-safety checks. Phase 3 and all four Phase 4 slices
  were then retargeted, revalidated, and squash-merged sequentially: PR #5 as `3c60e2b`, router PR
  #7 as `3d6d438`, command-log/replay PR #8 as `f5c2f11`, snapshot/recovery PR #6 as `25b0a39`,
  and native-Python PR #9 as `075d29a`. The final uncancelled `main` workflow passes GCC and Clang
  Debug/Release, ASan/UBSan, decoder-fuzz smoke, Python 3.11-3.14, formatting, the native Clang
  binding, source-distribution build/install smoke, and the CPython 3.11-3.14 manylinux wheel,
  audit, clean-container, and native/reference parity gates.
- ADR 0011, the Phase 3 evidence index, and documented dependency deferrals for Phase 4,
  Phase 6, and future persistence-format fuzzing.
