# 2026-07-24 - Deterministic matching evidence

## Goal

Close Phase 2 with evidence that can detect any visible state or event divergence after each
command, while keeping the format independently reproducible and the production API free of
mutable implementation details.

## Implementation

- Added exact best-price/FIFO `BookSnapshot`, `PriceLevelSnapshot`, and `OrderSnapshot` values.
- Added versioned, fixed-width, big-endian state and event encodings with separate `ATLSST01` and
  `ATLSEV01` domains and a self-contained incremental SHA-256 implementation.
- Added `MatchingEngine::snapshot()` and `MatchingEngine::state_digest()`.
- Added `atlas_cli engine-fixture <instrument_id> <path>` with per-command event/state hashes,
  deterministic summaries, and explicit exit-code precedence.
- Reused one parser-to-domain normalization boundary for both development fixture modes.
- Added an independent map/deque reference engine with linear identity lookup and separately
  implemented command validation, matching, mutation, and normalized events.

## Important edge cases

- A fresh snapshot has last sequence zero; a domain rejection changes the last sequence and state
  digest without changing any resting order.
- Parse failures occur before engine submission, do not consume sequences, and outrank ordinary
  rejection in the fixture's final exit code.
- State serialization preserves bid/ask best-price order and FIFO order within each level.
- Event serialization derives the discriminator from the variant and includes both batch identity
  and every event header.
- Absent optional IDs and top-of-book sides still have fixed-width canonical representations.
- State and event encodings are domain-separated and versioned; the hashes are regression
  evidence, not authentication.

## Evidence status

| Gate | Status |
| --- | --- |
| SHA-256 published vectors | Passed |
| Independently generated state/event golden hashes | Passed |
| Snapshot and digest field-sensitivity tests | Passed |
| Committed/rejected/malformed engine golden fixtures | Passed |
| Independent model comparison | Passed, 10,000 mixed plus 66 directed commands |
| Same-seed transcript rerun | Passed, 2,500 commands |
| Debug CTest suite | Passed, 254/254 |
| Release CTest suite | Passed, 254/254 |
| Production-only build | Passed |
| Local Clang and ASan/UBSan | Unavailable unless a compatible compiler is installed |
| Hosted GCC, Clang, and ASan/UBSan jobs | Pending publication |

An independent review found no blocker or high-severity issue. Its medium finding—that the random
stream deliberately bounded the book below the configured capacity—was closed with the directed
exact-capacity scenario and explicit path counters. Its two low findings were closed by storing
actual engine digests in the rerun transcript and directly testing normal/exhausted snapshot
sequence mapping.

## Scope boundary

Phase 2 is complete locally. The in-test C++ model is deliberately straightforward and
structurally independent, but Phase 3 still adds a separately packaged Python oracle, persistent
failure cases, shrinking, longer campaigns, and fuzzing. Replay logs, multi-instrument routing,
bindings, and benchmarks also remain later phases.
