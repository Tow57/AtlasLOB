# Differential testing interface

Phase 3 compares the public C++ matching engine with an independent Python implementation. The
native side of that comparison is the test-only `atlas_diff_native` executable. It is available
only when `BUILD_TESTING=ON`, is not installed, and is not a production protocol.

Phase 4 PR1 adds a separate multi-instrument reference and generation family without changing the
V1 adapter, generator, campaign, failure, or retention contracts documented below. That local PR1
implementation has passed its available local C++ and Python validation; hosted validation and
remote integration remain pending.

The design boundary and independence rules are recorded in
[ADR 0010](decisions/0010-independent-python-oracle-boundary.md). Deterministic workload,
campaign, retention, and reduction policy is recorded in
[ADR 0011](decisions/0011-deterministic-differential-campaigns.md). Multi-instrument routing,
normalization, and compatibility are recorded in
[ADR 0012](decisions/0012-multi-instrument-routing-and-global-sequencing.md).

## Input: `ATLAS_DIFF_V1`

Input is case-sensitive, whitespace-delimited text. Blank lines and lines whose first
non-whitespace character is `#` are ignored. The first non-ignored line is:

```text
ATLAS_DIFF_V1 <instrument_u32> <max_quantity_u64> <tick_increment_i64> <max_active_u64> <snapshot_interval_u64>
```

The instrument, maximum quantity, and tick increment must be valid nonzero engine values.
`max_active_u64` may be zero and must fit the native `size_t`. A snapshot interval of zero
disables per-command snapshots; the final record always contains one.

Commands use raw numeric enum representations:

```text
N <client_u32> <order_u64> <instrument_u32> <side_u8> <type_u8> <tif_u8> <price_present_0_or_1> <price_i64> <quantity_u64>
C <client_u32> <order_u64> <instrument_u32>
R <client_u32> <old_u64> <new_u64> <instrument_u32> <price_i64> <quantity_u64>
```

An absent New price uses `price_present=0` and a required zero placeholder. Unknown enum values
inside their underlying `u8` representation are submitted to domain validation. Token, width,
conversion, header, and absent-price-placeholder failures are adapter errors and are never
submitted, so they consume no engine sequence.

## Output: `atlas_diff_v1` JSON Lines

The executable writes exactly one JSON object per line and no diagnostic text to stdout. Domain
values that are signed or unsigned 64-bit integers are decimal JSON strings. Client and
instrument IDs are also strings for a uniform lossless decoder. Semantic versions and event
indices are JSON numbers; flags are JSON booleans; absent values are JSON `null`. Per-side total
aggregate quantities are also canonical decimal strings, but are not restricted to `u64`: the
sum of multiple individually valid order quantities can exceed that range. The adapter computes
these totals without fixed-width overflow, and the strict decoder accepts the complete
mathematical engine domain through `u64_max * u64_max`.

The first successful record has `kind="config"` and repeats the accepted policy plus
`semantics_version` and `mode`.

Every submitted command produces one `kind="result"` record containing:

- zero-based `command_index` and physical source `line`;
- `command_type`, `outcome`, authoritative `command_sequence`, explicit `reject_reason`, and any
  `engine_error`;
- the canonical `event_digest`;
- complete ordered `events` in exact mode, or `null` in compact mode;
- current public observers and `state_digest` under `state`;
- bid and ask `level_count`, `order_count`, and total `aggregate_quantity` summaries under
  `state`; and
- a complete canonical `snapshot` at the configured interval, otherwise `null`.

The concrete side-summary fields are `bid_level_count`, `bid_order_count`,
`bid_aggregate_quantity`, `ask_level_count`, `ask_order_count`, and
`ask_aggregate_quantity`. Counts are canonical unsigned decimal strings in the `u64` domain.
Aggregate totals use the wider exact decimal representation described above. All summaries are
derived from one public snapshot captured for the state record. The adapter cross-checks the
separate active-count, empty, sequence, and top-of-book observers against that snapshot before it
serializes the record.

Every normal stream ends with `kind="final"`, `commands_processed`, current observers, and a full
snapshot. The final snapshot is present even when the interval is zero.

An adapter failure produces `kind="error"` with the physical line and a closed-vocabulary error
code, then stops. A returned native engine error is represented in its command result and is
followed by a final record. A thrown engine/resource exception is a process failure rather than a
synthesized domain or engine result.

## Modes and exit codes

Run:

```sh
atlas_diff_native exact
atlas_diff_native compact
```

