# ADR 0016: Reproducible performance evidence and claim boundaries

- Status: accepted
- Date: 2026-07-25
- Implementation state: Phase 5 benchmark-contract infrastructure complete locally; native
  baseline evidence not started

## Context

Phases 0 through 4 establish deterministic matching, independent reference evidence,
multi-instrument routing, replay, recovery, and native Python batches. Their command counts and
elapsed test times are correctness metadata, not performance evidence. Phase 5 needs a separate
measurement system that can answer narrowly defined questions without weakening those correctness
contracts or treating noisy shared CI timing as a release gate.

AtlasLOB has several materially different performance boundaries:

- public C++ command execution through `MultiInstrumentEngine`;
- replay decoding and deterministic command application;
- Python command conversion, native batch execution, and result materialization;
- allocation and process-memory behavior; and
- a future gateway boundary that does not exist until Phase 6.

Combining those boundaries into one "orders per second" number would be misleading. Timing fixture
generation, parsing, state hashing, or filesystem synchronization while labeling the result as
matching-core cost would be equally misleading.

The authoritative development workstation currently has no general-purpose Linux distribution.
Windows, Docker Desktop, WSL2, and GitHub-hosted runners remain useful for compilation and smoke
tests, but they cannot provide the bare-metal CPU affinity and hardware-counter context required
for an official Phase 5 latency or `perf` claim.

## Decision

### Production and dependency boundary

Benchmark support is opt-in:

```text
ATLAS_BUILD_BENCHMARKS=OFF
```

The default library, command-line, test, and Python-package builds do not fetch or link a benchmark
framework. When benchmark support is enabled, CMake fetches Google Benchmark 1.9.5 at immutable
commit `192ef10025eb2c4cdd392bc502f0c852196baa48`. Its tests and installation rules are disabled,
and AtlasLOB warning or sanitizer settings apply only to AtlasLOB targets.

The benchmark layer must not change:

- semantic version 6;
- public engine, persistence, or Python APIs;
- normalized event ownership;
- matching, rejection, sequencing, FIFO, or active-ID behavior; or
- `ATLSST01`, `ATLSEV01`, `ATLSME01`, `ATLAS_DIFF_V1`, `ATLAS_DIFF_V2`, `ATLSLG01`, or
  `ATLSSN01`.

Benchmark executables and their instrumentation are never installed in the Python wheel.

The opt-in surface consists of:

```text
atlas_microbench
atlas_bench_runner
atlas_bench_alloc_runner

python -m atlaslob.performance materialize
python -m atlaslob.performance capture-environment
python -m atlaslob.performance run-suite
python -m atlaslob.performance analyze
python -m atlaslob.performance verify-bundle
```

The native scenario runners emit exactly one observation per process. The Python entry point
validates inputs, launches those independent processes, retains failures, and assembles evidence;
it does not move Python conversion or report work into a C++ core timing interval.

### Build profiles

The Phase 5 presets are optimized Release builds with `NDEBUG`, invariants and sanitizers disabled,
LTO explicitly disabled, debug symbols retained, and frame pointers preserved. GCC and Clang
presets allow compiler comparisons. The `release-profile` preset is code-generation-equivalent to
the Clang benchmark preset so a sampling profile does not silently describe a different program.

Every environment manifest binds the executable to a build receipt derived from `CMakeCache.txt`,
`compile_commands.json`, and the complete timed-target closure. It records normalized per-target
compile profiles, compiler identity and effective flags, Release-lock facts, binary SHA-256,
filesystem/storage class, and either native or complete installed-wheel CPython identity.

### Frozen workload family

`ATLAS_BENCH_WORKLOAD_V1` is separate from Generator V1/V2 and does not change their output. It
binds a benchmark ID to:

- a frozen generator and seed;
- a complete catalog and engine configuration;
- preload, warm-up, and measured command counts;
- active-order counts at the preload boundary, measured-region start, and final state;
- the canonical `ATLAS_DIFF_V2` command-stream SHA-256;
- expected empty, preload, and measured-region outcome/event/state evidence; and
- for W10 only, the exact `ATLSLG01` replay input, record count, SHA-256, and warm-page-cache
  policy.

Each W01-W10 and W12 workload has a small checked-in fixture. Large fixtures are generated into an
ignored output directory and verified before use. W11 is reserved for the Phase 6 gateway.

