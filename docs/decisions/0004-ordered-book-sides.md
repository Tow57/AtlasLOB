# ADR 0004: Ordered book sides and stable price-level ownership

- Status: accepted
- Date: 2026-07-23

## Context

The existing `PriceLevel` represents FIFO priority at one price but does not order prices or
identify the best bid and ask. Matching and cancellation need ordered sides whose level addresses
remain stable while other prices are inserted or removed. This matters because each resting
`OrderNode` keeps a non-owning backlink to its current level, and `PriceLevel` is intentionally
non-copyable and non-movable.

A completed book operation must also have an unambiguous structural boundary: mapped levels are
live queues, not placeholders. Leaving empty levels in a side would make best-price access and
whole-book invariants depend on cleanup that may or may not already have happened.

## Decision

- Keep `BookSide` in the private `atlas_core` implementation layer. It is non-copyable and
  non-movable and owns price levels, but it does not own any order nodes.
- Store levels as
  `std::map<PriceTicks, std::unique_ptr<PriceLevel>, Comparator>`.
- Order the buy side with a descending comparator and the sell side with an ascending comparator.
  `begin()` is therefore the best price for either side: highest bid or lowest ask.
- Heap-own each `PriceLevel` through `unique_ptr`. Map rebalancing may reorganize map nodes but does
  not move the pointed-to level, so a level address remains stable until that exact price is
  explicitly removed.
- Provide checked operations for level lookup, creation, best-level access, and removal. Creating
  an already-present price returns the existing level. Removing a missing or nonempty level fails
  without mutation.
- A mapped level must be nonnull, have the same price as its map key, and contain at least one
  order at every completed operation or command boundary. A temporarily prepared empty level is
  permitted only behind a move-only preparation guard. Destroying an uncommitted guard rolls back
  a newly created empty level; committing requires a nonempty, valid level. The guard must not
  outlive its `BookSide`, and callers must not erase its level while it is active.
- Remove a level immediately after its last node has been unlinked. The complete terminal-removal
  order remains: unlink the node, remove it from the future active-order index, remove the now-empty
  level, and destroy the node through storage last.
- Expose only best-to-worst read-only traversal and aggregate views outside the mutation helpers.
  Matching will receive a narrow internal path to the mutable best level rather than general
  ownership access.
- Validate side invariants explicitly: comparator order, nonnull ownership, key/price agreement,
  nonempty levels, and every contained level's existing FIFO invariants.
- Treat lookup misuse, invalid removal, and invariant failure as internal core errors. They are not
  public `RejectReason` values. Allocation failure continues to propagate as a resource failure.

## Alternatives considered

- Storing `PriceLevel` directly as the map value is technically viable: a node-based `std::map`
  can construct a non-movable mapped value in place and preserves references except to an erased
  element. The baseline still uses `unique_ptr` because the pointee ownership boundary is explicit,
  level preparation and rollback can be represented directly, and backlink invalidation is easier
  to review independently of the map node.
- An unordered map plus cached best-price value was rejected because every insertion and removal
  would have to maintain a second ordering structure or rescan prices.
- A sorted vector or flat map was rejected for the baseline because insertion can relocate values
  and would complicate the stable-address guarantee required by node backlinks.
- Using one ascending map and reversing bid traversal was rejected because `begin()` would not
  have one side-independent meaning in matching code.
- Retaining empty levels for reuse was rejected because it weakens best-price access, complicates
  invariants, and makes capacity accounting depend on stale implementation state.

## Consequences

Both sides have deterministic best-to-worst traversal and logarithmic price lookup, insertion, and
removal. Each live price level incurs a separate allocation, but its address is stable for the
entire lifetime of that price. Callers must coordinate empty-level cleanup with node unlinking and
must not retain a level pointer after the corresponding map entry is removed.

This decision establishes the ordered-side substrate only. The global active-order index, command
cancellation, whole-book invariants, and matching remain separate slices.

## Evidence

- Book-side tests cover bid/ask comparator direction, best-price access, stable level addresses
  across map growth and unrelated removal, checked failure paths, and exact identity removal.
- Preparation-guard tests cover rollback, commit, move construction, move assignment, and
  existing-level handling.
- Deliberate-corruption tests detect null mapped ownership, key/price disagreement, and detailed
  price-level invariant failures.
- The complete core suite passes with the ordered-side invariant checked at completed boundaries.
