# Phase 5 benchmark-contract journal

Date: 2026-07-25

## Objective

Build the opt-in, correctness-checked measurement foundation for Phase 5 without changing the
matching semantics, public APIs, canonical evidence bytes, persistence formats, or package
contents.

## Decisions frozen

- [ADR 0016](../decisions/0016-reproducible-performance-evidence.md) defines the measured
  boundaries, qualified-host rules, schemas, observation process model, statistics, experiment
  acceptance gate, and release workflow.
- [Performance methodology](../performance-methodology.md) is the operational measurement
  contract.
- Production libraries remain independent of Google Benchmark and measurement instrumentation.
- One runner process produces exactly one observation.
- Workload generation, parsing, validation replay, digest computation, and I/O stay outside
  timing. Ordinary core runs also exclude construction, preload, and warm-up; dedicated setup and
  preload boundaries label those costs explicitly.
- Shared CI proves that tools build, reject malformed evidence, reproduce tiny fixtures, and
  preserve digests. It does not establish a performance threshold.
- W11 remains explicitly deferred to the Phase 6 gateway.

## Slice evidence status

The PR 1 infrastructure is complete locally:

- workload manifests bind preload, warm-up, measured, active-state, prefix, final-state, and W10
  timed-log evidence;
- environment capture derives build provenance from the complete CMake target closure and records
  native or installed-wheel CPython identity plus sanitized host, filesystem, and storage facts;
- native runners cover throughput, latency, steady allocation, construction, preload,
  construction-plus-preload allocation, and warmed-log fast/verify replay;
- the standalone installed-wheel worker covers Python objects, columns, and summary batches;
- the suite schedules one observation per process, defaults to ten standalone attempts or five
  A-B-B-A blocks, caps suite labels at 32 characters, and retains invalid attempts;
- deterministic analysis emits exact group, comparison, and experiment records plus byte-stable
  Markdown and SVG; and
- bundle verification enforces exact workload/environment/observation/report closure, W10
  stream-to-log equivalence, experiment-note bindings, and full-file SHA-256 inventory.

This branch contains infrastructure only. It does not contain an authoritative baseline,
optimization result, or publishable performance number. The complete 16-job hosted run passed on
PR #11 implementation head `35b09ba`, including the threshold-free exploratory benchmark smoke,
GCC/Clang Debug and Release, ASan/UBSan, CPython 3.11-3.14, differential, fuzz, wheel, source
distribution, native-extension, and formatting gates.

The authoritative Phase 5 host remains a dedicated native Ubuntu 24.04 x86-64 machine. WSL2,
containers, and GitHub-hosted runners may provide exploratory or smoke evidence, but their
observations must identify virtualization and remain outside public performance claims.

## Reproduction

The [benchmark reproduction guide](../benchmark-reproduction.md) records the opt-in build,
workload materialization, sanitized environment capture, one-process observations, deterministic
analysis, and bundle verification workflow. The
[benchmark evidence format](../benchmark-evidence-format.md) records the exact field vocabulary,
canonical encoding, cross-file bindings, and invalid-run retention rules.

These commands are currently infrastructure reproduction only. The Phase 5 evidence index keeps
native-host baselines, profiles, experiments, and exact-tag release evidence explicitly
unstarted.

## Contract audit

The final PR 1 documentation was checked against the live Python dataclasses/canonical serializers,
the C++ observation serializer, all native runner modes, the standalone Python worker, analysis
grouping and experiment derivation, and bundle verification. In particular:

- ordinary core timing excludes construction, preload, warm-up, validation replay, hashing, and
  output, while the separately reported RSS envelope is intentionally broader;
- `core_preload` and `core_setup_allocation` use preload-prefix evidence rather than measured-region
  evidence;
- replay observations bind the exact W10 `.atlslg` digest and warm-page-cache policy;
- Python input parsing, batch-slice preparation, prefix execution, final hashing, and independent
  validation stay outside timing;
- invalid observations are excluded from statistics but retained, and an official group still
  requires at least ten valid independent attempts; and
- W11 has no boundary in the V1 schema and remains a Phase 6 gateway item.