Parsing, generation, ordinary preload and warm-up, report serialization, validation replay, and
final digest calculation occur outside the timed core interval. The dedicated `core_preload` and
`core_setup_allocation` boundaries measure preload deliberately and label it as such.

CI fixtures contain no more than 10,000 commands per workload. Study points use 64K active orders
and one million measured commands. W04 and W05 additionally define headline points of up to one
million active orders and five million measured commands. W09 covers 1, 16, 256, and 4,096
instruments with 262,144 total active orders and two million measured commands per point. W10
covers one-million- and ten-million-record logs. Allocation and memory evidence covers 1K, 64K,
and one million resting orders. Python W04 covers batch sizes 1, 64, 1,024, and 65,536 for each
objects, columns, and summary output.

### Environment qualification

Official observations require one dedicated bare-metal Ubuntu 24.04 x86-64 host. The initial host
class is expected to describe the Ryzen 7 9800X3D and 64 GiB workstation without exposing a
hostname, username, serial number, address, or private path. The runner is pinned to one explicitly
recorded nonzero logical CPU while its SMT sibling remains idle.

The environment collector records rather than silently changes governor, turbo, SMT, NUMA, kernel
security, or frequency configuration. Missing optional counters are limitations, not permission to
weaken host security.

Virtualized, containerized, WSL, dirty-worktree, non-Release, sanitizer, invariant-enabled, or
incompletely described runs are labeled exploratory. GitHub-hosted CI never produces an official
observation and never enforces a wall-clock threshold.

### Measurement boundaries

Core throughput begins immediately before the first measured public C++ `execute` call and ends
immediately after the final returned `EngineResult` and owned event batch have been destroyed. It
reports input commands and normalized events separately. Fixture generation, command parsing,
preload, warm-up, event inspection, validation replay, state hashing, persistence, Python, and
output I/O are excluded. Process-RSS capture is a separate, explicitly broader envelope that
begins before engine construction for ordinary core runs; it is not relabeled as timed-region
allocation evidence.

Core latency is a separate closed-loop service-time study. It uses `std::chrono::steady_clock`, not
the timestamp counter. Every thirty-second command is sampled deterministically into a
preallocated buffer of at most 200,000 integer-nanosecond observations. Quantiles use the documented
nearest-rank rule.

Allocation evidence comes from a separate executable that interposes the complete replaceable
global `new`/`delete` surface, including aligned, sized, array, and nothrow forms. Those results
describe requested C++ allocation behavior and are never reused as timing evidence. Linux process
RSS is reported independently. `core_allocation` surrounds only measured execution after setup;
`core_setup_allocation` separately surrounds engine construction plus preload.

Engine construction and preload also have separately named timing boundaries. Replay fast and
verify modes are separate, warmed-page-cache boundaries bound to an exact W10 log digest. Python
`objects`, `columns`, and `summary` batch modes are separate boundaries and always name their
batch size. Python command parsing, prefix execution, final hashing, and independent result
validation stay outside timing; per-call conversion, native execution, result materialization,
and result destruction stay inside. Filesystem durability and future gateway measurements are
not core results.

### Raw schemas and analysis

Phase 5 freezes four canonical, versioned schemas:

```text
ATLAS_BENCH_WORKLOAD_V1
ATLAS_BENCH_ENV_V1
ATLAS_BENCH_OBSERVATION_V1
ATLAS_BENCH_REPORT_V1
```

Counts and nanoseconds are canonical decimal strings. Raw observations store measured integers;
rates, statistics, A/B comparisons, and experiment decisions are derived during analysis.
Observations bind the exact workload manifest, stream, timed input when applicable, environment,
host/build context, binary, suite, measurement shape, and A-B-B-A position. Decoders reject
duplicate keys, unknown versions, noncanonical integers, mixed contexts or suites, runtime
boundary mismatches, workload or binary mismatch, and evidence digest mismatch.

The exact field vocabulary, canonical ASCII JSON rules, cross-file bindings, bundle inventory,
and failure/exit behavior are specified in
[Benchmark evidence format](../benchmark-evidence-format.md).

One native runner or standalone Python worker process emits exactly one observation. A suite label
is a safe identifier of at most 32 characters and cannot be mixed within one report. Official
points use at least ten valid independent processes. Baseline/candidate experiments run in at
least five complete valid `A-B-B-A` blocks on the same CPU. No observation is silently discarded;
an invalid, interrupted, or timed-out observation is retained with its scheduled identity and a
reason. Invalid attempts are excluded from statistics, remain counted and named as limitations,
and do not by themselves invalidate ten or more independently valid observations.

