# ADR 0015: Native Python bindings, batch ownership, and packaging

- Status: accepted
- Date: 2026-07-25
- Implementation state: implemented and locally validated on Phase 4 draft PR #9; all 15 hosted
  PR4 checks pass and stacked integration remains pending.

## Context

The Phase 3 Python package is an independent correctness oracle and subprocess differential
adapter. It intentionally does not load the C++ engine into the Python process. Phase 4 requires a
separate production-shaped boundary that exposes the deterministic multi-instrument coordinator,
write-ahead logging, recovery, and snapshots through a native CPython extension without coupling
the oracle to native implementation details.

The binding must preserve these properties:

- Python conversion failures occur before domain submission and consume no sequence.
- In-range semantic failures reach C++ and return ordinary sequenced rejection events.
- A complete finite batch is converted before its first command executes.
- Returned events, snapshots, columns, reports, and errors own their storage.
- One engine instance is serialized while distinct engine instances may execute concurrently.
- Python-free C++ work may run without the GIL, but Python objects are never accessed without it.
- A torn log is never reopened for append and corruption is never treated as a torn tail.
- The independent oracle remains usable when the extension is missing or deliberately blocked.

The C++ persistence API has an important boundary. `LoggedEngine::recover*` returns a writable
append session only for a clean, identity-validated log extent. Standalone replay can recover a
valid prefix from a torn tail, but that state cannot safely append to the original file. The Python
surface needs useful `valid-prefix` semantics without silently discarding durability.

`LoggedSubmissionResult` also distinguishes persistence failure from `EngineError`. A failed log
append may poison the persistence session and publishes no in-memory engine result for that
command. Mapping it to `EngineError::internal_failure` would misrepresent both sequencing and the
required recovery action.

## Decision

### Package and import boundary

The distribution version is `0.2.0`.

- The private extension is `atlaslob._native_engine`.
- The public wrapper is `atlaslob.engine`.
- The public class is `atlaslob.Engine`, loaded lazily from `atlaslob.__init__`.
- `atlaslob.native` remains the Phase 3 subprocess adapter.
- `ReferenceEngine`, `ReferenceRouter`, canonicalization, generation, shrinking, and differential
  modules never import the extension.
- Missing or ABI-incompatible native code raises a clear `ImportError`. There is no fallback to the
  reference model.
- The extension exports a private binding ABI integer. The wrapper checks it during import so stale
  Python and extension artifacts cannot run together silently.

`BookTop` moves to `atlaslob.domain` and remains re-exported from `atlaslob.reference` for
compatibility. Native and oracle observers therefore share one value class without importing each
other.

### Native holder and backend modes

The private extension owns one of three backend modes:

```text
live
logged
recovered_read_only
```

The holder contains one per-engine mutex and exactly one populated engine owner:

- `live`: a writable in-memory `MultiInstrumentEngine`;
- `logged`: a writable `LoggedEngine`, whose observer state is `logged.engine()`; or
- `recovered_read_only`: an in-memory `MultiInstrumentEngine` reconstructed from a torn log's valid
  prefix.

Public observers expose:

```text
engine.logged
engine.read_only
engine.recovery_report
```

The recovery report is an immutable owned Python value or `None`.

### Recovery behavior

`Engine.recover()` accepts at most one of `snapshot_path` and `snapshot_dir`.

- Clean full-log recovery uses `LoggedEngine::recover` and returns a writable logged engine.
- Clean explicit-snapshot recovery uses `LoggedEngine::recover_from_snapshot`.
- Clean directory recovery uses `LoggedEngine::recover_from_snapshot_directory`.
- `tail_policy="strict"` rejects a torn final record with `RecoveryError`.
- `tail_policy="valid-prefix"` plus a torn final record uses standalone replay or snapshot recovery
  and returns `logged=False`, `read_only=True`.
- `submit`, `submit_batch`, and `write_snapshot` on a read-only recovered engine raise
  `ReadOnlyRecoveryError` before locking, sequencing, filesystem writes, or engine mutation. The
  message directs callers to `atlas_inspect repair-tail <input> <new-output>` and then strict
  recovery of the new file.
- A checksum-complete corrupt record, interior corruption, configuration mismatch, or replay
  divergence raises `RecoveryError` under either tail policy.

This keeps `valid-prefix` useful for inspection and controlled read-only recovery while preserving
the C++ rule that only a clean authoritative extent becomes writable.

### Persistence failures

Logged open, append, flush, sync, snapshot, and recovery I/O failures are not converted to
`EngineError`.

- Operational failures raise `PersistenceError`.
- Structural recovery failures raise `RecoveryError`.
- Snapshot codec/publication failures raise `SnapshotError`.
- Read-only mutation attempts raise `ReadOnlyRecoveryError`.
- Exceptions retain owned structured details: category, byte offset, system error information,
  poison state, and recovery/publication report where applicable.
