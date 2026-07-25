# 2026-07-25 - Phase 4 native Python implementation

## Outcome

The Phase 4 PR4 native Python contract is frozen in
[ADR 0015](../decisions/0015-native-python-bindings-and-packaging.md), implemented through PR #9,
and integrated on `main`.

Version 0.2.0 adds the lazy public `atlaslob.Engine` facade over a private pybind11 extension,
strict all-before-execution input conversion, owned object/column/summary batches, logged
submission, recovery, snapshot publication, and an explicit GIL/mutex ownership boundary. The
local package gate builds and clean-smokes four CPython-minor-specific manylinux x86-64 wheels for
Python 3.11 through 3.14 and a PEP 517 source distribution.

GCC Debug and Release each pass 482/482 CTest cases. Python reports 354 passes with two expected
Windows canonical-symlink skips, plus 11 campaign/fuzz passes. Ruff, strict mypy, and formatting
pass. The real binding smokes cover batch-mode parity, Unicode persistence, clean and torn
recovery, same-engine noninterleaving, released-GIL thread progress, and returned-value lifetime.
All 15 hosted PR4 checks pass. PRs #7, #8, #6, and #9 were revalidated and squash-merged
sequentially after Phase 3 PR #5; final implementation commit `075d29a` and its uncancelled
push-to-main workflow are green. This closes Phase 4.

## Starting point

PR4 follows three Phase 4 infrastructure slices:

- the router owns the immutable catalog, global command sequence, active-order identity, and
  `ATLSME01` state;
- the command log provides `ATLSLG01`, write-ahead submission, bounded inspection, repair, and
  deterministic replay; and
- persisted recovery provides `ATLSSN01`, staged engine reconstruction, synchronized unique
  snapshot publication, and clean-tail log resumption.

PR3's 13 hosted compiler, sanitizer, decoder-fuzz-smoke, Python, formatting, wheel, and
differential checks are green. PR4 preserves semantic version 6 and every frozen Phase 0 through
Phase 3 evidence encoding. It does not move oracle transition logic into the binding.

## Import and API boundary

The package exposes:

```text
atlaslob.Engine
atlaslob.engine
atlaslob._native_engine
```

`Engine` is resolved lazily from `atlaslob.__init__`. The independent `ReferenceEngine`,
`ReferenceRouter`, canonicalization, generators, shrinking, differential runners, and
`atlaslob.native` subprocess adapter continue to work when the extension is missing or
deliberately blocked. Requesting `Engine` in that state raises a clear `ImportError`; it never
falls back to the reference model.

The wrapper checks a private integer binding ABI before using the extension. This catches a stale
Python wrapper/extension pair independently from the engine semantic version.

The native holder has exactly one of three modes:

```text
live
logged
recovered_read_only
```

A live engine owns the in-memory multi-instrument coordinator. A logged engine owns a
`LoggedEngine`. Clean full-log or snapshot recovery remains writable and attached to that log. A
torn valid-prefix recovery owns standalone reconstructed state and is deliberately read-only.

## Strict preflight

Catalog entries and commands must be the exact supported Python value types. Batch input is
materialized completely before execution. Iteration failure, a wrong late object, or a
representation overflow therefore executes zero commands from that call.

The adapter rejects `bool` for every integer field and performs explicit signed/unsigned range
checks. Client and instrument IDs use 32-bit unsigned bounds; order IDs and quantities use 64-bit
unsigned bounds; prices use signed 64-bit bounds; and enum codes use 8-bit unsigned bounds. Raw
unknown enum values inside that representation reach C++ domain validation and produce ordinary
sequenced rejections.

Paths accept `str` and string-valued `os.PathLike` objects, reject byte strings, and preserve
Unicode. Output, durability, replay, and tail-policy options are closed literals. Invalid
configuration remains distinct from a malformed command.

## Batch behavior and ownership

`submit()` uses the same native path as a one-command object batch. `submit_batch()` accepts three
output modes:

- `objects` returns ordered immutable results and event tuples;
- `columns` returns caller-owned standard-library arrays, command/event offsets, common event
  columns, and explicit presence arrays for every variant field; and
- `summary` returns counts, terminal state, and the final digest without materializing individual
  result or event objects.

All modes report the same submitted, processed, committed, rejected, terminal-error, and final
digest values. Domain and state rejections count as processed and do not stop a batch. A terminal
engine error counts as processed and stops before later commands. Batches are prefix-committing,
not transactional.

A persistence append, flush, or sync failure is not an engine error. It carries the successfully
published prefix, excludes the failed command from `processed_count`, and leaves the logged
session poisoned. Later commands require recovery from the authoritative log.

Events, arrays, snapshots, reports, and errors own their storage. They survive later mutations and
destruction of their originating engine. Column arrays are not views, and changing one returned
array cannot modify the engine or another result.

## Recovery and snapshot behavior

`Engine.create_logged()` selects buffered, flush-each-record, or sync-each-record durability with
sync as the default. `Engine.recover()` supports full-log, explicit-snapshot, or snapshot-directory
recovery.

