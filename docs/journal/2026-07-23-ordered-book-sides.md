# 2026-07-23 - Ordered bid and ask sides

## Goal

Turn the existing one-price FIFO primitive into deterministic bid and ask collections with
best-price access, without adding the global active-order index, command cancellation, or matching.

## Decisions

- Own price levels in side-specific ordered maps.
- Use descending bid order and ascending ask order so the first level is always best.
- Keep each non-movable `PriceLevel` behind `unique_ptr`, preserving its address while unrelated
  map entries are inserted or removed.
- Require map keys and level prices to agree.
- Require every mapped level to be nonnull and nonempty at completed operation boundaries.
- Keep level-management and invariant failures separate from public domain rejections.
- Preserve the terminal-removal lifetime rule established by ADR 0003.

ADR 0004 records the accepted ownership, ordering, cleanup, and error policy.

## Scope boundary

This slice is limited to ordered side ownership, best-to-worst traversal, best-price access,
checked level creation/removal, and side-local invariants. It does not add the global active-order
index, direct command cancellation, whole-book invariants, matching, replacement execution,
digests, Python, replay, or benchmarks.

## Evidence

Production and automated tests demonstrate:

- Highest-bid and lowest-ask best-price behavior.
- Deterministic best-to-worst traversal.
- Stable `PriceLevel` addresses while other prices are inserted.
- Failure-atomic duplicate creation and invalid removal.
- Immediate cleanup of an empty level.
- Detection of null ownership, key/price disagreement, empty mapped levels, and level corruption.
- Passing local debug compilation, the complete core suite, the pinned formatting gate, and
  whitespace validation. Hosted compiler and sanitizer evidence remains a pull-request gate.

The ordered-side roadmap checkbox is complete. The next slice adds the global active-order index,
direct cancellation, safe book teardown, and cross-structure invariants.
