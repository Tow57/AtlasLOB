# Phase 4 evidence index

This directory indexes Phase 4 and distinguishes locally implemented evidence surfaces from
hosted validation or integration that is still pending. The four slices are stacked as router,
command-log/replay, persisted snapshots/recovery, and native Python packaging. PR3's 13 hosted
compiler, sanitizer, decoder-fuzz-smoke, Python, formatting, wheel, and differential checks are
green. PR4 is implemented and locally validated: its native pybind11 engine, strict preflight,
three batch modes, logging/recovery/snapshot API, GIL/mutex boundary, four CPython 3.11-3.14
manylinux wheels, and source distribution pass the local gates recorded below. All 15 hosted PR4
checks are green; stacked integration remains pending.

## Current status

- Base: published Phase 3 PR #5 head
  `29049756e250fef04aac819c457438f0f01149c3`.
- Phase 3 prerequisite: required hosted gates passed.
- Remote integration: draft PR #5 remains open and remote `main` still ends at Phase 2. The Phase 4
  router, command-log, snapshot, and native-Python slices are published as sequential draft PRs
  #7, #8, #6, and #9.
- Router implementation: published on `codex/phase4-router`.
- Router local gate: GCC Debug and Release each pass 333/333 CTest tests; the pinned C++ formatter,
  296-test Python suite with two Windows privilege skips, 41 focused V2 tests, Ruff, and strict mypy
  pass. The green hosted PR3 head revalidates the stacked router code; individual integration
  remains pending.
- Command-log/replay contract: ADR 0013 and the byte-level `ATLSLG01` V1 reference are accepted on
  `codex/phase4-command-log`.
- Command-log/replay implementation: complete locally. GCC Debug and Release each pass 418/418
  CTest cases, including 85 persistence cases. The production-only build, pinned formatter,
  288-test non-campaign Python gate with two expected Windows symlink skips, Ruff, and strict mypy
  pass. The deterministic decoder mutation/truncation suite passes, and the green hosted PR3 head
  revalidates this stack with ASan/UBSan and bounded Clang libFuzzer smoke.
- Snapshot/recovery contract and implementation: ADR 0014 and the byte-level `ATLSSN01` V1
  reference are implemented on PR3. The focused selection contains 11 core
  restore, 21 codec, 22 publication/recovery/inspection/report, two exact-extent append-sink, and
  four exact V2 report cases; 59 pass and the canonical-symlink case is an expected Windows skip.
  GCC Debug and Release each pass 482/482 CTest cases. The production-only build, 288-test
  non-campaign and 11-test marked Python selections, and Unicode-path/LF-only CLI
  process-boundary script also pass. All 13 hosted PR3 checks are green; merge remains pending.
- Native Python contract and implementation: ADR 0015, `atlaslob.Engine`, the private
  `_native_engine` module, strict conversion, owned batch outputs, persistence/recovery/snapshot
  bindings, and package version 0.2.0 are implemented on draft PR #9. GCC Debug and
  Release each pass 482/482 CTest cases. Python reports 354 passes with two expected Windows
  canonical-symlink skips plus 11 campaign/fuzz passes. Ruff, strict mypy, and formatting pass.
- Native package artifacts: cibuildwheel 4.1.0 builds CPython 3.11, 3.12, 3.13, and 3.14
  manylinux x86-64 wheels. Wheel contents, auditwheel dependencies, and clean-container smokes
  pass. The PEP 517 source distribution builds, installs, and smokes in a clean environment.
  All 15 hosted PR4 checks pass.

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
The PR4 contract is
[ADR 0015](../../decisions/0015-native-python-bindings-and-packaging.md), with implementation and
local artifact evidence in the
[native Python journal](../../journal/2026-07-25-phase4-native-python.md).

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
| Public Python engine | `atlaslob.Engine` | Lazy native-only facade with live/logged/recovered backends |
| Binding ABI | `atlaslob._native_engine` / `BINDING_ABI` | Fail-closed wrapper/extension compatibility check |
| Strict preflight | Exact Python value conversion | Complete finite batch checked before first submission |
| Batch evidence | objects, columns, summary | Owned output modes with common counts and final digest |
| Python recovery | clean writable or torn-prefix read-only | No append to an unrepaired torn authoritative log |
| Concurrency boundary | GIL release plus per-engine mutex | Whole-batch noninterleaving for one engine |
| Native distribution | `atlaslob==0.2.0` | CPython 3.11-3.14 manylinux wheels and PEP 517 sdist |

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
| Decoder fuzzing | bounded header/record seeds and smoke runs | Deterministic mutation evidence and hosted PR3 libFuzzer smoke pass |

