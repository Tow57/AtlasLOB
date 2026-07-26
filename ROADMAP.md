# AtlasLOB Roadmap

Each slice must end with observable behavior, automated success and failure tests, documentation,
and a clean reproduction command. Later phases do not begin until the preceding release gate is
satisfied.

## Phase 0 - Executable foundation

- [x] Portable C++20 library, CLI, and test targets.
- [x] GCC and Clang presets and CI.
- [x] Warnings, formatting, ASan, and UBSan gates.
- [x] Strong values and deterministic new-order validation.
- [x] Initial semantics and architecture decision record.

## Phase 1 - Domain model and resting book

- [x] Complete command, event, and rejection vocabulary.
- [x] Stable owning storage for order nodes.
- [x] FIFO price levels with aggregate quantity and order count.
- [x] Ordered bid and ask sides with best-price access.
- [x] Direct indexed cancellation without level scans.
- [x] Full structural invariants and fixed-seed stress tests.
- [x] Hosted sanitizer evidence for the completed indexed-book slice.

The resting-book structure now provides ordered sides, stable level addresses, guarded level
preparation, a checked active-order index, direct cancellation, active-only ID reuse, safe
teardown, and cross-structure invariants. A 10,000-operation fixed-seed model stress test checks
the complete book after every mutation. The published Phase 2 head passed hosted GCC, Clang, and
ASan/UBSan with the indexed-book slice included.

ADR 0005 records the accepted ownership boundary: storage remains the sole node owner, a checked
non-owning index provides direct identity lookup, and one centralized path performs level unlink,
index removal, empty-level cleanup, and storage destruction.

## Phase 2 - Matching MVP

- [x] Assign authoritative sequences and apply deterministic state validation.
- [x] Produce immutable read-only match plans and checked final-capacity projections.
- [x] Preallocate owned event batches and staged GTC residuals before mutation.
- [x] Rest non-marketable limit GTC orders.
- [x] Match one level with full and partial fills.
- [x] Sweep multiple orders and price levels.
- [x] Support market IOC and limit IOC residual behavior.
- [x] Replace with cancel-and-new priority reset.
- [x] Produce normalized event and state digests.
- [x] Add a deterministic engine fixture with per-command evidence.
- [x] Compare mixed command streams against an independent map/deque reference model.

ADR 0006 fixes the Phase 2 failure boundary: admission consumes the command sequence, match plans
contain values rather than pointers, event capacity and a possible resting residual are allocated
before mutation, and active-order capacity is checked against the planned final state. Local
Debug and Release suites pass 157/157 tests, the production-only build and pinned formatting gate
pass, and an independent review found no blocker or high-severity issue. Hosted compiler and
sanitizer evidence remains a pull-request gate.

ADR 0007 applies that boundary to executable New and Cancel commands. Plans are rebound in exact
best-price/FIFO order; final capacity and top of book are projected before mutation; events and a
possible residual are fully allocated first; and all passive reductions are preflighted as one
batch. End-to-end tests cover both sides, multi-level sweeps, exact and residual IOC/market
outcomes, final-state capacity, cancellation after partial execution, active-ID reuse, and
exception rollback after residual preparation.

ADR 0008 completes command execution with atomic Replace. The old order, passive fills, and
optional new residual commit through one replacement-specific all-preflight boundary; same-price
replacement receives new FIFO priority and credits the old aggregate before checked addition.
The public `MatchingEngine` PImpl now exposes value-only commands, events, top of book, active
count, and sequence state without exposing mutable book internals.

ADR 0009 closes Phase 2 with exact best-price/FIFO snapshots, versioned fixed-width state and
event digests, committed/rejected/malformed golden engine fixtures, and a structurally independent
map/deque reference model. Four fixed seeds compare 10,000 mixed commands after every transition;
a further 2,500-command rerun verifies the complete digest transcript, and 66 directed commands
prove exact-capacity rejection and terminal execution. Local Debug and Release suites pass
254/254 tests and the production-only build passes. The published Phase 2 head subsequently passed
hosted GCC, Clang, ASan/UBSan, and pinned formatting. A hardening follow-up makes public result
states mutually exclusive, pins prepared replacement identity safely, and expands the local suite
to 264 tests; that follow-up must rerun the same hosted gates before merge.

## Phase 3 - Independent correctness evidence

- [x] Versioned, test-only native evidence adapter over the public engine API.
- [x] Independent canonical state/event encoders with frozen cross-language hash vectors.
- [x] Straightforward Python reference model.
- [x] Named per-command differential comparison with exact events and snapshots.
- [x] Seeded valid and invalid command generation.
- [x] Failure persistence and shrinking.
- [x] Fixed CI corpus, long campaigns, fuzzing, and hosted release evidence.

