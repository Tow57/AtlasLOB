# 2026-07-23 - Stable storage and FIFO price levels

## Goal

Introduce the smallest mutable core that can safely own resting orders and organize one price into
FIFO priority, without adding book sides, a global active-order index, cancellation execution, or
matching.

## Decisions

- Added a private `atlas_core` implementation layer linked to the public domain contract.
- Kept ownership in `HeapOrderStorage` and made every price-level link non-owning.
- Used `unique_ptr` pointees inside an order-ID hash table so node addresses stay stable as buckets
  grow and the owner can later be replaced behind a narrow storage interface.
- Required nodes to be unlinked before destruction and documented the future complete removal
  order: level unlink, active-index removal, empty-level removal, then storage destruction.
- Kept `OrderNode` destruction inaccessible to borrowers and enforced nonempty/linked teardown
  violations in every build.
- Made append, erase, and partial reduction checked and failure-atomic.
- Kept internal storage/level errors and invariant defects separate from public `RejectReason`
  values. Allocation failure remains a propagated resource failure.
- Added cycle-safe invariant validation and fixed-seed mutation stress coverage.
- Isolated deliberate-corruption friendship in a separately compiled test-only core target; the
  production core has no test-access backdoor.

## Scope boundary

This slice completes stable node ownership and one-price FIFO levels only. Ordered bid/ask sides,
best-price lookup, the global active-order index, executable cancellation and replacement,
empty-level cleanup in a book, matching, replay, Python, and benchmarks remain deferred.

## Evidence status

The following gates must be recorded as passing only after they run against the final commit:

| Gate | Status |
| --- | --- |
| Debug GCC configure, build, and full CTest suite | Passed: 63/63 tests |
| Release GCC configure, build, and full CTest suite | Passed: 63/63 tests |
| `BUILD_TESTING=OFF` production build | Passed |
| Fixed-seed storage/level stress test | Passed: 10,000 mutations |
| ASan and UBSan stress suite | Pending hosted Linux CI |
| Pinned clang-format 18.1.8 check | Passed |
| `git diff --check` and source-tree path/secret audit | Passed |
| Clean-worktree audit | Passed after local commit |
| Hosted GCC build and tests | Pending |
| Hosted Clang build and tests | Pending |
| Hosted ASan/UBSan job | Pending |
| Hosted formatting job | Pending |

The local MinGW toolchain does not include its AddressSanitizer or UndefinedBehaviorSanitizer
runtime libraries, so the pinned hosted Linux sanitizer job remains the authority for that gate.

No latency, throughput, allocation-rate, or scalability claim is made by this slice.

## Next decision

Choose the ordered bid/ask containers and build the non-owning global active-order index. The next
slice must prove direct indexed cancellation, empty-level cleanup, best-price behavior, and
whole-book invariants before matching begins.
