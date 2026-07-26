# Performance methodology

AtlasLOB performance work follows one order: freeze the question and input, measure the unchanged
engine, profile it, form a falsifiable hypothesis, change one variable, rerun correctness, and then
repeat the same measurement. A change that does not clear its declared gate is removed while its
result remains part of the study.

This document defines the Phase 5 measurement boundary. It does not contain benchmark results.
Those results cannot be published until they have been captured from the exact release tag on the
authoritative native Linux host.

The four canonical evidence schemas and their cross-file hash bindings are documented in
[Benchmark evidence format](benchmark-evidence-format.md). A smoke-to-bundle walkthrough is in
[Benchmark reproduction guide](benchmark-reproduction.md).

## Result boundaries

| Boundary | Included | Excluded |
| --- | --- | --- |
| C++ core throughput | Public `MultiInstrumentEngine::execute`, owned result/event construction and destruction | Generation, parsing, preload, warm-up, hashing, persistence, Python, output |
| C++ core latency | One sampled public command and owned result, measured with `steady_clock` | Batch-rate inference, network delay, fixture work, report work |
| Replay fast | Log decoding, structural checks, and engine application required by fast mode | Log creation and unrelated filesystem writes |
| Replay verify | Fast-mode work plus expected classification/event verification | Log creation |
| Python batch | Python-to-C++ conversion, native execution, selected output materialization, final batch digest | Fixture generation |
| Steady allocation | Requested global C++ allocation/deallocation behavior while executing the measured command region | Construction, preload, warm-up, and any latency or throughput claim |
| Construction | Engine construction for the manifest's catalog and capacities | Stream parsing, command execution, and digest validation |
| Preload | Publication of the manifest's preload prefix into an already constructed empty engine | Engine construction, parsing, and post-run validation |
| Setup allocation | Requested allocations for engine construction plus the complete preload prefix | Timing evidence and steady-state allocation claims |
| Process memory | Recorded Linux RSS/HWM for the named scenario | A component-only size estimate |

The future gateway-to-event and wire round-trip boundaries are Phase 6 work.

## Workload catalog

`ATLAS_BENCH_WORKLOAD_V1` covers W01-W10 and W12. W11 remains reserved for the gateway.

| ID | Name | Primary question |
| --- | --- | --- |
| W01 | Resting inserts | What do price-level insertion and active-state growth cost? |
| W02 | Hot-level FIFO | What do FIFO append and indexed cancellation cost at long hot queues? |
| W03 | Wide sparse | How does ordered price-level lookup behave over a wide range? |
| W04 | Balanced market | What does the declared mixed command distribution cost? |
| W05 | Crossing sweep | How does work scale with consumed levels and emitted events? |
| W06 | Cancel storm | What do lookup, unlink, and empty-level removal cost? |
| W07 | Replace storm | What does cancel-and-new priority reset cost? |
| W08 | IOC flow | What do non-resting residual and terminal paths cost? |
| W09 | Multi-instrument | How do routing and working-set size scale with catalog size? |
| W10 | Replay | What do fast and verified deterministic replay cost? |
| W11 | Gateway fragmented | Deferred until Phase 6 |
| W12 | Adversarial legal | Where do legal churn and boundary patterns expose cliffs? |

Every manifest names its generator, seed, catalog, preload, warm-up, measured count, command-stream
digest, and expected evidence. Small fixtures are retained in Git. Large streams are regenerated
outside the timed region.

## Required scales

| Evidence tier | Required shape |
| --- | --- |
| Shared-CI smoke | At most 10,000 commands per workload; one exploratory process per exercised boundary |
| Study | 64K active orders and 1M measured commands |
| Headline core | W04 and W05, up to 1M active orders and 5M measured commands |
| W09 routing | 1, 16, 256, and 4,096 instruments; 262,144 total active orders; 2M commands per point |
| W10 replay | Separate 1M- and 10M-record fast and verify points |
| Memory | 1K, 64K, and 1M resting-order shapes |
| Python W04 | Batch sizes 1, 64, 1,024, and 65,536 for objects, columns, and summary |

The optional 100M-record replay asset is not a completion gate. WSL2, Docker Desktop, and hosted
CI may exercise smaller exploratory points but cannot replace any official scale above.

## Timing and sampling

One scenario-runner process produces one observation. Official result points contain at least ten
valid process observations. Comparisons use five `A-B-B-A` blocks. The exact order is retained in
raw evidence.

