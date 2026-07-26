# Phase 5 baseline-campaign journal

Date: 2026-07-25

## Objective

Make the frozen Phase 5 study executable as one strict, resumable campaign before access to the
authoritative bare-metal Ubuntu host. Preserve all matching, persistence, Python engine, and
evidence schemas and publish no exploratory timing result.

## Implementation boundary

- `run-campaign` expands a checked plan into deterministic point, boundary, and Python batch
  shapes; the Phase 5 study resolves to 51 shapes and 510 required valid processes.
- All selected manifests and runner identities are preflighted before the first process.
- Attempts run round-robin, use stable directories, and are never overwritten.
- Every retained attempt must have the canonical counter-derived run label and a unique document
  digest, so copied or stale evidence cannot satisfy an observation target.
- The first invalid attempt is retained and stops the campaign. An explicit resume starts the next
  attempt, leaving the failure visible.
- `finalize-campaign` requires complete coverage and ten valid observations per shape by default.
  It verifies one physical host while preserving the distinct frozen build/runtime contexts used
  by core, allocation, and CPython reports.
- Report generation is staged before publication. A failed inventory or bundle verification removes
  only derived outputs, leaves raw evidence intact, and permits a corrected retry.
- `capture-profile` runs the exact public benchmark boundary beneath fixed `perf stat` or
  `perf record` prefixes, then validates the runner's canonical observation and produces a
  counter-availability summary.
- Profiling data never becomes core timing evidence, and inaccessible counters never authorize a
  kernel security change.

## Current evidence status

The automation and threshold-free hosted smoke are Stage 1 infrastructure. The native Ubuntu
baseline, counter data, call graphs, hotspot ranking, experiments, and public performance numbers
remain absent. The draft PR must not merge as a completed baseline until the
[native-host runbook](../phase5-native-host-runbook.md) succeeds on the dedicated machine.

## Comprehension checkpoint

The campaign separates three ideas that should remain distinct in review:

1. A physical host is the hardware/OS/affinity environment that must agree across every boundary.
2. `host_context_sha256` also binds build and runtime facts, so core, allocation, and CPython
   evidence regenerate through separate reports.
3. `perf` counter and call-graph captures explain CPU behavior but are not reused as the
   uninstrumented throughput or latency observations.