`--mode exact`, `--mode=exact`, and the corresponding compact forms are also accepted. Exact is
the default.

| Exit | Meaning |
| --- | --- |
| `0` | The complete stream was processed, including any ordinary sequenced rejections. |
| `2` | Input/header/record syntax failed before domain submission. |
| `3` | The engine, adapter process, resource boundary, or output stream failed. |

Compact output retains per-command classification and rejection reason, sequence, event digest,
public observers, and state digest. A compact mismatch must be rerun through the first divergent
command in exact mode; compact mode is not sufficient evidence for diagnosing event-payload
differences by itself.

## Comparison obligations

The master specification's verification levels map to concrete evidence as follows:

- **L0:** compare committed/rejected/engine-error classification and the explicit rejection reason
  on every command in both exact and compact modes.
- **L1:** compare the complete normalized event sequence in exact mode. Compact campaigns compare
  its canonical digest on every command and must rerun a divergent prefix in exact mode.
- **L2:** compare top of book, active-order count, and bid/ask aggregate quantities after every
  command. The stream also compares per-side level/order counts and sequence observers.
- **L3:** compare the complete canonical book snapshot at the configured checkpoint cadence and
  in the terminal record.
- **L4:** rerun from an empty reference engine and a fresh native process through the first
  divergent command, with exact events, a full snapshot at the divergence (or terminal state for
  a final-record mismatch), and compare the event and state digests again.

Accordingly, for each submitted command, the Python differential runner compares:

- commit/reject/error classification, rejection reason, and command sequence;
- every event header, variant, payload, and event index in exact mode;
- event digest;
- active count, per-side level/order counts and aggregate quantities, emptiness, next sequence,
  exhaustion, and top of book;
- state digest; and
- every canonical snapshot field at checkpoints and at the end.

The Python reference engine must produce and durably close its complete evidence stream before the
runner invokes the native adapter. It may not import C++ headers, private helpers, bindings, or
native transition logic. Matching agreement is supplemented by hand-derived named scenarios and
independently encoded ADR 0009 hash vectors.

The decoder also binds the transcript to the submitted mode, configuration, instrument, command
types, source lines, checkpoint cadence, contiguous command sequences, process exit, and terminal
state. Internally consistent but unrelated records are protocol failures. Exact event envelopes
and snapshot values are revalidated before they may be used as correctness evidence. Side order
counts must sum to the active count, empty and nonempty side summaries must agree with their top
observers, total quantities must fit the configured order-count/quantity envelope, and checkpoint
summaries are recomputed from every level and order in the canonical snapshot.

## Phase 4 multi-instrument evidence V2

The local router slice adds a standard-library-only `ReferenceRouter`. It owns one independent
`ReferenceEngine` per sorted catalog entry and separately implements the global sequence,
active-order directory, routing precedence, and projected engine-wide capacity. It does not call
the C++ router or private transition helpers.

V2 is a separate family rather than an in-place change to V1:

| Surface | Version or schema |
| --- | --- |
| Generator | `MULTI_GENERATOR_VERSION = 2` |
| Resolved specification | `atlas_workload_spec_v2` |
| Canonical stream header | `atlas_workload_stream_v2` |
| Canonical command record | `atlas_workload_command_v2` |
| Workload manifest | `atlas_workload_manifest_v2` |
| Reference capture | `atlas_differential_capture_v2` |
| Multi-engine state digest | `ATLSME01` |

The resolved V2 specification contains one or more uniquely routed per-instrument V1 workload
specifications plus one maximum-total-active-orders policy. The canonical stream requires sorted
catalog entries, records every command's instrument, and binds the complete stream and statistics
with SHA-256. Numeric values retain the same lossless canonical decimal conventions used by V1.

Same-stream comparison requires exact classification, rejection reason, global sequence, normalized
events, complete multi-engine snapshot, and `ATLSME01` state digest. The digest includes semantic
version 6, engine capacity, sorted catalog/policies, global sequence state, total active count, and
every sorted instrument's best-price/FIFO state. Its independent C++ and Python encoders use the
layout frozen by ADR 0012.

The independent-instrument reinterleaving property is intentionally narrower. It requires only
configured instruments, disjoint per-instrument order IDs, and nonbinding engine-wide capacity,
then preserves command order within each instrument while changing cross-instrument interleaving.
Absolute global command and priority sequences are normalized because they are expected to change.
Per-instrument classifications, event structure, economic state, and FIFO order must remain equal.

The non-installed `atlas_diff_multi_native` target provides the strict native V2 process boundary.
Its LF-delimited input begins with:

