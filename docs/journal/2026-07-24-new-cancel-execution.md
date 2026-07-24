# 2026-07-24 - Atomic New and Cancel execution

## Goal

Turn the Phase 2 planning and allocation foundation into executable price-time matching and
sequenced cancellation without exposing partial book state or partial event output.

## Implementation

- `CommandExecutor` sequences and admits typed New and Cancel commands.
- Domain rejects produce one owned normalized rejection batch, including a submitted zero
  instrument when structural validation fails.
- Accepted New commands plan and rebind every passive in exact best-price/FIFO order, project the
  final active count and top of book, prepare any GTC residual, prebuild all events, then apply one
  no-allocation batch.
- Accepted Cancel commands copy and project one exact active target, prebuild accepted/canceled/done
  events, then apply a terminal reduction through the same batch boundary.
- Limit GTC, limit IOC, and market IOC distinguish filled, resting, IOC-canceled, and
  market-exhausted outcomes.
- A test-only failpoint throws after residual preparation and before event allocation, proving
  sequence consumption plus exact storage/index/staging/top rollback.
- Default sequencing requires an empty book; restored books provide a next sequence greater than
  every active priority.

## Event policy

- New: accepted, trades in execution order, rested or done, optional final book-changed.
- Cancel: accepted, canceled, done, optional final book-changed.
- The final book-changed event is coalesced from the complete price-and-aggregate tuple.
- IOC and market residuals do not emit canceled events; passive fills do not emit done events.

## Scope boundary

This slice does not execute Replace, expose the public multi-command `MatchingEngine`, compute
state/event digests, add an engine fixture, or add command-stream model stress. Those remain the
next Phase 2 slices.

## Evidence status

| Gate | Status |
| --- | --- |
| New/Cancel end-to-end behavior | Passed |
| Projection and all-preflight mutation tests | Passed |
| Exception rollback after prepared residual | Passed |
| Independent correctness review | Passed; no blocker/high findings |
| Debug and Release CTest suites | Passed, 204/204 each |
| Production-only build | Passed |
| Pinned formatting gate | Passed |
| Local Clang and ASan/UBSan | Unavailable; `clang++` is not installed |
| Hosted GCC, Clang, and ASan/UBSan jobs | Pending branch publication |

The behavior suites cover both sides, multi-level sweeps, FIFO, partial fills, inclusive limits,
final-state capacity, aggregate overflow, all IOC/market terminal outcomes, cancellation after a
real partial fill, ID reuse, sequence exhaustion/resume, exact payloads, and allocation rollback.
