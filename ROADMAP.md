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
- [ ] Hosted sanitizer evidence for the completed indexed-book slice.

The resting-book structure now provides ordered sides, stable level addresses, guarded level
preparation, a checked active-order index, direct cancellation, active-only ID reuse, safe
teardown, and cross-structure invariants. A 10,000-operation fixed-seed model stress test checks
the complete book after every mutation. Hosted sanitizer evidence remains open until the branch
can be published; matching begins as a separate Phase 2 layer.

ADR 0005 records the accepted ownership boundary: storage remains the sole node owner, a checked
non-owning index provides direct identity lookup, and one centralized path performs level unlink,
index removal, empty-level cleanup, and storage destruction.

## Phase 2 - Matching MVP

- [ ] Rest non-marketable limit GTC orders.
- [ ] Match one level with full and partial fills.
- [ ] Sweep multiple orders and price levels.
- [ ] Support market IOC and limit IOC residual behavior.
- [ ] Replace with cancel-and-new priority reset.
- [ ] Produce normalized event and state digests.

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