```text
ATLAS_DIFF_V2 <max_total_active_u64> <catalog_count_u64> <command_count_u64> <checkpoint_interval_u64>
I <instrument_u32> <max_quantity_u64> <tick_increment_i64> <max_active_u64>
```

Exactly `catalog_count` instrument records and `command_count` numeric New/Cancel/Replace records
follow. Command records use the V1 numeric field layout. The adapter requires canonical single
spaces and LF endings, rejects duplicate instrument IDs and trailing input, and parses the complete
catalog and command stream before constructing or submitting to the engine. A malformed adapter
stream therefore consumes no sequence. Catalog order on input is not semantic; the accepted
catalog and all snapshots are emitted in sorted instrument-ID order.

Output is `atlas_diff_v2` JSON Lines:

- `config` binds mode, semantic version, engine capacity, sorted catalog, command count, and
  checkpoint interval;
- each `result` binds the source line, command type, outcome/error, authoritative global sequence,
  rejection reason, event digest, post-state digest, exact events or compact `null`, and an
  optional complete checkpoint snapshot;
- `final` includes declared/processed and outcome counts, final `ATLSME01`, and the complete engine
  snapshot; and
- `error` reports the physical line and one closed-vocabulary adapter code.

Exact and compact mode spelling and exit meanings remain aligned with V1: zero for a completely
processed stream, two for adapter input failure, and three for engine/resource/output failure.
The Python `atlaslob.multi_native` module encodes the requested input, invokes the selected V2
target, and strictly decodes the closed record schema. It binds output to the requested catalog,
mode, commands, checkpoint cadence, result positions, and process exit; revalidates event/state
digests and snapshot structure; and compares exact records with `MultiDifferentialCapture`.

The V2 target has direct C++ integration coverage and the strict Python process boundary has focused
parity and malformed-transcript tests. The local gate includes a deterministic generated
cross-language V2 campaign and exact/compact named-fixture parity.

Generator V1, `ATLAS_DIFF_V1`, Phase 3 campaign sizes/seeds, failure signatures, and retained
corpora remain unchanged.

## Runner executable selection

With `ATLAS_DIFF_NATIVE` unset, local Python tests search the normal MinGW/Windows and
Linux `build/dev-gcc` development locations. A complete evidence run fails when no adapter has
been built; unit-only modules can still be selected and run independently.

When `ATLAS_DIFF_NATIVE` is set, its value is authoritative. It must name an existing executable;
the suite fails instead of falling back to another build or skipping parity. Hosted evidence jobs
always set this variable after building the selected adapter.

## Deterministic workloads

Generator V1 accepts one explicit unsigned 64-bit seed and one fully resolved
`atlas_workload_spec_v1`. It uses frozen integer-only SplitMix64 sampling, so command identity does
not depend on Python's `random` module, floating-point sampling, the wall clock, process state, or
platform entropy.

The resolved specification serializes all engine and distribution parameters:

- command count, instrument, engine quantity/tick/capacity policy, and snapshot cadence;
- named profile and price model;
- New, Cancel, and Replace weights;
- invalid, aggressive, market, and boundary-quantity basis points; and
- midpoint, price span, active-order target, and client count.

The ten profile names are `uniform_synthetic`, `clustered_mid`, `hot_level_contention`,
`sparse_wide`, `cancel_heavy`, `sweep_heavy`, `replace_heavy`, `invalid_mix`,
`trace_driven_synthetic`, and `adversarial_boundary`. Profile factory defaults are convenience
inputs only. A persisted spec contains the resolved values and never asks a later generator version
to rediscover them.

The generator's private shadow tracks active identities, sides, prices, quantities, and FIFO only
to construct meaningful valid and invalid commands. It never produces expected classification,
sequence, events, snapshots, state summaries, or digests. Comparison always uses a fresh
`ReferenceEngine` and a separately executed public native engine.

An `atlas_workload_manifest_v1` binds generator version, seed, resolved specification, canonical
command-stream SHA-256, command count, and intent statistics. The `.atlas` workload repeats the
native input policy and contains canonical commands. Opening a persisted workload verifies the
manifest, stream digest, canonical spelling, and exact count before it can become evidence.

## Campaign tiers and seed provenance

Campaign policy V1 has no implicit date or default epoch:

