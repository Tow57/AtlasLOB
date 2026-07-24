# ADR 0005: Indexed order-book ownership and direct cancellation

- Status: accepted
- Date: 2026-07-23

## Context

Ordered bid and ask sides can locate a price, but a cancellation starts with an order ID. Searching
both sides, then every level and FIFO, would make identity lookup depend on book topology and would
duplicate pointer-lifetime logic across cancellation, replacement, and matching.

The book also combines three different relationships: storage owns order nodes, price levels link
them in FIFO order, and an order-ID index locates them. Removing a node in the wrong order could
leave the index or a level pointing at destroyed storage. Normal teardown must obey the same rules
even when a book is destroyed with live orders.

## Decision

- Keep `ActiveOrderIndex` and `InstrumentBook` in the private `atlas_core` implementation layer.
  Both are non-copyable and non-movable.
- `HeapOrderStorage` remains the sole owner of every resting `OrderNode`. The active index stores
  non-owning `OrderNode*` values, and price levels retain only their existing non-owning intrusive
  links and backlinks.
- Implement the active index with
  `std::unordered_map<OrderId, OrderNode*, StrongValueHash<OrderId>>`. Expose checked insertion,
  exact lookup, exact-pointer removal, size, and invariant inspection rather than exposing the
  container.
- Reject a zero ID, null pointer, key/node-ID disagreement, duplicate ID, missing ID, or
  wrong-pointer removal as an internal index error. Every returned index error is failure-atomic.
- Compose one bid side, one ask side, owning storage, and the active index inside
  `InstrumentBook`. It provides narrow checked operations for resting insertion, direct indexed
  lookup/removal, top-of-book inspection, explicit teardown, and whole-book invariant validation.
- Locate cancellation targets through the active index only. After the ID lookup, use the node's
  side and `PriceLevel` backlink to unlink it directly; do not scan prices or FIFO queues.
- Validate the complete storage/index/level relationship before the first terminal-removal
  mutation. Then use one centralized removal path in this exact order:
  1. Save any values the caller still needs.
  2. Unlink the node from its `PriceLevel`.
  3. Remove the exact node pointer from the active-order index.
  4. Remove the price level if it became empty.
  5. Destroy the node through `HeapOrderStorage` last.
- Never dereference the node after storage destruction. Never retain a level pointer after its map
  entry is removed.
- Resting insertion is transactional. Allocate the node, guarded price level, and index entry
  before linking the node into the FIFO, then commit the guarded level. A returned error leaves
  storage, index, sides, links, aggregates, and counts exactly as they were before the call.
- Allocation failure propagates as a resource failure; it is not converted to
  `RejectReason::capacity_exceeded`. Index, book, storage, level, invariant, and teardown failures
  remain internal errors and are never public `RejectReason` values.
- `InstrumentBook` has an explicit destructor that drains every live order through the centralized
  removal path before member destruction. Destruction performs no allocation. An inconsistent
  ownership graph during teardown is an internal contract violation and terminates rather than
  guessing which dangling pointers are safe to ignore.
- Validate whole-book invariants at completed operation boundaries:
  - both side-local invariant reports are valid;
  - every mapped level is nonnull, nonempty, and owned by exactly one side;
  - every traversed node has the containing side and price, the configured instrument, and a
    reciprocal level backlink;
  - every active ID occurs exactly once in storage, the index, and one FIFO;
  - each index key equals its node's ID and points to the same node owned by storage;
  - storage size, index size, and total FIFO traversal count agree;
  - no node pointer or order ID appears in more than one traversal;
  - best bid is strictly below best ask at a completed resting-book boundary.
- Provide test-only deliberate-corruption access only through the separately compiled test core.
  Production types contain no generally accessible ownership or map mutation backdoor.

## Alternatives considered

- Scanning both sides for every cancellation was rejected because identity lookup would depend on
  price depth and queue length, and cancellation would need to rediscover information already
  stored in the node's backlink.
- Making the index own nodes was rejected because storage would no longer be the single lifetime
  authority and level/index cleanup could destroy the same node through competing owners.
- Storing iterators instead of node pointers was rejected because it would couple the index to a
  particular level-container representation and complicate safe level erasure.
- Erasing storage before the index or price level was rejected because both structures would
  retain pointers to a destroyed node.
- Clearing containers independently in the destructor was rejected because their member
  destruction order does not express the cross-container unlink contract.
- Translating internal inconsistencies into ordinary domain rejections was rejected because a
  client command cannot safely recover an already-corrupted ownership graph.

## Consequences

Cancellation obtains its target without scanning price levels or FIFO queues, while one owner and
one terminal-removal path make pointer invalidation reviewable. The index adds one non-owning hash
entry per active order and every book mutation must keep four views—storage, index, side, and
level—consistent.

This decision does not yet define command sequencing, public cancellation events, replacement,
matching, event digests, or performance claims. Those remain later Phase 2 slices.

## Evidence

The implementation provides:

- checked index insertion, lookup, and exact-pointer removal, including atomic failure paths;
- cancellation of singleton, head, middle, and tail nodes at best and non-best prices;
- immediate empty-level cleanup and active-only ID reuse;
- safe explicit teardown of a book containing live orders;
- deliberate detection of missing, duplicate, ghost, wrong-pointer, wrong-side, wrong-price, and
  wrong-instrument relationships;
- fixed-seed mixed resting/cancellation stress with full invariants after every operation; and
- debug and release GCC suites, pinned formatting, and a production-only build.

The current local evidence is 110/110 tests in both Debug and Release, including 19 focused
`InstrumentBook` cases and a 10,000-operation model-based structural stress test. Hosted GCC,
Clang, and ASan/UBSan jobs remain a pull-request gate and are not claimed by this record yet.
