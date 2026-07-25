# 2026-07-25 - Phase 4 integration closure

## Outcome

Phase 3 and all four Phase 4 slices are integrated on `main`.

The reviewed stack was merged sequentially:

| Pull request | Slice | Main commit |
| --- | --- | --- |
| #5 | Phase 3 independent Python oracle and deterministic evidence | `3c60e2b` |
| #7 | Multi-instrument routing, global sequencing, and identity | `3d6d438` |
| #8 | Command logging, inspection, repair, and replay | `f5c2f11` |
| #6 | Persisted snapshots and log-suffix recovery | `25b0a39` |
| #9 | Native Python engine and packaging | `075d29a` |

Each child PR was retargeted to `main` only after its parent merged. Because the original stack and
the squash-merged commits had equivalent trees but different ancestry, each child received a
no-content ancestry merge. Tree comparisons before every push proved those reconciliation commits
did not change source bytes. Each retargeted PR was scoped back to its intended slice, marked ready,
rerun through its complete hosted matrix, and squash-merged only after every required check passed.

The final `main` tree is byte-for-byte equivalent to the reviewed Phase 4 PR #9 tree.

## Final hosted evidence

The uncancelled workflow for merged commit `075d29a` is GitHub Actions run `30175938269`. It
passes:

- GCC and Clang Debug build and CTest;
- GCC and Clang Release build and CTest;
- ASan and UBSan;
- retained command-log and snapshot decoder fuzz smoke;
- Python 3.11, 3.12, 3.13, and 3.14 formatting, lint, typing, and tests;
- pinned C++ formatting;
- Clang compilation of the native Python extension;
- PEP 517 source-distribution build, inspection, clean install, native smoke, and `pip check`; and
- CPython 3.11-3.14 manylinux x86-64 wheel build, audit, compiler-free smoke, retained artifacts,
  and installed CPython 3.12 parity with `ATLAS_DIFF_V2` and `ReferenceRouter`.

The fixed Phase 3 pull-request differential corpus is intentionally skipped on push-to-`main`; it
passed on each final retargeted pull-request head, including PR #9.

## Completion boundary

Phase 4 is complete. The repository now supplies deterministic multi-instrument matching,
engine-wide identity and sequencing, canonical command logging and replay, persisted snapshots and
recovery, and the native-backed Python 0.2.0 package boundary.

This closure does not claim benchmark results, latency, throughput, universal durability,
authentication, security, scalability, PyPI publication, non-Linux wheels, or production
readiness. Those remain outside Phase 4.