| Tier | Cases × commands | Mode | Snapshot cadence | Seed policy |
| --- | ---: | --- | ---: | --- |
| PR | 10 × 5,000 | Exact | 100 | Fixed literal V1 set |
| Main | 20 × 100,000 | Exact | 1,000 | Explicit rotating epoch |
| Nightly | 10 × 1,000,000 | Compact | 10,000 | Explicit rotating epoch |
| Release | 10 × 10,000,000 | Compact | 50,000 | Published literal V1 set |
| Release sanitizer | 10 × 100,000 | Exact | 1,000 | Published release V1 set |

PR, nightly, release, and sanitizer map one case to each required profile. Main maps two cases to
each profile. Main and nightly derive seeds from an explicit unsigned 64-bit epoch using the
domain-separated string `atlaslob:phase3:v1:<tier>:<epoch>:<index>` and the first eight bytes of
SHA-256. Omitting an epoch is an error. Release and sanitizer use one checked published seed set;
the sanitizer tier is an exact, smaller subset of the release workload policy.

Canonical checked policies live under `python/campaigns/v1`. They serialize the campaign policy
version, provenance kind, seed-set ID, derivation algorithm, epoch or literal seeds, ordered case
names, complete resolved specifications, comparison mode, and snapshot cadence.

The manual Release workflow first requires complete GCC and Clang Release builds and CTest runs,
then shards the published campaign by compiler and case. The sanitizer subset is sharded per case.
This keeps each hosted job inside its time and scratch-space boundary without changing the
published seed/profile coverage.

## Streaming runner

The runner performs two non-overlapping passes:

1. It regenerates or reads the canonical workload, runs every command through a fresh Python
   reference engine, and atomically closes `atlas_reference_evidence_v1` JSONL.
2. It starts the native adapter with the same persisted input, incrementally decodes one native
   JSONL record at a time, and compares it with the next checked reference record.

Reference evidence and command digests are accumulated while spooling. Native evidence uses the
same canonical mapping and rolling digest. A bounded line-pump queue prevents a silent pipe
deadlock without retaining a command-sized transcript in memory. Passing native output is deleted
by default. Standard diagnostic runs atomically retain it on divergence or harness failure;
summary-only large-tier runs may omit it.

Exact mode compares every event value. Compact mode retains event digests and the complete state
summary but omits event payloads between checkpoints. Every semantic divergence is replayed from
fresh reference and native engines. A command divergence is rerun from command zero through the
divergent command in exact mode with one full checkpoint there. A terminal-record mismatch replays
the complete workload and compares the final snapshot. Diagnosis and reduction use that fresh exact
replay result.

Fresh exact replay is the default for every semantic divergence. Capacity-bound Release and
sanitizer shards explicitly pass `--exact-replay-command-limit 1000000`. If the required prefix is
larger, the command still exits as a semantic failure and records
`diagnosis.status=deferred_command_limit`; it retains the portable workload, manifest, first
difference, and digests, intentionally omits giant diagnostic transcripts, and requires manual
exact reproduction on a suitable host. A deferred diagnosis never counts as passing release
evidence.

The native line pump has a queue capacity of eight records and rejects any JSONL record larger than
8 MiB. One end-to-end case deadline covers manifest verification, reference generation, native
execution, and required replay preparation. Before writing, the runner resolves all input, output,
temporary, executable, and manifest paths and rejects collisions; a failed capture removes partial
artifacts. Reusing a CLI output directory preflights the complete cleanup plan, rejects
symlink/junction redirects, removes only the runner-owned files and empty directories from the
previous run, and preserves unrelated manual content. Summary-only campaign jobs discard passing
workload and evidence streams after their verified manifests and summaries have been written.

The public CLI entry point is `atlaslob-diff`, equivalently `python -m atlaslob.cli`:

```sh
python -m atlaslob.cli fixture \
  --workload path/to/case.atlas \
  --mode exact \
  --native path/to/atlas_diff_native \
  --output path/to/evidence

python -m atlaslob.cli predefined \
  --tier pr \
  --native path/to/atlas_diff_native \
  --output path/to/pr-evidence
```

Rotating tiers additionally require `--epoch <u64>`. Exit zero means the complete comparison
passed, exit one means a semantic divergence was persisted, and exit two means campaign,
process-boundary, or harness failure.

## Failure bundles and semantic shrinking

`atlas_failure_report_v1` is written before reduction. Every bundle contains the original workload,
artifact digests, classification counts, the first differing field and values, reference/native
state digests and tops, the smallest available depth difference, recent commands, build/runtime
identity, and a relative-path exact reproduction command. Campaign-generated failures also retain
their manifest and source provenance; an ad hoc fixture may not have a separate manifest. Standard
diagnostic bundles retain both reference and native evidence streams. Summary-only large-tier
bundles may omit those streams, including when diagnosis is deferred at the command limit.

