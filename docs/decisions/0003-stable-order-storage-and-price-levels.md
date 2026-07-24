# ADR 0003: Stable order storage and intrusive price levels

- Status: accepted
- Date: 2026-07-23

## Context

Resting orders need one clear owner while price levels need direct FIFO insertion and removal. If
ownership is mixed into queue links, or if a container operation can silently change a node
address, the future active-order index and matching loop could retain dangling pointers. Mutation
failures must also be distinguishable from client-domain rejections.

## Decision

- Keep `OrderNode`, `PriceLevel`, and storage implementations in the private `atlas_core`
  implementation layer. All three are non-copyable and non-movable.
- `HeapOrderStorage` is the sole owner of baseline nodes through
  `std::unordered_map<OrderId, std::unique_ptr<OrderNode, OrderNodeDeleter>,
  StrongValueHash<OrderId>>`.
- Bucket growth may move `unique_ptr` objects, but it does not move their heap-allocated
  `OrderNode` pointees. A node address therefore remains stable until that exact node is explicitly
  destroyed.
- `OrderStorage` exposes checked create, destroy, and size operations. Duplicate creation and an
  invalid destroy fail without mutation. A node must be unlinked before destruction, and its
  destructor is inaccessible to non-owning borrowers. The `unique_ptr` deleter can be constructed
  only by `HeapOrderStorage`, preventing a borrower from promoting a raw observer into an owner.
- Allocation failure propagates as a resource failure. It is not translated into a client-visible
  `RejectReason`.
- Every `OrderNode*` link is non-owning. A node stores its immutable identity, side, price, and
  priority sequence; its remaining quantity; its previous and next FIFO neighbors; and a backlink
  to its current `PriceLevel`.
- `PriceLevel` is a non-owning intrusive FIFO. It stores price, aggregate remaining quantity, order
  count, and head and tail pointers, while storage retains node ownership.
- `append` requires a positive remaining quantity, matching price, no existing links or level
  backlink, and a priority sequence strictly later than the current tail. Aggregate overflow and
  all preconditions are checked before any mutation.
- `erase` unlinks a known member directly without scanning, handles singleton/head/middle/tail
  cases, updates aggregate and count, and clears the removed node's previous, next, and level
  pointers.
- `reduce_remaining` requires a reduction strictly between zero and the node's remaining quantity.
  Full removal is expressed through `erase`, so there is one unlink path.
- Explicit, cycle-safe invariant validation checks empty boundaries, reciprocal links, backlinks,
  price agreement, positive remaining quantities, strict priority order, traversal count, and
  recomputed aggregate quantity.
- Deliberate-corruption helpers are friends only in a separately compiled test core. The
  production `atlas_core` definition contains no test-access friendship.
- Price-level and storage failures are internal component errors, not public domain rejections.
  Invariant failures indicate implementation defects. Storage and level teardown enforce their
  unlink/empty lifetime contracts even when full traversal invariants are disabled.
- The required future terminal-removal order is:
  **unlink from the price level -> remove from the active-order index -> remove the empty price
  level -> destroy through storage**. A later book implementation must not destroy a node while
  any queue or index pointer can still reach it.

## Alternatives considered

- Owning nodes in each price level was rejected because cancellation needs one global owner and
  moving ownership between levels would complicate pointer lifetime and replacement.
- Storing queue entries separately from orders was rejected because direct cancellation would
  require another allocation or a level scan.
- Using a linked container as both owner and FIFO was rejected because it couples global identity
  lookup to one price-level organization.
- Storing values directly in the hash table was not selected. Although standard rehash preserves
  references to elements, `unique_ptr` makes ownership and pointee lifetime explicit, accommodates
  a non-movable node, and leaves a narrow seam for replacing heap allocation with a measured pool.
- Converting allocation or invariant failures into `RejectReason::capacity_exceeded` was rejected
  because client input did not cause an ordinary domain decision and the engine may no longer be
  able to guarantee safe continuation.

## Consequences

Stable addresses make future non-owning level links and the active-order index viable. The price
level can append and erase a known node without scanning, but callers must obey the documented
invalidation order. The baseline allocates each `OrderNode` individually; replacing that ownership
strategy with a measured pool remains future work.

## Evidence

- Core storage tests exercise duplicate creation, checked destruction, initialized links, distinct
  addresses, and address stability across hash-table growth.
- Price-level tests exercise FIFO mutation, aggregate accounting, atomic failure paths, and
  deliberate invariant corruption.
- A fixed-seed stress test validates invariants after every storage/level mutation under sanitizer
  configurations.
