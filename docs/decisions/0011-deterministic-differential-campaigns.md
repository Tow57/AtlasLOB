# ADR 0011: Deterministic differential campaigns and failure reduction

- Status: accepted
- Date: 2026-07-24

## Context

ADR 0010 established an independent Python oracle and a versioned process boundary to the public
C++ engine. Named exact scenarios and frozen hash vectors prove important transitions, but they do
not provide broad, replayable coverage of command distributions or a disciplined way to retain and
minimize a newly discovered divergence.

Phase 3 therefore needs a deterministic workload contract, bounded-memory execution for long
campaigns, portable failure artifacts, semantic reduction, and explicit campaign tiers. These
mechanisms are correctness evidence. They are not benchmarks, production protocols, or permission
to couple the generator to either implementation under comparison.

## Decision

### Generator and workload identity

`GENERATOR_VERSION = 1` identifies the command-generation algorithm. A workload is identified by
that version, one explicit unsigned 64-bit seed, a fully resolved `atlas_workload_spec_v1`, the
command count, the canonical command-stream SHA-256 digest, and generation-intent statistics.
Every profile default is resolved before serialization. Reproduction never depends on the Python
version, wall clock, process ID, platform entropy, dictionary iteration order, or an undocumented
default.

Generation uses a frozen integer-only SplitMix64 implementation and rejection sampling for bounded
choices. Floating-point sampling and the Python `random` module are excluded from workload
identity. Checked cross-version vectors protect the PRNG contract.

The generator exposes ten workload profiles:

1. `uniform_synthetic`
2. `clustered_mid`
3. `hot_level_contention`
4. `sparse_wide`
5. `cancel_heavy`
6. `sweep_heavy`
7. `replace_heavy`
8. `invalid_mix`
9. `trace_driven_synthetic`
10. `adversarial_boundary`

Resolved specifications independently record the price model, operation weights, invalid,
aggressive, market, and boundary-quantity basis points, midpoint, price span, active-order target,
client count, engine policy, and snapshot cadence. The trace-driven profile uses a deterministic
trend/mean-reversion price model; the boundary profile selects legal and deliberately invalid
fixed-width edges.

The generator maintains a private lifecycle shadow so cancel and replace commands can target known
active or inactive IDs and generated order flow can reach useful states. That shadow is not a
correctness oracle: it emits no expected classification, sequence, event, snapshot, aggregate, or
digest, and its state never participates in comparison. The Python reference model and native
engine independently determine all expected and actual evidence.

### Campaign policy

Campaign policy version 1 assigns each tier an exact size, comparison mode, checkpoint cadence,
profile mapping, and seed provenance:

| Tier | Cases × commands | Mode | Snapshot cadence | Seed policy |
| --- | ---: | --- | ---: | --- |
| Pull request | 10 × 5,000 | Exact | 100 | Fixed literal V1 set |
| Main | 20 × 100,000 | Exact | 1,000 | Explicit rotating epoch |
| Nightly | 10 × 1,000,000 | Compact | 10,000 | Explicit rotating epoch |
| Release | 10 × 10,000,000 | Compact | 50,000 | Published literal V1 set |
| Release sanitizer | 10 × 100,000 | Exact | 1,000 | Published release V1 set |

The PR, nightly, release, and sanitizer tiers cover each required profile once. Main covers each
profile twice. PR seeds are fixed. Main and nightly seeds are derived from an explicitly serialized
unsigned 64-bit epoch with a domain-separated SHA-256 rule. Release seeds are published literals;
the sanitizer tier uses the same seeds on a smaller exact workload. Rotating tiers reject an
omitted epoch rather than consulting the clock.

Canonical checked campaign documents record the complete resolved suite and seed provenance.
Generated command streams are not checked into the repository because each stream can be
regenerated and verified from its small manifest and digest.

The manual Release workflow requires full GCC and Clang Release builds and CTest runs before its
compiler-by-case campaign shards start. The sanitizer subset is independently sharded per case.
Sharding changes execution capacity, not the published seeds, profiles, or command counts.

### Streaming comparison

For each case, the runner completes the Python reference pass first and atomically closes a
versioned JSONL evidence spool. Only then does it spawn the native process. This ordering prevents
native execution from becoming transition logic for the reference model.

Native output is decoded incrementally through an eight-record queue with an 8 MiB maximum JSONL
record. The runner compares each result as it arrives and retains only bounded terminal state plus
rolling digests unless a failure requires an artifact. Exact mode compares classification,
sequence, full normalized events, event digest, state summaries, state digest, and checkpoint
snapshots. Compact mode compares the same evidence except full event payloads. Every semantic
divergence is rerun from a fresh reference and native engine in exact mode. A command divergence
replays through that command and takes one full snapshot there; a terminal divergence replays the
complete workload and compares terminal state. The case deadline covers manifest verification,
reference execution, native execution, and required exact replay preparation.

Fresh exact replay is the default for every semantic divergence. Capacity-bound Release and
sanitizer shards set `--exact-replay-command-limit 1000000`. If a required prefix exceeds that
limit, the case remains a semantic failure with `diagnosis.status=deferred_command_limit`.
The runner retains the portable workload, manifest, first difference, and digests, deliberately
omits giant diagnostic transcripts, and requires manual exact reproduction on a suitable host.
Deferred diagnosis is never accepted as passing release evidence.

The runner stops at the first value difference, protocol failure, process failure, or harness
failure. A value difference records a stable category and field path. State divergences also
record the smallest relevant side, level, order, and field difference when available.

### Failure artifacts and reduction

