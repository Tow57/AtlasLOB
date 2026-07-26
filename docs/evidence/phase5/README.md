# Phase 5 evidence

Phase 5 is the measured portfolio-release milestone. The PR 1 benchmark contract is merged, and
the PR 2 branch implements baseline/profile campaign automation. No native Ubuntu baseline,
official observation, experiment result, or performance claim exists yet.

## Current state

| Evidence | Status |
| --- | --- |
| ADR 0016 measurement and claim contract | Complete locally |
| Frozen W01-W10/W12 recipes, small fixtures, and resolved plans | Complete locally |
| Workload, environment, observation, and report schemas | Complete locally |
| Core, setup, allocation, replay, and Python batch runners | Complete locally |
| Independent-process orchestration and A-B-B-A scheduling | Complete locally |
| Deterministic analysis, experiment decisions, Markdown/SVG, and bundle verification | Complete locally |
| Shared-CI benchmark smoke definition | Passed on PR #11 implementation head `35b09ba` |
| Resumable baseline and Linux `perf` campaign automation | Implemented on PR 2 branch |
| Native Ubuntu baseline and Linux `perf` profiles | Awaiting dedicated Ubuntu host |
| Hypothesis-driven experiments | Not started |
| Exact-tag `v1.0.0` evidence bundle | Not started |

W11 is intentionally reserved for the Phase 6 gateway. Existing Phase 3 campaign durations and
Phase 4 replay report metadata are correctness evidence and must not be copied into this phase as
benchmark results.

The evidence contract is documented field by field in
[Benchmark evidence format](../../benchmark-evidence-format.md). The corresponding build,
materialization, capture, process-run, analysis, and bundle-validation workflow is in the
[Benchmark reproduction guide](../../benchmark-reproduction.md).

Official measurements require the dedicated native Ubuntu host defined by ADR 0016. Windows,
Docker, WSL, and hosted-CI runs remain exploratory even when they pass every schema and digest
check.

The eleven implemented evidence boundaries are:

```text
core_throughput
core_latency
core_allocation
core_construction
core_preload
core_setup_allocation
replay_fast
replay_verify
python_objects
python_columns
python_summary
```

Each raw process attempt is retained, bound to one suite label of at most 32 characters, and tied
to exact workload-manifest, stream, timed-input, executable, environment, and host-context hashes.
Invalid attempts remain in reports but never contribute to statistics. Bundle verification
requires exact source/report/rendering/note closure and a SHA-256 inventory of every retained
file.

## Claim status

No throughput, latency, allocation, memory, scalability, durability, security, or
production-readiness claim is currently authorized.

The next evidence step is the unchanged Phase 4 baseline on the qualified native Ubuntu host,
using the [native-host baseline runbook](../../phase5-native-host-runbook.md). Until that run
exists, all locally or CI-produced observations are infrastructure validation only.