ADR 0010 fixes the independence boundary: the Python model uses ordinary dictionaries, deques,
and sorted prices; it has no binding or access to C++ transition helpers; and native execution is
observed only through a versioned test process. The first slice compares complete event payloads,
headers, canonical snapshots, top/count/sequence observers, and both digests after every command
in 13 named scenarios. Independent golden encoders reproduce the ADR 0009 empty, representative,
signed-price, all-event, and rejection hashes.

ADR 0011 freezes generator V1, ten required profiles, fully resolved workload manifests, and
fixed, rotating-epoch, and published seed policy. The disk-spooled runner closes the complete
reference pass before native execution, compares native JSONL incrementally, and reruns every
semantic divergence from fresh engines with one exact checkpoint at the divergent command.
Standard diagnostic bundles record the first structured difference, both evidence streams, recent
commands, build identity, and exact reproduction. Summary-only large-tier bundles may omit the
transcripts while retaining the workload, manifest, first difference, and digests.
Deterministic semantic reduction preserves one stable signature while deleting commands and
simplifying identities, prices, quantities, command types, side, and time in force.

Release execution is sharded by compiler and case behind a full GCC/Clang Release build-and-CTest
prerequisite; the sanitizer subset is sharded per case. Those capacity-bound shards cap automatic
exact replay at 1,000,000 commands. A larger required prefix remains a semantic failure with
`diagnosis.status=deferred_command_limit` and requires manual exact reproduction on a suitable
host; it is never counted as passing release evidence.

Three development-only evidence-boundary faults prove detection and shrinking for newest-at-price,
incoming-price execution, and stale partial-fill aggregates. Five applicable metamorphic
properties, bounded byte-stream and single-field-mutation fuzzing, and checked minimal/golden/
boundary/regression corpora are implemented.

The local default Python selection passes 244 tests with 2 Windows-symlink skips and 11 deselected;
the marked campaign/fuzz selection passes 11 with 246 deselected; and the fixed pull-request policy
passes 10 exact 5,000-command cases. Checked evidence records a passing epoch-0 nightly case with
1,000,000 compact commands. GCC Debug and Release CTest each pass 288/288; the production-only
build, pinned clang-format, Ruff, strict mypy, and wheel build/install smoke gates also pass
locally. Published PR #5 head `29049756` passed every required hosted GCC/Clang Release,
ASan/UBSan, Python 3.11-3.14, formatting, wheel, PR-corpus, and Linux link-safety gate. This closes
the Phase 3 implementation and evidence gate. PR #5 was squash-merged as `3c60e2b` before Phase 4
integration began.

## Phase 4 - Deterministic infrastructure and Python

- [x] Multi-instrument routing and global sequencing - merged through PR #7.
- [x] Explicit append-only command log codec - merged through PR #8.
- [x] Inspector, replay, corruption, and truncated-tail tests - merged through PR #8.
- [x] Canonical persisted snapshot plus log-suffix recovery - merged through PR #6.
- [x] pybind11 batch API and distributable native-backed Python package - merged through PR #9.

[ADR 0012](docs/decisions/0012-multi-instrument-routing-and-global-sequencing.md) defines the first
Phase 4 slice. The implementation adds an immutable, eagerly constructed
instrument catalog; one global command sequence; one engine-wide active-order identity directory;
projected per-instrument and global capacity; complete multi-engine snapshots; and the separate
`ATLSME01` state digest. The existing `MatchingEngine` delegates through the same execution path,
while semantic version 6 and the frozen `ATLSST01`, `ATLSEV01`, and `ATLAS_DIFF_V1` contracts
remain unchanged.

The independent Python side adds `ReferenceRouter`, Generator V2, canonical V2 workload/manifest
schemas, multi-engine digest parity, and a constrained independent-instrument reinterleaving
property that normalizes only the absolute global sequence and priority values that interleaving
is expected to change. A separate test-only `atlas_diff_multi_native` target accepts the strict
`ATLAS_DIFF_V2` catalog/command stream and emits exact or compact `atlas_diff_v2` JSON Lines; the
Python process boundary strictly decodes and binds those records to the requested workload and
independent reference capture. A move-only engine-wide preparation owns the command, complete
precommit batch, staged book mutation, exact identity removals, preallocated directory addition,
and reserved sequence under one lease. The later persistence slice can append between preparation
and its allocation-free no-throw publication of book, directory, and sequence state.

[ADR 0013](docs/decisions/0013-command-log-and-replay.md) and the
[command-log format reference](docs/command-log-format.md) define the implemented `ATLSLG01` V1
contract: a canonical header/catalog digest, fixed command records, CRC32C, bounded scanning,
write-ahead durability modes, sticky session poisoning, safe copy-only tail repair, and
fast/verify/diagnostic replay. A post-WAL engine commit is required to be production-infallible; an
impossible detected mismatch returns sticky `state_not_recoverable` and forces authoritative-log
recovery rather than permitting the session to continue. Since the log stores event count and
digest rather than complete expected events, diagnostic replay cannot reconstruct an expected
field-level event body without a separate transcript.

