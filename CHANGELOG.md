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
- Monotonic command sequencing, deterministic pure/state admission, and explicit internal failure
  boundaries.
- Read-only best-price/FIFO match planning with final active-order capacity projection.
- Immutable command-owned event batches with exact preallocated slots and contiguous headers.
- Allocation-before-mutation preparation for a future GTC residual, including rollback and
  allocation-free publication at existing or detached price levels.
- Atomic New execution for limit GTC, limit IOC, and market IOC with passive-price trades,
  price-time sweeps, residual handling, and coalesced final top-of-book events.
- Sequenced Cancel execution with current-residual cancellation events and direct terminal
  removal.
- All-preflight passive reduction batches and allocation-free final top-of-book projection.
- Deterministic exception evidence for exact prepared-residual rollback after the command sequence
  has been consumed.
- Atomic Replace execution with retained identity/side, new FIFO priority, passive-price matching,
  final-state capacity checks, and one normalized event batch.
- Replacement-specific residual preparation and all-preflight mutation with same-level aggregate
  relief and exact exception rollback.
- Public non-owning-detail-free `MatchingEngine` PImpl with typed/variant command execution,
  policy configuration, sequence observers, active count, and top-of-book values.
- ADR 0003 documenting node ownership, pointer invalidation, and internal error boundaries.
- ADRs 0004 through 0008 documenting ordered-side, indexed-book, admission, planning,
  execution-preparation, atomic New/Cancel, atomic Replace, and public API boundaries.
- GCC, Clang, formatting, ASan, and UBSan workflow definitions.
- Initial semantic contract, roadmap, and architecture decision record.
