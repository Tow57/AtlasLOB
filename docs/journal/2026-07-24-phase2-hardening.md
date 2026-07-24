# 2026-07-24 - Phase 2 safety hardening

## Goal

Close the actionable findings from a second full Phase 0–2 audit without changing matching
semantics or expanding the public engine's scope.

## Changes

- Replaced the publicly mutable `EngineResult` batch/error pair with private mutually exclusive
  state. Validated factories construct either a nonempty event batch or a concrete engine error;
  accessors are read-only and moved-from results are explicitly inert.
- Replaced `PreparedRest`'s replacement-old raw pointer with stable order identity. The owning book
  pins that exact ID against direct reduction, removal, and cancellation until the replacement
  commits or rolls back. Unrelated orders remain mutable.
- Added whole-book validation for replacement pins and covered move construction, move assignment,
  abandonment, deliberate corruption, and successful atomic commit.
- Made successful `PreparedLevel` commit clear its observer pointer so the guard becomes false and
  cannot expose a level after its lifetime ends.
- Kept `ATLAS_ENABLE_INVARIANTS` private to the core targets.

## Added evidence

- Deterministic failure injection at detached-level, staging-level, storage-node, and active-index
  preparation stages proves exceptions leave no visible or hidden mutation.
- Public execution proves an active ID can be reused after its passive order is fully filled.
- SHA-256 regression vectors cover the 55/56/63/64/65-byte padding boundaries and 1,000 bytes.
- A separately generated golden snapshot hash covers signed-price two's-complement encoding.
- Fresh local Debug and Release suites pass 264/264 tests.
- Production-only static and shared-library builds, the pinned formatting gate, deterministic
  fixture rerun, and repository hygiene checks pass. Exhaustive Cppcheck review found no confirmed
  defect.
- The hardening follow-up still requires hosted GCC, Clang, and ASan/UBSan after publication.

## Scope

The public engine still starts at sequence one and has no restoration seam. Internal tests cover
maximum-sequence issuance and sticky exhaustion; directly forcing public exhaustion would require
an unrelated test-only construction path and remains deferred until restored-engine state exists.
