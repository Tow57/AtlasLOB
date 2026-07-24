# 2026-07-24 - Matching execution foundation

## Goal

Establish the sequencing, admission, planning, event-ownership, and allocation-before-mutation
contracts needed to execute Phase 2 commands atomically.

## Decisions

- Issue a nonzero authoritative sequence before pure or state validation so every domain command,
  including a rejection, consumes one sequence.
- Keep parse failures upstream and keep sequence exhaustion, invalid configuration, corruption,
  and resource failure outside public rejection vocabulary.
- Apply deterministic state-validation precedence, while deferring capacity until the command's
  final active-order count can be projected.
- Plan opposite-side matches with immutable values in best-price/FIFO order and revalidate those
  values immediately before mutation.
- Own every command's complete normalized result in one immutable `EventBatch` with contiguous
  event indexes.
- Allocate exact event capacity and a possible GTC resting residual before any fill is applied.
- Stage the residual in private storage/index/FIFO state so abandonment restores the exact visible
  book and commit can publish without allocation.

ADR 0006 records the accepted contract.

## Scope boundary

This slice intentionally stops before command mutation. It does not execute trades, generate
accepted command batches, apply cancel or replace, expose a public matching engine, compute
digests, or add an engine fixture.

## Evidence status

| Gate | Status |
| --- | --- |
| Sequencer first/max/exhaustion tests | Passed |
| Pure and state admission precedence | Passed |
| Full/partial, multi-order, multi-level, limit/IOC/market plans | Passed |
| Checked final active-count projection | Passed |
| Event ownership, indexing, and every payload alternative | Passed |
| Prepared residual commit, rollback, move, and failure atomicity | Passed |
| Independent correctness review | Passed; no blocker/high findings |
| Debug and Release CTest suites | Passed, 157/157 each |
| Production-only build | Passed |
| Pinned formatting gate | Passed |
| Hosted GCC, Clang, and ASan/UBSan jobs | Pending branch publication |

The next slice binds a verified plan to mutable passive nodes, prebuilds the complete event batch,
and applies New and Cancel commands through one no-allocation mutation boundary.
