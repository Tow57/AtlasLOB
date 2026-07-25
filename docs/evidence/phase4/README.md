# Phase 4 evidence index

This directory indexes Phase 4 and distinguishes locally implemented evidence surfaces from
contracts or validation that are still in progress. The router slice is not remotely merged.
The command-log/replay PR2 contract and implementation are complete locally, with hosted
Clang/sanitizer/libFuzzer validation and remote integration still pending. The
persisted-snapshot/recovery PR3 contract and implementation are also complete on the working
branch. Its 60-case selected recovery gate reports 59 passes plus one expected Windows
canonical-symlink skip. GCC Debug and Release each pass 482/482 CTest cases, the production-only
build and both Python selections pass, and the CLI process-boundary check passes. Hosted
compiler/sanitizer/libFuzzer validation remains pending. These slices do not complete Phase 4
because native Python packaging is still outstanding.

## Current status

- Base: published Phase 3 PR #5 head
  `29049756e250fef04aac819c457438f0f01149c3`.
- Phase 3 prerequisite: required hosted gates passed.
- Remote integration: PR #5 remains open because the available GitHub integration cannot create or
  merge pull requests in this repository; remote `main` still ends at Phase 2.
- Router implementation: published on `codex/phase4-router` at `dd9bcc2`.
- Router local gate: GCC Debug and Release each pass 333/333 CTest tests; the pinned C++ formatter,
  296-test Python suite with two Windows privilege skips, 41 focused V2 tests, Ruff, and strict mypy
  pass. Hosted Clang/sanitizer validation and remote integration remain pending.
- Command-log/replay contract: ADR 0013 and the byte-level `ATLSLG01` V1 reference are accepted on
  `codex/phase4-command-log`.
- Command-log/replay implementation: complete locally. GCC Debug and Release each pass 418/418
  CTest cases, including 85 persistence cases. The production-only build, pinned formatter,
  288-test non-campaign Python gate with two expected Windows symlink skips, Ruff, and strict mypy
  pass. The deterministic decoder mutation/truncation suite passes; actual Clang libFuzzer and
  ASan/UBSan execution remain authoritative hosted gates because the local Windows toolchain lacks
  those runtimes.
- Snapshot/recovery contract and implementation: ADR 0014 and the byte-level `ATLSSN01` V1
  reference are implemented on the PR3 working branch. The focused selection contains 11 core
  restore, 21 codec, 22 publication/recovery/inspection/report, two exact-extent append-sink, and
  four exact V2 report cases; 59 pass and the canonical-symlink case is an expected Windows skip.
  GCC Debug and Release each pass 482/482 CTest cases. The production-only build, 288-test
  non-campaign and 11-test marked Python selections, and Unicode-path/LF-only CLI
  process-boundary script also pass. Hosted Clang, ASan/UBSan, actual Clang libFuzzer execution,
  publication, and merge remain pending.

The accepted contract is [ADR 0012](../../decisions/0012-multi-instrument-routing-and-global-sequencing.md);
the implementation narrative is the
[Phase 4 router journal](../../journal/2026-07-24-phase4-router.md).
The PR2 contract is [ADR 0013](../../decisions/0013-command-log-and-replay.md), with exact bytes in
the [command-log format reference](../../command-log-format.md) and current scope in the
[command-log journal](../../journal/2026-07-24-phase4-command-log.md).
The PR3 contract is
[ADR 0014](../../decisions/0014-persisted-snapshots-and-log-suffix-recovery.md), with exact bytes
in the [snapshot format reference](../../snapshot-format.md) and current local evidence in the
[snapshot/recovery journal](../../journal/2026-07-25-phase4-snapshot-recovery.md).

## Implemented evidence surfaces