One stable `FailureSignature` identifies the category, field path, and expected/actual value kinds.
The shrinker accepts a candidate only when fresh reference/native processes reproduce that exact
signature. Its deterministic passes are:

1. contiguous chunk deletion;
2. individual command deletion;
3. client/order identifier remapping;
4. global and per-command price reduction;
5. quantity reduction toward one and observed boundaries;
6. command-type simplification;
7. side and time-in-force normalization;
8. a second price/quantity reduction; and
9. a final individual-deletion pass.

Candidate results are cached by SHA-256 rather than retained command tuples. Evaluation-count and
end-to-end elapsed-time budgets are explicit report fields and cover candidate construction,
hashing, execution, and verification. Automatic shrinking is capped at 100,000 input commands;
larger failures retain the complete bundle and report that reduction was skipped. Budget exhaustion
preserves a verified smaller reproducer but does not claim global minimality.
The final `minimized.atlas` must be rerun once more and reproduce the original signature before the
report is finalized.

Three development-only evidence transforms prove the diagnosis path: newest-at-price identity,
incoming rather than resting trade price, and a stale partial-fill aggregate. They modify a copied
native evidence mapping after real native execution and strict decoding. They are unavailable to
engine code and are not latent engine fault switches. Their checked minimized fixtures live under
`python/corpus/regressions/phase3-injected`.

## Metamorphic and fuzz scope

Phase 3 checks five applicable metamorphic relations with explicit preconditions:

- fresh reference and native engines replay the same workload deterministically;
- side and price mirroring preserves quantities, identities, and FIFO under the mirrored price
  transform;
- splitting one market order into two with the same total quantity consumes the same resting
  sequence and reaches the same book, while event grouping and command sequences may differ;
- adding a far nonmarketable level does not change bounded incoming fills; and
- a rejected prefix changes audit sequencing but not the later valid command's book outcome.

Bounded Hypothesis tests interpret arbitrary bytes as a capped command sequence and separately
mutate one field of a valid generated sequence. Both execute a fresh reference/native comparison.
The fixed seed corpus under `python/corpus/fuzz/v1` includes minimal, golden mixed, boundary, and
prior-regression inputs.

Some useful properties depend on infrastructure outside Phase 3:

- snapshot/restore continuation remains deferred to a later Phase 4 persistence slice;
- independent multi-instrument reordering is implemented locally in the Phase 4 router slice and
  remains subject to that PR's final validation;
- protocol encode/decode identity and incremental decoder fuzzing are deferred to Phase 6; and
- corrupt command-log and snapshot-deserializer fuzzing is deferred until those persistence
  formats exist.

These dependency deferrals do not defer fresh-engine exact-prefix replay, which is part of the
current runner.

## Retention and current evidence

Hosted workflow policy retains passing PR evidence for 14 days, main and nightly evidence for 30
days, and published release/sanitizer evidence for 90 days. A failing case retains its original and
minimized portable bundle. Successful raw streams are discarded by default after the campaign
summary, manifests, classification counts, and rolling digests are recorded.

On 2026-07-24, the local default Python selection passed 244 tests with 2 Windows-symlink skips and
11 deselected, the marked campaign/fuzz selection passed 11 with 246 deselected, and the fixed PR
policy passed all 10 cases and 50,000 exact commands. Checked evidence records a passing epoch-0
nightly case with 1,000,000 compact commands. GCC Debug and Release CTest each passed 288/288; the
`BUILD_TESTING=OFF` production build, pinned clang-format, Ruff, strict mypy, and wheel
build/install smoke gates also passed locally. Hosted CI subsequently passed GCC, Clang, Release
GCC/Clang, ASan/UBSan, Python 3.11-3.14, the fixed PR corpus and marked proofs, wheel smoke,
formatting, and Linux link-safety checks. Published PR #5 head `29049756` therefore closes the
Phase 3 correctness-evidence gate. It remains open rather than remotely merged because GitHub
authentication is unavailable in this session. The local Phase 4 router slice described above
passes its available local Debug/Release, formatting, Python, stress, and cross-language gates;
hosted validation and remote integration remain pending. Later Phase 4 persistence and binding work
has not started. See
[the Phase 3 evidence index](evidence/phase3/README.md) for the complete record.

Command counts and elapsed campaign time are correctness-test metadata. They make no latency,
throughput, allocation, memory, scalability, or production-readiness claim.
