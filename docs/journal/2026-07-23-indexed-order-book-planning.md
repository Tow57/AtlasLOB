# 2026-07-23 - Indexed order-book and cancellation

## Goal

Lock and implement the ownership, lookup, removal, teardown, and invariant contracts for the final
resting-book structural slice.

## Decisions

- Preserve `HeapOrderStorage` as the sole node owner.
- Add a checked, non-owning active-order index keyed by `OrderId`.
- Compose storage, bid/ask sides, and the index behind a private `InstrumentBook`.
- Resolve cancellations by index lookup and node backlinks without scanning a level.
- Centralize the terminal-removal order: level unlink, index removal, empty-level removal, then
  storage destruction.
- Drain live books explicitly through that same path during teardown.
- Keep allocation and internal consistency failures separate from public rejection vocabulary.
- Validate correspondence among every storage entry, index entry, mapped level, and FIFO node.

ADR 0005 records the accepted contract.

## Implementation

- `ActiveOrderIndex` provides checked exact-pointer insertion, lookup, removal, and invariant
  reporting without owning nodes.
- `InstrumentBook` composes storage, ordered bid/ask sides, and the active index behind one
  mutation boundary.
- Resting insertion allocates storage, guarded level state, and the index entry before publishing
  a node in a FIFO. Every returned failure is atomic; an impossible rollback failure is fatal.
- Resting insertion rejects a price that would cross the opposite best before mutation. Matching
  will consume marketable orders before calling this passive-book operation.
- Cancellation validates the complete ownership graph, locates its node by ID, then performs the
  required unlink, index removal, empty-level removal, and storage destruction order.
- Cross-structure validation proves exact storage/index/FIFO correspondence through exact node
  identity checks and equal cardinalities, and detects invalid client or instrument identity,
  side/price/backlink faults, crossed books, and component-local corruption.

## Scope boundary

This slice implements direct cancellation but does not add engine sequencing, stateful command
rejection, cancel events, replacement, matching, digests, Python, replay, or benchmarks.

## Evidence status

| Gate | Status |
| --- | --- |
| Active-index success and failure tests | Passed |
| Direct singleton/head/middle/tail cancellation tests | Passed |
| Empty-level cleanup and ID-reuse tests | Passed |
| Whole-book invariant and corruption tests | Passed |
| Fixed-seed mixed structural stress | Passed, 10,000 operations |
| Safe live-book teardown test | Passed |
| Debug and release CTest suites | Passed, 110/110 each |
| Production-only build | Passed |
| Hosted GCC, Clang, and ASan/UBSan jobs | Pending branch publication |
| Pinned formatting and source-tree audits | Passed locally |

No latency, throughput, or complexity-performance claim is supported by this record.
