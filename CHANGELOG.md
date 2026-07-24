# Changelog

All notable changes to AtlasLOB will be documented here.

The format is based on Keep a Changelog, and public releases will follow semantic versioning.

## [Unreleased]

### Added

- C++20 domain library, CLI, and unit-test foundation.
- Strong identifier, price, quantity, and sequence values.
- Deterministic new-order validation and demonstration command.
- Client identity plus new, cancel, and replace command variants.
- Explicit command, event, completion, and rejection vocabularies.
- Normalized value-only event schemas and deterministic sequence/identity policy.
- Canonical text command fixtures with separate parse and domain-validation outcomes.
- Pinned GoogleTest 1.17.0 test infrastructure.
- Private `atlas_core` implementation layer for mutable book internals.
- Stable heap-owned order nodes with checked creation and destruction.
- Checked, non-owning intrusive FIFO price levels with aggregate quantity and order count.
- Cycle-safe price-level invariants and fixed-seed storage/level stress coverage.
- Ordered bid and ask maps with stable level addresses, best-price access, and guarded level
  preparation.
- Checked active-order indexing, direct indexed cancellation, empty-level cleanup, safe live-book
  teardown, and full storage/index/FIFO invariants.
- Fixed-seed 10,000-operation model stress coverage for mixed resting, reduction, and cancellation.
- ADR 0003 documenting node ownership, pointer invalidation, and internal error boundaries.
- ADRs 0004 and 0005 documenting ordered-side and indexed-book ownership boundaries.
- GCC, Clang, formatting, ASan, and UBSan workflow definitions.
- Initial semantic contract, roadmap, and architecture decision record.
