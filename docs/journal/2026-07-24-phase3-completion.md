# 2026-07-24 - Phase 3 completion implementation and pending release evidence

## Outcome

The remaining Phase 3 implementation now exists: versioned deterministic workload generation,
fully resolved manifests, five explicit campaign tiers, a disk-spooled streaming differential
runner, portable failure bundles, deterministic semantic reduction, three injected-fault
demonstrations, applicable metamorphic properties, and bounded command-sequence fuzzing.

This is an implementation-and-local-evidence checkpoint, not the final release declaration. The
Python selections, fixed PR corpus, one checked million-command case, both GCC CTest
configurations, and the remaining local quality gates pass. Branch publication and hosted
compiler, sanitizer, formatting, and PR-corpus evidence are still pending.

## Generator and campaigns

Generator V1 uses frozen integer-only SplitMix64 sampling and a completely serialized workload
specification. It covers uniform, midpoint-clustered, hot-level, sparse-wide, cancel-heavy,
sweep-heavy, replace-heavy, invalid-mix, trace-driven trend/mean-reversion, and adversarial-boundary
profiles. Configurable invalid intent covers shape, state, ownership, tick, and range families.

A private generation shadow tracks only enough lifecycle state to target meaningful active and
inactive identities. It does not emit or influence expected outcomes, events, snapshots, or
digests. The independent Python engine and public native engine remain the only compared
implementations.

Campaign policy V1 resolves every profile, engine parameter, seed, and snapshot cadence. PR uses
10 fixed 5,000-command exact cases. Main uses 20 rotating 100,000-command exact cases. Nightly uses
10 rotating 1,000,000-command compact cases. Release uses 10 published 10,000,000-command compact
cases, and the sanitizer subset uses the same 10 seeds for 100,000 exact commands each.

The manual Release workflow gates campaign execution on full GCC and Clang Release build-and-CTest
jobs, then shards by compiler and case. The sanitizer subset is sharded per case.

## Comparison and diagnosis

The runner writes and closes the entire versioned reference evidence spool before it starts the
native process. Native JSONL is decoded and compared incrementally, so long cases do not require a
command-sized in-memory transcript. Each command compares classification, sequence, events or event
digest, top/count/aggregate state, state digest, and configured snapshots. Exact final state is
always compared.

Every semantic mismatch is rerun from fresh engines through the exact failing prefix, with one full
snapshot checkpoint at the divergence rather than every-command snapshots. The first structured
value difference records a stable failure signature, both state digests and tops, and the nearest
depth difference. Every portable bundle retains the original workload, first difference, recent
commands, build/runtime metadata, artifact digests, and a relative-path reproduction command;
campaign failures also retain their manifest and source provenance. Standard diagnostic bundles
retain both evidence streams; summary-only large-tier bundles may omit them.

Capacity-bound Release and sanitizer shards pass
`--exact-replay-command-limit 1000000`. A larger required prefix remains a semantic failure with
`diagnosis.status=deferred_command_limit`; it retains portable reproduction data while
intentionally omitting giant transcripts and requires manual exact reproduction on a suitable
host. It never counts as passing release evidence.

The reducer verifies the original signature in fresh processes, then deterministically removes
chunks and individual commands, remaps IDs, reduces prices and quantities, simplifies command
types, normalizes side/TIF, and performs a final deletion pass. Cached results and explicit
evaluation/time budgets make partial reduction honest and reproducible.

## Harness proofs and properties

Three development-only evidence transforms model newest-at-price selection, incoming-price trades,
and stale partial-fill aggregates. Each is detected through the real strict boundary and shrunk to
a checked human-readable fixture. These transforms never enter native engine code and are evidence
that the harness can find and reduce representative defects.

Metamorphic tests cover deterministic replay, side/price mirroring, splitting a market order, a far
nonmarketable level, and rejection-prefix isolation. Bounded Hypothesis tests compare byte-derived
command streams and single-field mutations of valid generated streams. The checked byte corpus
includes minimal, mixed golden, boundary, and prior-regression examples.

## Evidence collected

- Local default Python selection: 244 passed, 2 Windows-symlink skips, 11 deselected.
- Local marked `campaign or differential_fuzz` selection: 11 passed, 246 deselected.
- Local fixed PR policy: 10 × 5,000 exact commands passed.
- Checked epoch-0 nightly case 0: 1,000,000 compact commands passed.
- Local GCC Debug and Release CTest: 288/288 in each configuration.
- Local `BUILD_TESTING=OFF` production build, pinned clang-format, Ruff, strict mypy, and wheel
  build/install smoke gates passed.

The checked campaign JSON and fuzz/regression corpora are deterministic repository inputs. Passing
CI retains summaries and digests according to tier policy; divergences retain their original and
minimized portable bundles.

## Pending before Phase 3 release completion

- Publish the final branch and verify hosted GCC, Clang, ASan/UBSan, pinned formatting, the fixed
  PR-corpus job, and the two Linux link-safety checks skipped locally.

Phase 4 remains not started. Snapshot/restore continuation and independent multi-instrument reorder
properties move there because their required infrastructure does not exist yet. Protocol
encode/decode identity and decoder fuzzing remain Phase 6. Corrupt log and snapshot-deserializer
fuzzing remain deferred until those formats exist.

No campaign count or elapsed time is a latency, throughput, allocation, memory, scalability, or
production-readiness claim.
