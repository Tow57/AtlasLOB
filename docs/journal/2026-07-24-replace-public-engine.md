# 2026-07-24 - Atomic Replace and public matching engine

## Goal

Complete executable Replace without exposing cancel-and-new intermediate state, then place the
private book/executor stack behind a small supported library API.

## Implementation

- `CommandExecutor` admits Replace once, copies the exact old target, synthesizes a retained-side
  limit GTC replacement, plans passive trades, and projects final capacity and top of book.
- `InstrumentBook` has a replacement-specific preparation guard and all-preflight mutation
  transaction. Same-level preflight credits the old quantity and priority before checking the new
  residual.
- Event production follows accepted, replaced, canceled-old, done-old, trades, new terminal/resting
  outcome, and optional final book-changed order.
- A hidden replacement node is allocated before event construction. Dropping its guard restores
  storage, index, staging level, and visible state.
- `atlaslob::MatchingEngine` exposes typed and variant command execution, normalized event batches,
  policy configuration, sequence state, active count, and top of book through a PImpl.

## Important edge cases

- A same-price replacement goes to the FIFO tail even when price, quantity, and visible aggregate
  are unchanged.
- The old quantity is subtracted before checked same-level addition, so net-neutral replacement
  succeeds at maximum aggregate representation.
- A fully executed replacement leaves both the old and new IDs terminal and available for later
  reuse.
- Capacity is evaluated on the final state, so replacing at the configured active-order limit is
  valid when the final count remains within that limit.
- Allocation failure after replacement preparation consumes the sequence but leaves the old order,
  passive orders, top of book, and active count unchanged.

## Scope boundary

This slice does not yet add canonical state/event digests, the executable engine fixture, or the
independent command-stream model stress test. Those are the final Phase 2 evidence slices.

## Evidence status

| Gate | Status |
| --- | --- |
| Replace projection, preparation, transaction, and executor cases | Passed |
| Public `MatchingEngine` API cases | Passed |
| Deterministic replacement rollback injection | Passed |
| Debug and Release CTest suites | Passed, 235/235 each |
| Independent correctness review | Passed after fixing the one medium finding |
| Production-only build | Passed |
| Pinned formatting and repository hygiene | Passed |
| Local Clang and ASan/UBSan | Unavailable unless a compatible compiler is installed |
| Hosted GCC, Clang, and ASan/UBSan jobs | Pending publication |

The review finding concerned default move behavior for `EngineResult`: its source optional could
remain engaged around a moved-from event batch. Custom move operations now explicitly reset the
source, observers also reject empty batches, and move construction/assignment are covered
directly. The review's useful low-risk gaps were addressed with exact public Replace
sequence/header/payload assertions, partial-passive/full-replacement execution, and removal of a
dangling address comparison from a test.
