# Phase 4 router evidence index

This directory indexes the first Phase 4 slice and distinguishes locally implemented evidence
surfaces from validation that has actually completed. The router slice is not remotely merged and
does not complete Phase 4.

## Current status

- Base: published Phase 3 PR #5 head
  `29049756e250fef04aac819c457438f0f01149c3`.
- Phase 3 prerequisite: required hosted gates passed.
- Remote integration: PR #5 remains open because GitHub authentication is unavailable in this
  session; remote `main` still ends at Phase 2.
- Router implementation: present locally on `codex/phase4-router`.
- Router local gate: GCC Debug and Release each pass 333/333 CTest tests; the pinned C++ formatter,
  296-test Python suite with two Windows privilege skips, 41 focused V2 tests, Ruff, and strict mypy
  pass. Hosted Clang/sanitizer validation and remote integration remain pending.

The accepted contract is [ADR 0012](../../decisions/0012-multi-instrument-routing-and-global-sequencing.md);
the implementation narrative is the
[Phase 4 router journal](../../journal/2026-07-24-phase4-router.md).

## Implemented evidence surfaces

| Surface | Version or location | Obligation |
| --- | --- | --- |
| Public router | `atlaslob::MultiInstrumentEngine` | Eager immutable catalog and independent books |
| Global audit order | Engine-owned sequencer | One sequence across instruments and rejections |
| Active identity | Engine-wide directory | Global uniqueness and checked owner/instrument routing |
| Capacity | Per-book plus engine policy | Validate the projected final active state |
| Book preparation | `PreparedCommandExecution` | Inspectable owned batch, RAII abandon, no-allocation book commit |
| Engine preparation | `PreparedMultiInstrumentCommand` | One lease and atomic directory/book/sequence publication |
| Multi snapshot | `EngineSnapshot` | Sorted catalog, global sequence, and complete FIFO state |
| State evidence | `ATLSME01` | Canonical independent C++/Python SHA-256 input |
| Python oracle | `ReferenceRouter` | Independent routing, identity, sequencing, and capacity |
| Generator | `MULTI_GENERATOR_VERSION = 2` | Reproducible multi-instrument valid/invalid streams |
| Workload spec | `atlas_workload_spec_v2` | Fully resolve catalog, engine policy, and distributions |
| Workload stream | `atlas_workload_stream_v2` | Canonical catalog and indexed commands |
| Workload manifest | `atlas_workload_manifest_v2` | Bind seed, spec, stream digest, count, and intent statistics |
| Reference capture | `atlas_differential_capture_v2` | Exact events and complete multi-engine state |
| Native adapter | `ATLAS_DIFF_V2` / `atlas_diff_v2` | Strict process evidence from the public C++ router |
| Native decoder | `atlaslob.multi_native` | Bind input/process/output and compare exact reference parity |

Generator V1, `ATLAS_DIFF_V1`, `ATLSST01`, `ATLSEV01`, Phase 3 campaign policy, retained corpora,
and failure signatures remain frozen and must continue passing unchanged.

## Router acceptance matrix

The local test surfaces cover:

- invalid empty/zero/duplicate catalogs and canonical sorted construction;
- interleaved global sequences, pure/state rejections, maximum sequence, and sticky exhaustion;
- cross-instrument duplicate, unknown, ownership, mismatch, and terminal-ID reuse behavior;
- projected per-instrument and engine-wide capacity;
- provisional identity and sequence rollback on injected allocation failure;
- engine-wide preparation for committed/rejected/error outcomes, overlapping-lease rejection, and
  exact identity-delta publication;
- complete catalog/book/directory/count/priority invariants;
- single-instrument facade and frozen evidence compatibility;
- independent C++/Python `ATLSME01` golden and sensitivity cases;
- Python `ReferenceRouter`, Generator V2, canonical workload/manifest round trips, shrinking, and
  independent-instrument reinterleaving;
- strict native V2 parsing, exact/compact output, checkpoint/final snapshots, and adapter failures;
- strict Python V2 encoding/decoding, transcript binding, digest/snapshot revalidation, and exact
  reference/native parity; and
- fixed-seed multi-engine stress with invariant checks after every operation.

These surfaces pass the available local gate recorded above. No hosted Phase 4 result is claimed
until the final router head is published and checked.

## Local reproduction

Run the normal C++ gates:

```sh
cmake --preset dev-gcc
cmake --build --preset dev-gcc
ctest --preset dev-gcc

cmake --preset release
cmake --build --preset release
ctest --preset release
```

Run the focused Python reference/evidence tests and static gates:

```sh
python -m pytest python/tests/test_reference_router.py python/tests/test_multi_evidence.py
python -m ruff format --check python
python -m ruff check python
python -m mypy
```

Run the sanitizer and compatibility gates through the repository presets/workflows before
publication. The retained Phase 3 corpus must pass without regenerated V1 goldens.

## Claim boundary

PR1 supplies deterministic in-memory routing and evidence. It does not supply command-log
durability, replay, persisted snapshot recovery, native Python bindings, benchmark results, a
network gateway, or production-operational guarantees.

Test counts and elapsed times are correctness metadata only. They are not latency, throughput,
allocation, memory, scalability, durability, or production-readiness claims.