- A poisoned logged session accepts no later command. The exception instructs the caller to
  recover from the authoritative log.

Implicit Python object destruction is a deliberately narrower boundary. Pybind11 releases the GIL
before invoking the native destructor, so closing a logged file cannot block unrelated Python
threads. Destruction is nevertheless `noexcept` and best-effort: an operating-system close failure
cannot be raised safely from a destructor and is not part of the mapped exception contract.
Callers that require a surfaced durability result must use the normal synchronized submission
mode or successful snapshot publication before relinquishing the engine.

The private module's four native exception types retain one owning process-lifetime reference.
Native error paths therefore remain safe even if a caller deletes a private module attribute.
Subinterpreter-specific module state is deferred with free-threaded CPython support.

For a batch, a persistence failure carries the successfully published prefix `BatchResult`. The
failed command is excluded from `processed_count` because no `EngineResult` was published and no
authoritative command sequence became visible in memory.

### Strict Python input conversion

All Python input conversion occurs while holding the GIL and before acquiring the engine mutex.

- Catalog entries must be exact `InstrumentConfig` instances.
- Commands must be exact `NewOrder`, `CancelOrder`, or `ReplaceOrder` instances. Mappings,
  subclasses used as duck types, and arbitrary attribute objects are rejected.
- The complete finite command iterable is materialized and converted before execution. If
  iteration raises or a late command is malformed, zero commands execute.
- `bool` is rejected explicitly for every integer field.
- Normal `int` and `IntEnum` values are accepted.
- Enum fields accept raw in-range `0..255` values. Unknown values reach C++ and produce sequenced
  domain rejections.
- Client and instrument IDs use unsigned 32-bit range checks.
- Order IDs and quantities use unsigned 64-bit range checks.
- Prices use signed 64-bit range checks.
- Enum codes use unsigned 8-bit range checks.
- Representation overflow raises `OverflowError`.
- Wrong object type, shape, or literal option raises `TypeError`.
- Invalid catalog or engine configuration raises `ValueError`.
- `std::bad_alloc` becomes `MemoryError`.
- Paths accept `str` and `os.PathLike[str]`, reject `bytes`, and preserve Unicode.
- Output mode, durability, replay mode, and tail policy are closed string literals.

The extension does not use pybind11's automatic integer caster for command/configuration fields
because that caster accepts `bool` and obscures the required `TypeError` versus `OverflowError`
boundary.

### Results and object batches

Python exposes immutable value classes. An `EngineResult` contains exactly one of:

- an immutable tuple of events; or
- an `EngineError`.

Committed/rejected state, command sequence, and instrument ID are derived properties. Every C++
event alternative maps to the existing frozen domain event class.

`submit(command)` performs the same conversion and native path as a one-command
`submit_batch(..., output="objects")` and returns its sole `EngineResult`.

`BatchResult` contains:

```text
submitted_count
processed_count
committed_count
rejected_count
terminal_error
final_state_digest
payload
```

Exactly one mode-specific payload is populated:

- `ObjectBatch(results=tuple[EngineResult, ...])`;
- `ColumnBatch(...)`; or
- an explicit empty `SummaryBatch` marker.

Domain/state rejections increment `processed_count` and `rejected_count` and do not stop the batch.
An `EngineError` command increments `processed_count`, appears in object/column output, becomes
`terminal_error`, and stops the batch before later commands. Batches are prefix-committing, not
transactional. Empty batches are valid and return the current digest.

### Column batches

Column mode returns caller-owned standard-library `array.array` values. The wrapper verifies the
CPython item sizes for:

```text
B = unsigned 8-bit
H = unsigned 16-bit
I = unsigned 32-bit
Q = unsigned 64-bit
q = signed 64-bit
```

It contains:

- command-to-event offsets of length `processed_count + 1`;
- one command outcome per processed command;
- engine-error values and explicit presence;
- common event columns for command sequence, event index, event type, and instrument ID; and
- event-length variant columns with explicit presence arrays for every accepted, rejected, trade,
  rested, canceled, replaced, done, and book-changed field.

Absent numeric values are canonical zero. Presence arrays distinguish absence from a meaningful
zero or from a missing best bid/ask. Mutating returned arrays never changes engine state or another
result.

Summary mode counts outcomes and computes the final digest without materializing individual event
objects or event columns.

### GIL, locking, and ownership

The binding uses this lock order:

1. hold the GIL while converting all Python arguments into owned C++ values;
2. release the GIL;
3. acquire the per-engine mutex;
4. execute the entire batch and move/copy results into Python-free owned C++ DTOs;
5. release the mutex;
6. reacquire the GIL; and
7. materialize Python values.

The binding never waits for the engine mutex while holding the GIL. A batch holds one engine mutex
for its complete execution so commands from different calls cannot interleave. Separate engines
have separate mutexes and may execute concurrently.