The log stores expected event count and digest, not expected event bodies. Diagnostic replay can
identify the first divergent record and compare logged metadata with actual replay output, but a
field-level expected event body requires a separately retained exact transcript.

## Snapshot/recovery evidence

The rows below describe the focused snapshot/recovery evidence. The full local gates and all 13
hosted PR3 checks also pass.

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

## Native Python local evidence

| Surface | Contract | Current evidence status |
| --- | --- | --- |
| Import isolation | Oracle modules never import `_native_engine` | Blocked-extension oracle tests pass; requesting `Engine` fails clearly |
| ABI boundary | Private integer `BINDING_ABI` | Missing and incompatible extension cases fail closed |
| Strict conversion | Exact command/config types and integer ranges | `bool`, overflow, wrong shape, late malformed batch, raw invalid enum, and Unicode-path cases pass |
| Object batch | Immutable owned `EngineResult` tuple | Single/batch identity, result lifetime, rejection continuation, and terminal stopping pass |
| Column batch | Owned standard-library arrays plus presence columns | Offsets, variant fields, zero/absence distinction, mutation isolation, and object parity pass |
| Summary batch | Counts, terminal status, and digest only | Counts and final digest agree with object and column modes |
| Logged execution | Write-ahead native backend | Logged batch parity, rejection persistence, prefix accounting, and poison behavior pass |
| Recovery | Clean writable; torn valid-prefix read-only | Clean, strict torn, valid-prefix torn, corruption, snapshot, and repaired-copy cases pass |
| Snapshot publication | Logged engine only | Unicode-directory publication and owned report/snapshot output pass |
| Same-engine concurrency | One mutex for a complete batch | Concurrent calls receive noninterleaved command-sequence ranges |
| GIL boundary | Release only for Python-free native work | Another Python thread makes progress during native execution |
| Oracle/native parity | Independent `ReferenceRouter` and V2 adapter | Named native batches agree on events, state, outcomes, and digests |
| Wheel matrix | cp311-cp314 manylinux x86-64 | Four wheels build; content, auditwheel, exact extension-export, and clean-container smokes pass |
| Source distribution | PEP 517 isolated build | Contents, build-from-sdist, clean install, and smoke pass |

Local aggregate evidence is 482/482 CTest cases in both GCC Debug and Release; 354 Python passes
with two expected Windows canonical-symlink skips; 11 additional campaign/fuzz passes; and green
Ruff, strict mypy, and formatting checks. All 15 hosted PR4 checks independently revalidate the
stack on implementation commit `0525c9b`.

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

These surfaces pass the available local gate recorded above. Router draft PR #7 has 12 green
hosted checks, PR3 has 13, and PR4 has 15; stacked integration remains pending.

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

Build and inspect the native package artifacts on a Linux x86-64 host with Docker available:

```sh
python -m cibuildwheel --output-dir wheelhouse .
python tests/packaging/verify_wheels.py wheelhouse
python tests/packaging/verify_auditwheel.py wheelhouse

python -m build --sdist --outdir dist
python tests/packaging/verify_sdist.py dist
```

Each cibuildwheel environment runs `tests/packaging/wheel_smoke.py` against its installed wheel.
The audit verifier also requires the Linux extension to define only
`PyInit__native_engine` in its dynamic export table.
The source-distribution gate installs `dist/atlaslob-0.2.0.tar.gz` through normal isolated PEP 517
build handling in a clean environment, then runs the same smoke and `pip check`.

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

The retained Phase 3 corpus passes without regenerated V1 goldens in the hosted PR4 run. Stacked
integration remains required.

## Claim boundary

PR1 supplies deterministic in-memory routing and evidence. PR2 supplies the command-log, scanner,
write-ahead, repair, replay, reporting, CLI, and fuzz-target implementation. PR3 supplies the
snapshot codec, all-or-nothing bulk reconstruction, synchronized publication, inspection,
candidate-safe discovery, log-suffix recovery, and clean-tail writable resumption surfaces; its
hosted checks are green. PR4 supplies the locally validated native Python facade, strict batches,
logging/recovery/snapshot bindings, concurrency/ownership boundary, wheels, and source
distribution; all 15 of its hosted checks are green. The stacked PRs are not yet integrated.
Benchmark results, a network gateway, and production-operational guarantees remain outside the
current evidence.

Test counts and elapsed times are correctness metadata only. They are not latency, throughput,
allocation, memory, scalability, durability, or production-readiness claims.
