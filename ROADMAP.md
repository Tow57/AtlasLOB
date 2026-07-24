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

- [ ] Straightforward Python reference model.
- [ ] Seeded valid and invalid command generation.
- [ ] Per-command differential comparison.
- [ ] Failure persistence and shrinking.
- [ ] Fixed CI corpus, long campaigns, and fuzzing.

## Phase 4 - Deterministic infrastructure and Python

- [ ] Multi-instrument routing and global sequencing.
- [ ] Explicit append-only command log codec.
- [ ] Inspector, replay, corruption, and truncated-tail tests.
- [ ] pybind11 batch API and installable Python package.
- [ ] Optional canonical snapshot plus log-suffix recovery.

## Phase 5 - Measured portfolio release

- [ ] Frozen workloads and environment manifests.
- [ ] Baseline throughput, latency, allocation, and memory results.
- [ ] Linux `perf` profiles and hypothesis-driven experiments.
- [ ] Raw data, reproducible analysis, and limitations.
- [ ] Clean-clone review, evidence map, and tagged release.

## Phase 6 - Optional Linux systems extension

- [ ] Versioned protocol and golden byte fixtures.
- [ ] Incremental, bounded, fuzzed decoder.
- [ ] Nonblocking loopback `epoll` server.
- [ ] Partial reads/writes, bounded queues, and backpressure.
- [ ] Network fault matrix and ThreadSanitizer suite.

## Deferred until evidence justifies them

FOK, custom allocators, custom lock-free queues, TLS, live market feeds, strategies, kernel bypass,
DPDK, huge pages, and distributed recovery.