No Python object is stored in or accessed by Python-free execution code. The extension exposes no
reference return policy, pointer, span, memory view, or container view into engine-owned memory.
Results remain valid after later engine mutation and engine destruction.

Python result materialization may itself fail after a native prefix committed. Such a Python
allocation failure does not make the batch transactional; the engine's authoritative state remains
the source of truth.

### Native build and installation

CMake adds:

```text
ATLAS_BUILD_PYTHON=OFF
```

When enabled:

- `PYBIND11_FINDPYTHON` is enabled;
- CPython 3.11 or newer and `Development.Module` are required;
- pybind11 is required from the build environment;
- all linked static project libraries are position-independent and compiled with hidden
  visibility;
- the Linux linker version script exports only `PyInit__native_engine`;
- only the extension is installed under the `python` component; and
- C++ headers, archives, tools, tests, corpora, and build paths are not installed.

The Python build forces `BUILD_SHARED_LIBS=OFF`, `BUILD_TESTING=OFF`, and
`ATLAS_BUILD_PYTHON=ON`.

### Packaging and wheel policy

The build backend is `scikit-build-core==1.0.3` with `pybind11==3.0.4`.
`cibuildwheel==4.1.0` builds official artifacts.

- Official wheels target Linux x86-64 CPython 3.11, 3.12, 3.13, and 3.14.
- Wheels are CPython-minor-specific. Phase 4 does not set `abi3`, `Py_LIMITED_API`, or
  `wheel.py-api`.
- CI retains wheels and the source distribution but does not publish to PyPI.
- Wheel contents include the extension, Python package, `.pyi`, `py.typed`, license, and
  third-party notices.
- Wheel contents exclude C++ headers, static libraries, executables, tests, corpora, source-tree
  paths, and build-tree paths.
- `auditwheel` must show no unexpected shared-library dependency.
- ELF inspection must find exactly one defined dynamic export: `PyInit__native_engine`.
- Clean-environment wheel smoke tests run without a compiler or checkout.
- The source distribution must build and install in a clean environment.

The package retains no runtime dependency on the oracle or subprocess adapter beyond its own
Python modules.

## Consequences

- The native API cannot silently execute against the slower reference implementation.
- Strict conversion makes late malformed batches atomic with respect to execution.
- Raw invalid enum values remain replayable domain inputs instead of becoming Python parse errors.
- Clean recovered engines remain durably log-attached; torn valid-prefix state is intentionally
  read-only until repaired to a new file.
- Persistence failure remains distinguishable from engine exhaustion/internal failure.
- Holding one mutex for a batch gives deterministic non-interleaving per engine while still
  allowing independent engines to run concurrently.
- CPython-minor-specific wheels increase the artifact matrix but avoid prematurely freezing a
  limited ABI contract.
- Windows and macOS wheel publication, `abi3`, PyPy, free-threaded CPython, and PyPI publication
  remain deferred.

## Acceptance evidence

The local PR4 gate now proves:

- oracle modules import and execute with `_native_engine` blocked;
- missing or stale native code fails clearly without fallback;
- every numeric field rejects `bool` and every representation boundary is checked;
- a malformed late batch element executes zero commands;
- raw invalid enum values consume a sequence and return a rejection;
- object, column, and summary modes have identical counts and final digest;
- terminal `EngineError` and persistence-failure prefix accounting follow this ADR;
- same-engine batches have noninterleaved sequence ranges;
- another Python thread progresses during released-GIL native work;
- returned values survive mutation and engine destruction;
- native results agree with the independent `ReferenceRouter` and V2 subprocess adapter;
- clean recovery is writable and logged;
- strict torn recovery fails, valid-prefix torn recovery is read-only, and corruption never becomes
  a torn-tail warning;
- repaired-copy strict recovery is writable;
- Unicode persistence paths work;
- CPython 3.11–3.14 manylinux wheels and the source distribution pass clean-environment smoke
  tests; and
- wheel contents, dynamic dependencies, and the one-symbol extension export surface match the
  packaging policy above.

The real-extension selections include object/column/summary parity, Unicode logged paths, clean
writable recovery, strict torn-tail refusal, valid-prefix read-only recovery, same-engine
noninterleaving, progress in another Python thread while native work runs without the GIL, and
owned-result lifetime checks.

GCC Debug and Release each pass 482/482 CTest cases. Python reports 354 passes with two expected
Windows canonical-symlink skips, plus 11 campaign/fuzz passes. Ruff, strict mypy, and formatting
pass. cibuildwheel 4.1.0 produces four CPython-minor-specific manylinux x86-64 wheels for 3.11
through 3.14; each passes content validation, auditwheel policy validation, and clean-container
smoke. The PEP 517 source distribution builds, installs, and smokes in a clean environment.

The local results are independently revalidated by all 15 hosted PR4 checks on implementation
commit `0525c9b`. Stacked integration remains required before Phase 4 completion. No benchmark,
universal durability, security, or production-readiness claim follows from this evidence.