| Surface | Version or location | Obligation |
| --- | --- | --- |
| Public router | `atlaslob::MultiInstrumentEngine` | Eager immutable catalog and independent books |
| Global audit order | Engine-owned sequencer | One sequence across instruments and rejections |
| Active identity | Engine-wide ID and priority directories | Global ID/priority uniqueness and checked owner/instrument routing |
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
| Command-log header | `ATLSLG01` / `ATLSCF01` | Canonical sorted configuration and checked host conversion |
| Command records | Format V1 | Raw domain fields, outcome evidence, event count/digest, CRC32C |
| Write-ahead facade | `LoggedEngine` | Persist and synchronize before allocation-free publication |
| Bounded inspection | `atlas_inspect` | Structured offsets, record summaries, and safe new-file repair |
| Deterministic replay | `atlas_replay` | Fast, verify, and diagnostic modes with strict tail policy |
| Machine reports | `ATLAS_LOG_REPORT_V1`, `ATLAS_REPLAY_REPORT_V1` | Byte-stable path/time-free JSON |
| Snapshot codec | `ATLSSN01` V1 | Bounded canonical hierarchy with whole-file CRC32C |
| Core restoration | Private staged bulk restore seam | Allocate levels/nodes/indexes/directories before FIFO linking and sequence publication |
| Snapshot publication | `LoggedEngine::write_snapshot` | Log sync, verified temporary file, atomic no-replace publication |
| Snapshot recovery | Explicit or newest valid directory candidate | Exact boundary plus verified log-suffix replay |
| Writable recovery | `LoggedEngine::recover*` | Clean-tail, exact-extent, existing-only append resumption |
| Snapshot reports | `ATLAS_SNAPSHOT_REPORT_V1`, `ATLAS_REPLAY_REPORT_V2` | Stable path/time-free inspection and recovery evidence |
| Decoder fuzzing | Three libFuzzer targets | Retained log/snapshot seeds plus bounded hosted smoke job |

Generator V1, `ATLAS_DIFF_V1`, `ATLSST01`, `ATLSEV01`, Phase 3 campaign policy, retained corpora,
and failure signatures remain frozen and must continue passing unchanged.

## Command-log/replay local evidence

| Surface | Frozen contract | Current evidence status |
| --- | --- | --- |
| Header/catalog codec | `ATLSLG01`, `ATLSCF01`, `96 + 28N` bytes | Golden and bounded-decode tests pass |
| Command records | 66-byte envelope; 102/82/106-byte variants | All variants/raw enum/boundary tests pass |
| Corruption checks | CRC32C Castagnoli over header/record except trailing CRC | Published vector and mutation tests pass |
| Bounded scanner | 1 MiB header, 64 KiB record, checked arithmetic | Every truncation/classification tests pass |
| Write-ahead session | buffered/flush/sync; sync default; sticky poison | Every partial-write boundary and failure seam passes |
| Commit breach | impossible post-WAL mismatch returns sticky `state_not_recoverable` | No-throw seam and post-commit equivalence checks pass |
| Tail repair | validated-prefix copy to distinct new output only | Clean refusal, torn repair, and corruption refusal pass |
| Replay | fast/verify/diagnostic; strict/valid-prefix | State/evidence/tail and validated-prefix digest-sensitivity tests pass |
| Repeated verified replay | Real file-backed log, two independent engine rebuilds | Exact JSON/text report bytes, counts, original snapshots/digests, and follow-on trade events/state agree |
| Machine reports | `ATLAS_LOG_REPORT_V1`, `ATLAS_REPLAY_REPORT_V1` | Exact JSON/text golden tests pass |
| Decoder fuzzing | bounded header/record seeds and smoke runs | Deterministic PR2 mutation/smoke evidence passes; hosted libFuzzer pending |

The log stores expected event count and digest, not expected event bodies. Diagnostic replay can
identify the first divergent record and compare logged metadata with actual replay output, but a
field-level expected event body requires a separately retained exact transcript.

## Snapshot/recovery local evidence

The rows below describe the focused working-branch evidence. They do not substitute for the
pending full CTest, hosted, sanitizer, or libFuzzer gates.

