# Phase 4 evidence index

This directory indexes Phase 4 and distinguishes locally implemented evidence surfaces from
contracts or validation that are still in progress. The router slice is not remotely merged.
The command-log/replay PR2 contract and implementation are complete locally, with hosted
Clang/sanitizer/libFuzzer validation and remote integration still pending. Neither slice completes
Phase 4.

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

The accepted contract is [ADR 0012](../../decisions/0012-multi-instrument-routing-and-global-sequencing.md);
the implementation narrative is the
[Phase 4 router journal](../../journal/2026-07-24-phase4-router.md).
The PR2 contract is [ADR 0013](../../decisions/0013-command-log-and-replay.md), with exact bytes in
the [command-log format reference](../../command-log-format.md) and current scope in the
[command-log journal](../../journal/2026-07-24-phase4-command-log.md).

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
| Command-log header | `ATLSLG01` / `ATLSCF01` | Canonical sorted configuration and checked host conversion |
| Command records | Format V1 | Raw domain fields, outcome evidence, event count/digest, CRC32C |
| Write-ahead facade | `LoggedEngine` | Persist and synchronize before allocation-free publication |
| Bounded inspection | `atlas_inspect` | Structured offsets, record summaries, and safe new-file repair |
| Deterministic replay | `atlas_replay` | Fast, verify, and diagnostic modes with strict tail policy |
| Machine reports | `ATLAS_LOG_REPORT_V1`, `ATLAS_REPLAY_REPORT_V1` | Byte-stable path/time-free JSON |
| Decoder fuzzing | Two libFuzzer targets | Retained canonical seeds plus bounded hosted smoke job |

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
| Machine reports | `ATLAS_LOG_REPORT_V1`, `ATLAS_REPLAY_REPORT_V1` | Exact JSON/text and repeat-byte tests pass |
| Decoder fuzzing | bounded header/record seeds and smoke runs | Deterministic smoke passes; hosted libFuzzer pending |

The log stores expected event count and digest, not expected event bodies. Diagnostic replay can
identify the first divergent record and compare logged metadata with actual replay output, but a
field-level expected event body requires a separately retained exact transcript.

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
atlas_replay <path> --mode verify --json

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
  --target atlas_fuzz_log_header_decoder atlas_fuzz_log_record_decoder
python tests/fuzz/run_command_log_fuzz_smoke.py \
  --header-fuzzer build/command-log-fuzz/atlas_fuzz_log_header_decoder \
  --record-fuzzer build/command-log-fuzz/atlas_fuzz_log_record_decoder \
  --corpus-dir build/command-log-fuzz/corpus
```

The retained Phase 3 corpus must pass without regenerated V1 goldens. Hosted sanitizer and
libFuzzer evidence must pass before PR2 merge.

## Claim boundary

PR1 supplies deterministic in-memory routing and evidence. PR2 supplies the locally validated
command-log, scanner, write-ahead, repair, replay, reporting, CLI, and fuzz-target implementation.
Neither PR is remotely integrated, and PR2's hosted Clang/sanitizer/libFuzzer result remains
pending. Persisted snapshot recovery, native Python bindings, benchmark results, a network gateway,
and production-operational guarantees remain outside the current evidence.

Test counts and elapsed times are correctness metadata only. They are not latency, throughput,
allocation, memory, scalability, durability, or production-readiness claims.