One process observation has a 15-minute cap. A timeout remains in the raw bundle with its failure
reason. It is never hidden or replaced by a smaller favorable input.

Throughput uses one monotonic interval around the complete measured command block. It publishes
both input commands per second and normalized events per second. The final state digest is computed
and verified after timing.

Latency is a separate run. Every thirty-second command is sampled, beginning at the deterministic
offset encoded by the workload, into a preallocated vector capped at 200,000 entries. No sorting,
file output, or percentile calculation occurs while a command is timed. Samples are integer
nanoseconds from `std::chrono::steady_clock`.

Given sorted samples `x[0..n-1]`, the nearest-rank percentile `p` is:

```text
x[max(0, ceil(p * n) - 1)]
```

The report publishes minimum, maximum, p50, p90, p95, p99, p99.9, and sample count. It calls this
closed-loop core service time, not network or exchange latency.

## Statistics

No valid observation is discarded as an outlier. Invalid and timed-out observations remain in the
bundle with their reason and are excluded from aggregates.

For each official point, the report publishes:

- observation count;
- minimum and maximum;
- median;
- median absolute deviation;
- interquartile range; and
- exact raw-evidence hashes.

Rates are derived from raw integer command/event counts and elapsed nanoseconds. Raw files do not
store rounded floating-point rates as authoritative measurements.

An optimization is accepted only under the gate in ADR 0016. A report may still describe a
rejected or neutral result; the absence of an accepted optimization never authorizes choosing a
more favorable subset of runs.

## Environment

Official results require bare-metal Ubuntu 24.04 x86-64, a clean exact-tag checkout, an optimized
non-sanitized build with invariants and LTO disabled, a complete environment manifest, a known
binary digest, and affinity to one recorded logical CPU. Its SMT sibling must remain idle during
the suite.

The collector records but does not silently change governor, turbo, NUMA, SMT, or kernel security
configuration. A username, hostname, serial number, network address, or private filesystem path is
never evidence.

WSL, containers, virtual machines, and GitHub-hosted runners are explicitly exploratory.
Shared-host timing is never a merge threshold.

## Memory and allocation

The allocation runner interposes the replaceable global C++ allocation functions and reports
requested bytes and calls. Instrumentation overhead and altered allocation layout mean those runs
cannot support latency claims.

Process RSS/HWM is captured separately on Linux. Bytes-per-order results identify their preload
shape and whether they represent a process delta or requested-allocation count. Phase 5 measures
1K, 64K, and 1M resting-order shapes.

The native runner parses and verifies the `.atlas` stream before any RSS baseline. Boundary names
then determine what the process delta includes:

- `core_construction` captures immediately before and after engine construction.
- `core_preload` constructs the empty engine first, then captures the delta while publishing only
  the preload prefix.
- `core_setup_allocation` captures before construction and tracks requested allocations through
  construction plus preload; it is deliberately untimed.
- steady `core_throughput`, `core_latency`, and `core_allocation` capture the RSS baseline before
  construction and the final RSS after construction, preload, warm-up, and the measured region.
  The allocation counters themselves are enabled only for the measured region.

Linux `peak_rss_bytes` is the process-lifetime high-water mark and can include the parser,
stream storage, runtime, validation engine, and measured engine. It is reported separately and is
never described as engine-only memory.

For Python batch evidence, workload parsing, engine construction, preload/warm-up submission, and
creation of immutable measured batch slices occur before timing. The timed region begins before
each public `Engine.submit_batch` call and includes Python-to-C++ conversion, native execution,
the selected output materialization, result accounting, and result destruction. Correctness is
recomputed afterward with object-mode batches outside the timed region.

## Linux perf

Where permitted, the baseline captures:

```text
cycles
instructions
branches
branch-misses
cache-references
cache-misses
page-faults
context-switches
```

Sampling profiles retain call graphs from the same optimized code-generation policy as the
benchmark executable. Unavailable counters are recorded as limitations; reproduction never
requires weakening the host's security policy.

The retained baseline and every accepted optimization include:

```text
perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses,page-faults,context-switches
perf record -g --call-graph dwarf
perf report --stdio
```

W04 and W05 receive baseline call graphs. An accepted optimization also profiles the workload
that motivated it.

## Claims

A number is publishable only when it names the exact tag, boundary, workload and scale, host class,
compiler/build, number of independent processes, statistic, and raw bundle.

The benchmark catalog is synthetic. It does not predict a production exchange, compare AtlasLOB
with a trading firm, or establish universal latency, throughput, durability, security, or
scalability.