One observation has a fifteen-minute cap. A baseline timeout is retained as invalid evidence
instead of being silently removed or replaced by a smaller favorable workload.

Reports publish median, minimum, maximum, median absolute deviation, and interquartile range.
Latency reports additionally publish sample count and nearest-rank p50, p90, p95, p99, and p99.9.
Analysis and deterministic SVG generation use the Python standard library so the installed package
retains no third-party runtime dependency.

### Profiling and optimization

The unchanged Phase 4 engine is measured before a production optimization is accepted. Linux
`perf stat` captures cycles, instructions, branches, branch misses, cache references, cache misses,
faults, and context switches where the host permits them. `perf record` uses call graphs for W04
and each workload supporting an accepted change.

Every experiment changes one major variable, retains raw before/after evidence, runs the full
correctness gate, and ends in one of:

```text
accepted
rejected
neutral
deferred
```

An accepted general change needs at least a five-percent median target improvement larger than
twice the larger relative MAD, no required control regression above five percent, and no peak-RSS
increase above ten percent. A finite-capacity reservation experiment may use up to twenty percent
more peak RSS only when its target throughput improves by at least ten percent. Correctness,
ownership, and public lifetime guarantees always take precedence.

Rejected code is removed before merge while its experiment record remains. If no change clears the
gate, Phase 5 publishes that result rather than weakening the rule.

### Release and claims

The first measured portfolio release is `v1.0.0`. The exact tag is rebuilt on the authoritative
host and produces a SHA-256-inventoried release bundle containing environment and workload
manifests, raw observations, profiles, reports, plots, experiment notes, limitations, and
reproduction commands.

Large raw data is attached to the GitHub release. A post-tag documentation-only change records the
permanent asset links and hashes so the measured commit is the exact release tag rather than a
self-referential later evidence commit. Wheels remain CPython-minor-specific for 3.11-3.14 on
manylinux x86-64. Phase 5 does not publish to PyPI.

Every public number names the boundary, workload, tag, host class, process count, statistic, and
evidence link. AtlasLOB does not claim production readiness, exchange comparability, universal
latency, security, durability, or scalability.

## Consequences

- Benchmark configuration and evidence add code, fixtures, and CI smoke time without becoming
  production dependencies.
- Large official suites require access to a suitable native Linux host and cannot be completed on
  the current Windows-only environment.
- Separate boundaries produce more result rows but prevent Python, replay, allocation, or
  persistence costs from being mislabeled as core matching latency.
- Retaining negative results makes the study auditable and prevents optimization theater.
- Frame pointers and disabled LTO favor inspectable first-release profiles over a best-number-only
  build.
- Exact-tag assets plus a post-tag evidence index avoid a circular claim about a commit containing
  measurements of itself.

## Verification plan

- Canonical-schema round trips plus duplicate-key, unknown-version, overflow, noncanonical,
  privacy, digest-mismatch, and mixed-environment failures.
- Small-fixture generation and exact command-stream/final-state evidence for W01-W10 and W12.
- Google Benchmark dry run plus core throughput, latency, allocation, and report smoke tests.
- Proof that parsing, validation replay, final hashing, and output occur outside timing; ordinary
  core runs also exclude setup/preload/warm-up, while the dedicated construction, preload, and
  setup-allocation boundaries include only the costs their names specify.
- Official-mode refusal for dirty, virtualized, non-Linux, non-Release, invariant, sanitizer, LTO,
  missing-affinity, and incomplete-environment cases.
- Deterministic statistics, quantiles, SVG, bundle hashes, and report regeneration from a tiny
  retained sample.
- Exact bundle-closure checks: every workload/environment is used, every observation is covered
  by exactly one report, every `.atlas`/`.atlslg`/report rendering/experiment note is referenced,
  W10 log commands and evidence match the source stream, and every file is inventoried without
  symbolic links or path escapes.
- Full Phase 0-4 compiler, sanitizer, differential, fuzz, replay/recovery, Python, packaging, and
  byte-fixture gates after every accepted optimization.

Copy-paste exploratory commands and the additional native-host controls are maintained in the
[Benchmark reproduction guide](../benchmark-reproduction.md).