Strict recovery rejects a torn final record. Valid-prefix recovery may reconstruct that complete
prefix, but the returned engine is not logged and is read-only. Submission, batch submission, and
snapshot publication fail before mutation and direct the caller to create a distinct clean log
through `atlas_inspect repair-tail`, then recover that repaired copy strictly. A complete corrupt
record, interior corruption, incompatible configuration, or replay divergence remains a recovery
error under either tail policy.

Snapshot publication is available only from a writable logged engine and retains the PR3
log-synchronization, reread verification, unique-name, and no-overwrite rules. The Python layer
does not weaken the persistence poison or report contracts.

## GIL and mutex boundary

One call follows this order:

```text
convert with GIL
release GIL
lock one engine
execute complete batch into C++-owned values
unlock engine
reacquire GIL
materialize Python results
```

The binding does not wait for an engine mutex while holding the GIL. One mutex covers an entire
batch, so concurrent calls on the same engine receive noninterleaved command-sequence ranges.
Distinct engines own distinct mutexes. Python-free native work releases the GIL, and tests prove
that another Python thread makes progress while it runs.

No Python object is retained by C++ execution. The extension exports no borrowed pointer, span,
memory view, or container view into matching-engine state.

## Packaging boundary

The distribution uses:

- `scikit-build-core==1.0.3`;
- `pybind11==3.0.4`; and
- `cibuildwheel==4.1.0`.

Ordinary CMake configuration leaves `ATLAS_BUILD_PYTHON=OFF`. A package build enables PIC, hidden
visibility, and the private extension target while disabling tests and shared project libraries.
Only the extension and Python package are installed.

The wheel matrix is:

```text
cp311-manylinux_x86_64
cp312-manylinux_x86_64
cp313-manylinux_x86_64
cp314-manylinux_x86_64
```

The wheels are CPython-minor-specific rather than `abi3`. Content verification requires the
extension, wrapper, type stub, `py.typed`, license, and third-party notices and rejects headers,
static libraries, executables, tests, corpora, and build paths. auditwheel validation permits only
the declared manylinux system-library boundary and rejects bundled or unexpected libraries. A
Linux version script and ELF inspection limit defined dynamic exports to
`PyInit__native_engine`.

The source distribution contains the CMake and C++ sources required to build the extension but
excludes repository, CI, test, corpus, cache, and build artifacts. It builds through ordinary
isolated PEP 517 handling, installs, runs the same package smoke, and passes `pip check`.

CI retains wheel and source artifacts. Phase 4 does not publish to PyPI.

## Implemented surfaces

- [x] Lazy public `Engine` and private binding-ABI check.
- [x] Extension-free oracle import isolation and fail-closed native loading.
- [x] Exact catalog and command conversion with all-batch preflight.
- [x] Single submission plus object, column, and summary batches.
- [x] Owned event, array, snapshot, report, and error values.
- [x] Live, logged, and recovered-read-only backends.
- [x] Logged creation, clean recovery, torn-prefix read-only recovery, and snapshots.
- [x] Structured engine, persistence, recovery, snapshot, and read-only error boundaries.
- [x] Per-engine batch serialization and released-GIL native execution.
- [x] scikit-build-core migration, private extension CMake target, stub, and typing marker.
- [x] CPython 3.11-3.14 manylinux x86-64 wheel matrix and source distribution.
- [x] Packaging content, dynamic dependency, exact export, clean-install, and import-isolation
  smokes.
- [x] Documentation, evidence index, roadmap, changelog, and semantic-contract updates.

## Validation status

- [x] GCC Debug: 482/482 CTest cases.
- [x] GCC Release: 482/482 CTest cases.
- [x] Python default selection: 354 passes and two expected Windows canonical-symlink skips.
- [x] Python campaign/fuzz selection: 11 passes.
- [x] Ruff formatting and lint.
- [x] Strict mypy.
- [x] C++ formatting.
- [x] Object/column/summary parity and exact single/batch behavior.
- [x] Strict type, `bool`, integer-boundary, raw-enum, and late-malformed preflight cases.
- [x] Unicode logged paths and snapshot publication.
- [x] Clean writable, strict torn, valid-prefix read-only, and corrupt-log recovery cases.
- [x] Same-engine noninterleaving and released-GIL Python-thread progress.
- [x] Returned-value lifetime and mutation isolation.
- [x] Four manylinux x86-64 wheels: contents, auditwheel, exact extension exports, and
  clean-container smoke.
- [x] PEP 517 source distribution: contents, clean build/install, smoke, and `pip check`.
- [x] Hosted PR4 validation.
- [x] PR4 publication as draft PR #9.
- [x] Sequential stacked integration through PRs #7, #8, #6, and #9.
- [x] Final uncancelled `main` workflow.

## Claim boundary

This record claims the implemented and merged Phase 4 surfaces plus the local and hosted
validation listed above. It does not claim PyPI publication, Windows or macOS wheels, `abi3`,
PyPy, free-threaded CPython, benchmark results, latency or throughput, universal durability,
authentication, security, scalability, or production readiness.
