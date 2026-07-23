# 2026-07-22 - Domain contract

## Goal

Complete the command and event vocabulary before adding mutable order-book state, while preserving
deterministic validation and a reproducible adapter-level fixture.

## Decisions

- Added client identity and explicit operational enum values before any wire representation exists.
- Kept parsing failures separate from domain rejects.
- Assigned future engine sequence to every well-formed domain submission, including rejects.
- Chose active-only order-ID uniqueness; terminal-ID retention remains deferred.
- Defined normalized events as value-only variants without machine or container details, using the
  variant alternative as the single event-type discriminator.
- Adopted GoogleTest as a pinned, test-only dependency because the growing semantic matrix had
  outgrown the bootstrap test executable.

## Evidence

The change is complete only when:

```text
cmake --preset dev-gcc
cmake --build --preset dev-gcc
ctest --preset dev-gcc

cmake --preset release
cmake --build --preset release
ctest --preset release
```

The hosted GCC, Clang, ASan/UBSan, and pinned formatting jobs must also pass. Exact run links are
added after the branch is published.

## Next decision

Choose and document the baseline stable owner for resting order nodes, then implement a non-owning
intrusive FIFO price level without bid/ask maps or matching.