| Surface | Frozen contract | Current evidence status |
| --- | --- | --- |
| Snapshot file | `ATLSSN01` V1; 169-byte fixed header; whole CRC32C | Reviewed 561-byte populated golden and exact round trip pass |
| Catalog identity | Exact 28-byte entries plus `ATLSCF01` | Valid, invalid, duplicate, order, host-conversion, and digest cases pass |
| Canonical hierarchy | 36-byte instrument, 32-byte level, 41-byte order entries | FIFO, empty configured instrument, hierarchy, sorting, aggregate, and count cases pass |
| Bounded decode | 256 MiB default with explicit snapshot-only override | Every-byte truncation, every-byte single-bit corruption, bound, and count-bomb cases pass |
| State evidence | Recomputed `ATLSME01` | Round-trip equality and configuration/state digest mismatch cases pass |
| Core restore | Bulk-staged nodes, links, indexes, ID/priority directories, and sequence | Exact 1,024-order rebuild, 26 allocation boundaries, rejection/exhaustion, and malformed-state cases pass |
| Publication | Log sync, verified same-directory temp, atomic no-replace rename | All file stages, surfaced cleanup, log-sync poison, duplicate final, and prior-good preservation pass |
| Discovery | Canonical names, non-following status, newest-to-oldest validation | Filename mismatch, candidate I/O, symlink non-following, newer-bad/older-good, and full-log fallback pass |
| Suffix recovery | Exact covered offset and `covered_sequence + 1` | Boundary rejection, strict/valid-prefix tails, rejection boundary, and full-log equivalence pass |
| Writable recovery | Clean tail plus exact append extent | Full-log/snapshot/directory resume and append pass; torn/missing logs remain non-writable |
| Snapshot inspection | `ATLAS_SNAPSHOT_REPORT_V1` | Stable JSON schema, caller bound, CLI exit, Unicode path, and LF-only output pass |
| Snapshot replay | `ATLAS_REPLAY_REPORT_V2`; V1 log-only report unchanged | Stable V2 fields/path omission and unchanged V1 process behavior pass |

The focused total is 60 selected cases: 11 core restoration, 21 codec, 22
publication/recovery/inspection/report, two exact-extent append-sink, and four exact V2 report
goldens. It reports 59 passes plus one expected Windows canonical-symlink skip. The fixed golden is
a reviewed literal and is not claimed to come from an independent encoder.

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

Run the persistence and Python gates:

```sh
ctest --test-dir build/dev-gcc --output-on-failure -L persistence
atlas_inspect log <path> --json --records
atlas_inspect snapshot <path> --json
atlas_replay <path> --mode verify --json
atlas_replay <path> --snapshot <snapshot-path> --mode verify --json
atlas_replay <path> --snapshot-dir <directory> --mode verify --json

python -m pytest -m "not campaign and not differential_fuzz"
python -m ruff format --check python
python -m ruff check python
python -m mypy
```

With a non-MSVC Clang toolchain, run the retained decoder seed smoke:

```sh
cmake -S . -B build/command-log-fuzz -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DATLAS_BUILD_FUZZERS=ON \
  -DATLAS_ENABLE_ASAN=ON \
  -DATLAS_ENABLE_UBSAN=ON
cmake --build build/command-log-fuzz \
  --target atlas_fuzz_log_header_decoder atlas_fuzz_log_record_decoder \
  atlas_fuzz_snapshot_decoder
python tests/fuzz/run_command_log_fuzz_smoke.py \
  --header-fuzzer build/command-log-fuzz/atlas_fuzz_log_header_decoder \
  --record-fuzzer build/command-log-fuzz/atlas_fuzz_log_record_decoder \
  --snapshot-fuzzer build/command-log-fuzz/atlas_fuzz_snapshot_decoder \
  --corpus-dir build/command-log-fuzz/corpus
```

The retained Phase 3 corpus must pass without regenerated V1 goldens. Hosted sanitizer and
libFuzzer evidence must pass before the stacked persistence slices are integrated.

## Claim boundary

PR1 supplies deterministic in-memory routing and evidence. PR2 supplies the locally validated
command-log, scanner, write-ahead, repair, replay, reporting, CLI, and fuzz-target implementation.
PR3 supplies the implemented snapshot codec, all-or-nothing bulk reconstruction, synchronized
publication, inspection, candidate-safe discovery, log-suffix recovery, and clean-tail writable
resumption surfaces with the local evidence above. The stacked PRs are not remotely integrated;
all Phase 4 hosted Clang/sanitizer/libFuzzer results remain pending. Native Python bindings,
benchmark results, a network gateway, and production-operational guarantees remain outside the
current evidence.

Test counts and elapsed times are correctness metadata only. They are not latency, throughput,
allocation, memory, scalability, durability, or production-readiness claims.
