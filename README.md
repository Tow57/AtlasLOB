# AtlasLOB

[![CI](https://github.com/Tow57/AtlasLOB/actions/workflows/ci.yml/badge.svg)](https://github.com/Tow57/AtlasLOB/actions/workflows/ci.yml)

AtlasLOB is a deterministic, in-memory limit order book and matching engine built primarily in
C++20, with Python reserved for independent validation, workload generation, bindings, and
benchmark analysis.

AtlasLOB is an educational portfolio project. It is not affiliated with Hudson River Trading and
does not connect to a real exchange.

The project is being developed as a sequence of evidence-backed releases. Correctness,
reproducibility, and clear engineering tradeoffs take priority over feature count or unsupported
latency claims.

## Current status

**Version 0.2.1 combines the deterministic C++20 multi-instrument matching engine, canonical
command-log replay and recovery, native Python bindings, and reproducible performance evidence.
The official Phase 5 campaign is finalized and independently verified: 510 valid observations,
no invalid observations, across 51 measurement shapes at the frozen release commit.**

See the [verified Phase 5 results](docs/benchmarks/phase5-results.md) and the
[Phase 4 evidence index](docs/evidence/phase4/README.md) for the measured and correctness boundaries.

Phase 5's measurement boundaries and claim policy are in the
[performance methodology](docs/performance-methodology.md). The
[benchmark evidence format](docs/benchmark-evidence-format.md) specifies every canonical field
and hash binding, while the
[benchmark reproduction guide](docs/benchmark-reproduction.md) walks from an opt-in build to a
verified exploratory bundle. The
[native-host runbook](docs/phase5-native-host-runbook.md) freezes the later official campaign.

| Capability | Status | Evidence |
| --- | --- | --- |
| Target-based C++20 build | Complete | `CMakeLists.txt`, named presets |
| Strong order and instrument values | Complete | Compile-time separation in unit tests |
| New/cancel/replace vocabulary | Complete | `atlas_domain_tests` |
| Deterministic pure validation | Complete | GoogleTest domain cases |
| Normalized event schema | Complete | Event payload and discriminator tests |
| Command sequencing and ID policy | Complete | ADR 0002 and semantic contract |
| Canonical domain fixture | Complete | `atlas_cli domain-fixture` integration tests |
| Stable owning order-node storage | Complete | `atlas_core_tests`, ADR 0003 |
| Checked intrusive FIFO price levels | Complete | Core mutation, invariant, and stress tests |
| Ordered bid/ask sides and best-price access | Complete | `core.BookSide*` tests, ADR 0004 |
| Active-order index and direct cancellation | Complete | `core.ActiveOrderIndex*`, `core.InstrumentBook*`, ADR 0005 |
| Sequenced command admission and state validation | Complete | `core.CommandAdmission*`, ADR 0006 |
| Read-only match planning and final-capacity projection | Complete | `core.MatchPlan*`, ADR 0006 |
| Owned normalized event batches and prepared residuals | Complete | `core.EventBatchBuilder*`, `core.InstrumentBookPreparedRest*` |
| Atomic limit/market New execution | Complete | `core.CommandExecutor*`, ADR 0007 |
| Sequenced Cancel execution and normalized events | Complete | `core.CommandExecutorCancel*`, ADR 0007 |
| Atomic Replace with priority reset | Complete | `core.CommandExecutorReplace*`, ADR 0008 |
| Public single-instrument matching facade | Complete | `atlaslob::MatchingEngine`, ADR 0008 |
| Canonical snapshots and state/event digests | Complete | `core.Canonical*`, ADR 0009 |
| Executable matching fixture | Complete | `atlas_cli engine-fixture`, golden integration fixtures |
| Independent command-stream comparison | Complete | 10,000 mixed commands plus deterministic rerun |
| Versioned native differential adapter | Complete | `atlas_diff_native`, strict JSONL integration tests |
| Independent Python domain and digest model | Complete | Python golden-vector and strict typing tests |
| Independent Python matching oracle | Complete | `dict`/`deque` model, named transition tests |
| Named cross-language parity | Complete | Exact per-command events, snapshots, observers, and digests |
| Internal oracle package | Complete | Python 3.11-3.14 matrix and normal typed-wheel smoke gate |
| Versioned deterministic workload generation | Complete | Generator V1, ten profiles, checked manifests |
| Streaming differential campaigns | Complete | Exact/compact runner, fixed/rotating/published tiers |
| Failure persistence and semantic shrinking | Complete | Fresh exact replay, bounded deferral, three injected faults |
| Metamorphic and bounded fuzz evidence | Complete | Five properties, two fuzz paths, checked seed corpus |
| Phase 3 long campaign and final release gates | Complete | Checked million-command case and published PR-head gates |
| GCC and Clang CI | Passed on published Phase 3 PR head | `.github/workflows/ci.yml` |
| ASan and UBSan CI | Passed on published Phase 3 PR head | `asan-ubsan` preset and CI job |
| Pinned clang-format gate | Passed on published Phase 3 PR head | `format-check` CI job |
| Multi-instrument facade and immutable catalog | Complete on `main` | `atlaslob::MultiInstrumentEngine`, ADR 0012 |
| Global sequence, active-ID directory, and capacity | Complete on `main` | Router unit and stress coverage |
| Multi-engine snapshots and `ATLSME01` digest | Complete on `main` | Independent C++/Python golden evidence |
| Native multi-instrument adapter and strict decoder | Complete on `main` | `atlas_diff_multi_native`, `ATLAS_DIFF_V2` |
| Python `ReferenceRouter` and Generator V2 | Complete on `main` | V2 workload, manifest, and interleaving tests |
| Append-only command log, scanner, repair, and replay | Complete on `main` | [ADR 0013](docs/decisions/0013-command-log-and-replay.md), [format](docs/command-log-format.md), 85 persistence tests |
| Persisted snapshot codec, publication, recovery, and clean-tail log resumption | Complete on `main` | [ADR 0014](docs/decisions/0014-persisted-snapshots-and-log-suffix-recovery.md), [format](docs/snapshot-format.md), PR #6 |
| Native pybind11 engine, strict batches, logging, recovery, and snapshots | Complete on `main` | [ADR 0015](docs/decisions/0015-native-python-bindings-and-packaging.md), Python binding and packaging tests |
| CPython 3.11-3.14 manylinux wheels and source distribution | Complete on `main` | `cibuildwheel==4.1.0`, clean wheel/auditwheel and PEP 517 sdist smoke |
| Resting book structure | Complete | `stress.InstrumentBookStress*` |
| Matching and normalized command execution | Complete | Phase 2 |
| Reproducible benchmark and evidence infrastructure | Complete | [ADR 0016](docs/decisions/0016-reproducible-performance-evidence.md), deterministic smoke fixtures and bundle verifier |
| Resumable baseline and `perf` campaign | Complete | [Verified Phase 5 results](docs/benchmarks/phase5-results.md) |
| Native-host baseline and finalized reports | 510/510 valid observations | [Phase 5 results](docs/benchmarks/phase5-results.md) |
| Versioned Linux gateway | Deferred | Phase 6 |

## Technical overview

AtlasLOB implements price-time-priority matching for limit and market orders across an immutable
instrument catalog. Integer domain types, deterministic command sequencing, canonical state and
event digests, and an independent Python reference model make semantic differences reproducible
rather than anecdotal. Versioned ATLAS workloads and ATLSLG command logs support exact replay;
allocation tracking exposes core and setup memory behavior. Native Python bindings provide owned
object, typed-column, and summary batch interfaces.

```mermaid
flowchart LR
  C[Commands] --> R[Multi-instrument router]
  R --> B[Price-time order books]
  B --> E[Normalized events]
  B --> S[Canonical snapshots and digests]
  C --> L[ATLSLG command log]
  L --> P[Replay and recovery]
  C --> O[Independent Python oracle]
  E --> V[Deterministic parity evidence]
  O --> V
  S --> V
```

Tests cover domain validation, matching and cancellation invariants, atomic replacement,
multi-instrument identity, byte-exact logs and snapshots, replay/recovery, cross-language parity,
native bindings, allocation failure boundaries, fuzz decoding, and packaging.

## Verified benchmark highlights

These rates are medians of 10 valid observations for each shape on
`ryzen-9800x3d-64g-ubuntu2404-a` (AMD Ryzen 7 9800X3D, Ubuntu 24.04.4 LTS, kernel
7.0.0-28-generic, local NVMe ext4). Official execution was pinned to logical CPU 1 with its SMT
sibling idle. The full campaign contains 510 valid observations across 51 shapes and was finalized
and independently bundle-verified.

| Result | Finalized value |
| --- | ---: |
| W04 C++ matching, 65,536 active orders | 2,583,618.74 commands/s; 9,430,208.40 events/s |
| W04 command latency, 312,500 samples | 310 ns p50; 511 ns p99; 802 ns p99.9 |
| Verified 10,000,000-record replay | 334,744.21 records/s; 1,171,604.75 events/s |
| Python summary output, batch 1,024 | 932,256.78 commands/s; 3,402,737.26 events/s |
| One-million-order preload | 1,456,462.28 commands/s; 2,958,441.92 events/s |

See [Phase 5 benchmark results](docs/benchmarks/phase5-results.md) for methodology, units,
allocation results, the complete Python batch table, report-field citations, and the profiling
limitation. Release measurement paths avoid pathological invariant and digest work while retained
inputs still undergo independent semantic validation and deterministic output checks.

## Quick start

Requirements:

- CMake 3.25 or newer
- Ninja
- Git, used by CMake to fetch the pinned test-only GoogleTest dependency
- A C++20 compiler: GCC 13+ or Clang 17+
- Python 3.11 through 3.14 for the independent correctness-evidence package and native
  distribution

The first testing-enabled configure downloads GoogleTest 1.17.0 at an immutable commit. Production
library builds configured with `BUILD_TESTING=OFF` do not fetch or link GoogleTest.

The ordinary C++ build keeps native Python disabled. Package builds enable the private pybind11
extension through scikit-build-core; importing the independent oracle does not require that
extension, while accessing `atlaslob.Engine` fails clearly if it is unavailable.

Configure, build, and test with GCC:

```sh
cmake --preset dev-gcc
cmake --build --preset dev-gcc
ctest --preset dev-gcc
```

Run the deterministic validation demonstration:

```sh
./build/dev-gcc/atlas_cli validate-demo
```

On Windows, run `build/dev-gcc/atlas_cli.exe validate-demo` instead.

Validate a canonical development fixture:

```sh
./build/dev-gcc/atlas_cli domain-fixture examples/domain-valid.commands
```

Execute a deterministic matching fixture with per-command event and state hashes:

```sh
./build/dev-gcc/atlas_cli engine-fixture 7 examples/engine-demo.commands
```

On Windows, use `build/dev-gcc/atlas_cli.exe` for either fixture command.

Build and run the independent Python evidence suite on Linux or macOS:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -e ".[dev]"
cmake --preset dev-gcc
cmake --build --preset dev-gcc --target atlas_diff_native
.venv/bin/python -m ruff format --check python benchmarks/python tests/fuzz/run_command_log_fuzz_smoke.py tests/packaging
.venv/bin/python -m ruff check python benchmarks/python tests/fuzz/run_command_log_fuzz_smoke.py tests/packaging
.venv/bin/python -m mypy
.venv/bin/python -m pytest
```

Use the virtual environment directly from Windows PowerShell:

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -e ".[dev]"
cmake --preset dev-gcc
cmake --build --preset dev-gcc --target atlas_diff_native
.\.venv\Scripts\python.exe -m ruff format --check python benchmarks/python tests/fuzz/run_command_log_fuzz_smoke.py tests/packaging
.\.venv\Scripts\python.exe -m ruff check python benchmarks/python tests/fuzz/run_command_log_fuzz_smoke.py tests/packaging
.\.venv\Scripts\python.exe -m mypy
.\.venv\Scripts\python.exe -m pytest
```

The parity tests discover the normal `build/dev-gcc` adapter path. A complete evidence run fails
when no adapter exists. To select a different build, set `ATLAS_DIFF_NATIVE` to that executable's
existing absolute path. An explicit missing path is also an evidence failure; it never falls back
to another build or skips parity.

```sh
ATLAS_DIFF_NATIVE=/absolute/path/to/atlas_diff_native .venv/bin/python -m pytest
```

```powershell
$env:ATLAS_DIFF_NATIVE = "C:\absolute\path\to\atlas_diff_native.exe"
.\.venv\Scripts\python.exe -m pytest
```

Run the bounded default and marked Phase 3 selections separately:

```sh
python -m pytest -m "not campaign and not differential_fuzz"
python -m pytest -m "campaign or differential_fuzz"
```

Run the fixed pull-request policy through the command-line runner:

```sh
python -m atlaslob.cli predefined \
  --tier pr \
  --native build/dev-gcc/atlas_diff_native \
  --output build/phase3-pr
```

The PR policy is ten fixed 5,000-command exact cases, one for each required workload profile.
Main and nightly tiers require an explicit `--epoch`; published release tiers use a checked literal
seed set. Manual Release work is sharded by compiler and case after a full GCC/Clang Release
build-and-CTest prerequisite; the sanitizer subset is sharded per case. Capacity-bound Release and
sanitizer shards defer an exact replay longer than 1,000,000 commands while retaining a portable
semantic-failure handoff for manual reproduction on a suitable host. See the
[differential-testing interface](docs/differential-testing.md) and
[Phase 3 evidence index](docs/evidence/phase3/README.md) before running a long campaign.

## Supported environments

Ubuntu 24.04 is the primary supported environment because later gateway and profiling work will
use `epoll` and Linux `perf`. CI tests GCC and Clang on Ubuntu. The portable foundation is also
developed with MinGW GCC on Windows, but Linux CI is the support authority.

## Design boundaries

- Prices are signed integer ticks; floating-point prices do not enter the core.
- Client validation returns explicit values rather than throwing exceptions.
- The matching core will remain single-writer and independent of sockets, filesystems, Python,
  logging frameworks, and benchmark frameworks.
- Market orders are IOC and never rest. Market GTC is rejected before mutation.
- FOK exists in the versioned vocabulary but is explicitly unsupported until a verified
  non-mutating liquidity preflight exists.
- Baseline order storage owns nodes through `unique_ptr`; price levels hold non-owning intrusive
  links and never control node lifetime.
- ADR 0005 retains storage as the sole owner while the active index and FIFO links hold checked
  non-owning pointers. Direct indexed cancellation follows one reviewed invalidation order:
  unlink, index removal, empty-level removal, and storage destruction.
- ADR 0006 assigns a sequence before domain admission, plans matches without mutation, owns each
  command's complete event batch, and allocates a resting residual before any planned fill is
  applied.
- ADR 0007 rebinds plans in exact price-time order, prebuilds normalized events, and commits New or
  Cancel through one all-preflight mutation boundary.
- ADR 0008 treats Replace as one atomic old-removal/passive-fill/residual transaction and keeps
  mutable implementation details behind the public `MatchingEngine` PImpl.
- ADR 0009 freezes exact best-price/FIFO snapshots and versioned big-endian state/event digest
  encodings, then verifies complete command streams against a separate map/deque reference model.
- ADR 0010 keeps the Python oracle in a separate process with no bindings or private C++ access
  and defines fatal adapter/resource boundaries for cross-language evidence.
- ADR 0011 freezes generator V1, ten workload profiles, campaign sizes and seed provenance,
  bounded-memory comparison, fresh exact replay with an explicit large-prefix deferral boundary,
  portable failure bundles, semantic shrinking, and the Phase 3 metamorphic/fuzz boundary.
- ADR 0012 places one deterministic router above eagerly constructed instrument books, makes
  sequence and active-order identity engine-wide, defines projected local/global capacity and the
  internal prepare/commit boundary, and freezes the separate `ATLSME01`/V2 evidence family without
  changing semantic version 6 or the Phase 2/3 encodings.
- ADR 0013 freezes the byte-exact `ATLSLG01` V1 header and records, CRC32C coverage, write-ahead
  durability and poisoning boundary, bounded scanning, safe tail repair, replay verification,
  deterministic report schemas, and sticky poisoning after an impossible post-WAL commit mismatch.
- ADR 0014 freezes the byte-exact `ATLSSN01` V1 hierarchy, bounded validation, staged bulk
  intrusive-state restoration, synchronized unique publication, symlink-safe newest-valid
  discovery, exact log-boundary/suffix recovery, clean-tail `LoggedEngine` resumption, and
  separate snapshot-aware report schemas.
- ADR 0015 freezes the native Python import and ABI boundary, exact preflight conversion,
  prefix-committing object/column/summary batches, persistence and read-only recovery errors,
  per-engine mutex and GIL order, owned return values, and the CPython-specific manylinux
  distribution policy.
- ADR 0016 freezes opt-in measurement boundaries, qualified-environment evidence, versioned
  benchmark schemas, process-level sampling and comparison rules, optimization acceptance gates,
  and the exact-tag portfolio release workflow. Shared CI measurements remain smoke evidence and
  cannot support latency or hardware-counter claims.

See [the semantic contract](docs/semantics.md) and
[ADR 0001](docs/decisions/0001-core-semantics.md) plus
[ADR 0002](docs/decisions/0002-command-sequencing-and-identity.md) plus
[ADR 0003](docs/decisions/0003-stable-order-storage-and-price-levels.md) plus
[ADR 0004](docs/decisions/0004-ordered-book-sides.md) plus
[ADR 0005](docs/decisions/0005-indexed-order-book-and-cancellation.md) plus
[ADR 0006](docs/decisions/0006-command-admission-and-execution-preparation.md) plus
[ADR 0007](docs/decisions/0007-atomic-new-and-cancel-execution.md) plus
[ADR 0008](docs/decisions/0008-atomic-replace-and-public-engine.md) plus
[ADR 0009](docs/decisions/0009-canonical-deterministic-evidence.md) plus
[ADR 0010](docs/decisions/0010-independent-python-oracle-boundary.md) plus
[ADR 0011](docs/decisions/0011-deterministic-differential-campaigns.md) plus
[ADR 0012](docs/decisions/0012-multi-instrument-routing-and-global-sequencing.md) plus
[ADR 0013](docs/decisions/0013-command-log-and-replay.md) plus
[ADR 0014](docs/decisions/0014-persisted-snapshots-and-log-suffix-recovery.md) plus
[ADR 0015](docs/decisions/0015-native-python-bindings-and-packaging.md) plus
[ADR 0016](docs/decisions/0016-reproducible-performance-evidence.md) for accepted rules.
The
test-only process, workload, campaign, and failure schemas are documented in
[Differential testing interface](docs/differential-testing.md).
The separate Phase 5 measurement schemas are documented in
[Benchmark evidence format](docs/benchmark-evidence-format.md).

## Roadmap

1. Ordered book sides, a per-book active-order index, direct cancellation, and full book
   invariants.
2. Limit/market matching, GTC/IOC residuals, replace, canonical digests, and deterministic
   command-stream evidence.
3. Independent Python reference model, differential generation, shrinking, and fuzzing.
4. Multi-instrument routing, command logging, deterministic replay, persisted recovery, and
   native-backed Python batch bindings and distribution.
5. Reproducible benchmarks and a profile-supported optimization study.
6. Optional versioned protocol and nonblocking Linux gateway after the core release is tagged.

More detail is maintained in [ROADMAP.md](ROADMAP.md).

## Non-goals

AtlasLOB is not a production exchange, broker, live trading system, or strategy. The initial
release does not include real exchange connectivity, financial-performance claims, distributed
consensus, kernel bypass, custom allocators, lock-free queues, or complex exchange order types.

## License

AtlasLOB is released under the [MIT License](LICENSE).