A semantic divergence is persisted before reduction. Every versioned bundle contains:

- the original canonical `.atlas` workload, plus its manifest and source provenance when the
  failure came from a generated campaign;
- command and evidence digests plus classification counts;
- the first differing field, expected and actual values, state digests, top of book, and nearest
  depth difference;
- the failing command index and a bounded recent-command window;
- portable revision, compiler, build-type, Python, and platform metadata; and
- an exact CLI reproduction command using paths relative to the bundle.

Standard diagnostic bundles additionally retain both reference and native evidence streams.
Summary-only large-tier bundles may omit those streams, including when exact replay is deferred at
the command limit.

Reduction always runs fresh reference and native processes for an uncached candidate and preserves
one exact semantic failure signature. Automatic reduction is limited to workloads of at most
100,000 commands; larger failures retain the complete reproducible bundle without materializing the
workload in memory. The deterministic pipeline performs chunk deletion,
individual deletion, identifier remapping, global and per-command price reduction, quantity
reduction toward one and observed boundaries, command simplification, side/TIF normalization,
another field-reduction pass, and a final individual-deletion pass. Candidate results are cached.
Evaluation and end-to-end elapsed-time budgets cover candidate construction, hashing, execution,
and verification. Budget exhaustion retains the smallest verified reproducer found so far and is
never described as proof of global minimality.

The minimized fixture is rerun once more before publication. Its report must reproduce the original
signature, update the artifact digests, and contain an exact relative-path reproduction command.

### Injected faults, metamorphic checks, and fuzzing

Three development-only evidence-boundary faults prove that detection and shrinking work:

- newest rather than oldest resting identity at a price;
- incoming rather than resting execution price; and
- stale aggregate quantity after a partial fill.

The injectors transform a copied native evidence mapping only after the real engine and strict
native decoder have completed. They are not compiled into the engine, do not alter native
transitions, and cannot be enabled by the production API. Their checked minimized fixtures are
evidence about the differential harness, not known engine defects.

Applicable Phase 3 metamorphic checks cover fresh-engine deterministic replay, side/price mirror
symmetry, split market-order consumption, a far nonmarketable level, and a rejected prefix followed
by the same valid command. Each property states the permitted sequence or event-grouping
difference.

Bounded Hypothesis fuzzing interprets byte streams as command sequences and separately applies one
field mutation to a valid generated sequence. Both paths compare the independent reference and
native engine. The checked seed corpus includes minimal, golden mixed, boundary, and prior
regression bytes. A fuzz failure must be reproducible as a normal workload and may use the same
failure bundle and semantic shrinker.

Snapshot/restore continuation and independent multi-instrument reordering depend on Phase 4.
Protocol encode/decode identity and decoder fuzzing depend on Phase 6. Corrupt log and snapshot
deserializer fuzzing remains deferred until those persistence formats exist.

### Retention and claims

Passing PR evidence is retained by hosted CI for 14 days. Main and nightly artifacts are retained
for 30 days. Published release and sanitizer evidence is retained for 90 days. A failing case
retains its portable bundle and minimized reproducer; successful command/evidence streams are
discarded by default after their manifest, summary, and digests have been recorded.

No latency, throughput, allocation, memory, scalability, or production-readiness claim may be
derived from campaign duration or command count. Performance measurement remains Phase 5.

## Alternatives considered

- Python's `random` module and floating-point distributions were rejected because the exact
  workload identity should not depend on runtime implementation details.
- Generating expected outputs from the lifecycle shadow was rejected because it would create a
  third, partially hidden oracle and weaken the independent comparison.
- Interleaving reference and native transitions was rejected because native state could
  accidentally influence expected output.
- Buffering complete million-command transcripts in memory was rejected in favor of disk spooling,
  rolling digests, and incremental native decoding.
- Keeping only a seed was rejected because defaults, generator versions, campaign epochs, and
  engine policy are required to reproduce a stream exactly.
- Unstructured delta debugging was rejected because a smaller sequence that triggers a different
  failure is not a valid minimization of the original divergence.

## Consequences

Phase 3 can reproduce a generated command stream from explicit provenance, compare it with bounded
memory, stop at the first divergence, retain enough evidence for independent review, and reduce a
failure without changing its semantic identity.

The cost is a larger test-only Python surface, checked campaign schemas, duplicated evidence
serialization, and scheduled work that can consume substantial CI time. Campaign policy changes
therefore require a version change and review rather than silently changing named tiers.

## Evidence

- Generator, manifest, campaign-policy, runner, reporting, shrinker, injected-fault, metamorphic,
  and bounded-fuzz tests.
- Checked campaign JSON documents and fuzz/regression corpora under `python/`.
- Local default Python suite: 244 passed, 2 Windows-symlink skips, 11 deselected.
- Local marked campaign/fuzz suite: 11 passed, 246 deselected.
- Local fixed PR campaign: 10 cases × 5,000 commands passed in exact mode.
- Checked epoch-0 nightly case 0: 1,000,000 compact commands passed.
- Local GCC Debug and Release CTest: 288/288 in each configuration.
- Local `BUILD_TESTING=OFF` production build, pinned clang-format, Ruff, strict mypy, and wheel
  build/install smoke gates passed.

Published Phase 3 PR #5 head `29049756` passed the required hosted GCC, Clang, Release,
ASan/UBSan, Python 3.11-3.14, formatting, wheel, PR-corpus, and Linux link-safety checks. ADR 0011
is fully evidenced and Phase 3 is complete. PR #5 remains open because GitHub authentication is
unavailable in this session; the local Phase 4 router slice is implemented and under validation.