PR1 and PR2 were independently validated, retargeted to `main`, rerun through their full hosted
matrices, and squash-merged through PRs #7 and #8. PR3 passes 482/482 GCC Debug and Release CTest
cases, the production-only build, the 288-test non-campaign and 11-test marked Python selections,
and the CLI process-boundary check. Its 13 hosted compiler, sanitizer, decoder-fuzz-smoke, Python,
formatting, wheel, and differential checks are green.

[ADR 0014](docs/decisions/0014-persisted-snapshots-and-log-suffix-recovery.md) and the
[snapshot format reference](docs/snapshot-format.md) define the implemented `ATLSSN01` V1 slice.
It adds a bounded canonical snapshot codec; all-or-nothing bulk reconstruction that allocates
levels, nodes, local indexes, the global active-ID directory, and a reverse active-priority
directory before linking FIFO state; log-synchronized unique publication; standalone inspection;
candidate-safe newest-valid directory selection that never follows canonical snapshot symlinks;
and verified recovery from the exact log suffix. Clean full-log and snapshot recovery can resume
as an existing-only append session after an exact-extent check. Torn-tail valid-prefix recovery
remains read-only until copy-only repair creates a new clean log.

The focused 60-case recovery selection reports 59 passes and one expected Windows
canonical-symlink skip. GCC Debug and Release each pass 482/482 CTest cases, the production-only
build passes, both Python selections above pass, and the published PR3 head passes all 13 hosted
checks. The slice was retargeted, revalidated, and squash-merged through PR #6.

[ADR 0015](docs/decisions/0015-native-python-bindings-and-packaging.md) defines the implemented
final Phase 4 slice. Version 0.2.0 exposes `atlaslob.Engine` lazily over the private
`atlaslob._native_engine` pybind11 module while preserving import isolation for the independent
oracle. It provides strict full-batch preflight, immutable owned results, object/column/summary
batch modes, live and logged engines, clean writable recovery, torn valid-prefix read-only
recovery, and snapshot publication. Python conversion occurs before taking the per-engine mutex;
Python-free execution releases the GIL, and each batch holds one engine mutex so calls on that
engine cannot interleave.

PR4's local gate passes 482/482 CTest cases in both GCC Debug and Release, 354 Python tests with
two expected Windows canonical-symlink skips, the 11 campaign/fuzz tests, Ruff, strict mypy, and
formatting. `cibuildwheel==4.1.0` builds CPython 3.11-3.14 manylinux x86-64 wheels that pass
contents, auditwheel, and clean-container smoke checks. The PEP 517 source distribution also
builds, installs, and smokes in a clean environment. Real-binding tests cover batch-mode parity,
Unicode persistence paths, clean and torn recovery, same-engine noninterleaving, released-GIL
thread progress, and ownership after later mutation. All 15 hosted PR4 compiler, sanitizer,
fuzz-smoke, Python, formatting, differential, wheel, and source-distribution checks pass. PR #9
was retargeted, revalidated, and squash-merged as `075d29a`; the uncancelled workflow on that
`main` commit also passes. Current evidence status is indexed in
[the Phase 4 evidence record](docs/evidence/phase4/README.md).

## Phase 5 - Measured portfolio release

- [x] Frozen workloads and environment manifests.
- [x] Resumable 51-shape baseline orchestration and separate Linux `perf` capture.
- [ ] Baseline throughput, latency, allocation, and memory results.
- [ ] Linux `perf` profiles and hypothesis-driven experiments.
- [ ] Raw data, reproducible analysis, and limitations.
- [ ] Clean-clone review, evidence map, and tagged release.

[ADR 0016](docs/decisions/0016-reproducible-performance-evidence.md) freezes the Phase 5
measurement contract. The first slice adds opt-in benchmark builds, strict versioned evidence
schemas, deterministic workload materialization, single-observation native runners, allocation
instrumentation, analysis tooling, and threshold-free hosted smoke checks. It deliberately
publishes no performance result.

The second slice adds round-robin plan execution, strict resume without overwriting failed
attempts, complete campaign finalization, physical-host consistency checks across distinct
core/allocation/CPython build contexts, fixed Linux `perf` capture, and an exact native-host
runbook. The draft remains blocked on the later qualified-host evidence run.

Authoritative baseline and experiment measurements require a dedicated native Ubuntu 24.04
x86-64 host. WSL2, containers, and shared CI are limited to exploratory or smoke runs. The gateway
workload W11 remains visibly deferred to Phase 6.

## Phase 6 - Optional Linux systems extension

- [ ] Versioned protocol and golden byte fixtures.
- [ ] Incremental, bounded, fuzzed decoder.
- [ ] Nonblocking loopback `epoll` server.
- [ ] Partial reads/writes, bounded queues, and backpressure.
- [ ] Network fault matrix and ThreadSanitizer suite.

## Deferred until evidence justifies them

FOK, custom allocators, custom lock-free queues, TLS, live market feeds, strategies, kernel bypass,
DPDK, huge pages, and distributed recovery.
