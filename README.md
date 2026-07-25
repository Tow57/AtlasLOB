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

**Phase 2 matching MVP is complete on `main`. Phase 3 implementation now includes the independent
Python oracle, deterministic valid/invalid workload generation, fixed and rotating campaigns, a
streaming differential runner, portable failure bundles, semantic shrinking, metamorphic checks,
and bounded cross-language fuzzing. Local closure evidence now includes 244 passed, 2
Windows-symlink skips, and 11 deselected in the default Python selection; 11 passed and 246
deselected in the marked campaign/fuzz selection; the fixed 10-by-5,000 exact PR corpus; one
checked 1,000,000-command compact case; and GCC Debug and Release CTest at 288/288 each. The
`BUILD_TESTING=OFF` production build, pinned clang-format, Ruff, strict mypy, and wheel
build/install smoke also pass locally. Publication and the hosted GCC, Clang, ASan/UBSan,
formatting, PR-corpus, and Linux link-safety checks remain pending, so Phase 3 is not yet declared
complete. Phase 4 has not started.**

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
| Metamorphic and bounded fuzz evidence | Complete locally | Five properties, two fuzz paths, checked seed corpus |
| Phase 3 long campaign and final release gates | Pending | Local million-command and final gates pass; hosted evidence pending |
| GCC and Clang CI | Defined; Phase 3 branch evidence pending publication | `.github/workflows/ci.yml` |
| ASan and UBSan CI | Defined; Phase 3 branch evidence pending publication | `asan-ubsan` preset and CI job |
| Pinned clang-format gate | Passes locally; hosted Phase 3 evidence pending publication | `format-check` CI job |
| Resting book structure | Complete | `stress.InstrumentBookStress*` |
| Matching and normalized command execution | Complete | Phase 2 |
| Replay, Python bindings, benchmarks, gateway | Planned | Later gated phases |

## Quick start

Requirements:

- CMake 3.25 or newer
- Ninja
- Git, used by CMake to fetch the pinned test-only GoogleTest dependency
- A C++20 compiler: GCC 13+ or Clang 17+
- Python 3.11 through 3.14 for the independent correctness-evidence package

The first testing-enabled configure downloads GoogleTest 1.17.0 at an immutable commit. Production
library builds configured with `BUILD_TESTING=OFF` do not fetch or link GoogleTest.

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
.venv/bin/python -m ruff format --check python
.venv/bin/python -m ruff check python
.venv/bin/python -m mypy
.venv/bin/python -m pytest
```

Use the virtual environment directly from Windows PowerShell:

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -e ".[dev]"
cmake --preset dev-gcc
cmake --build --preset dev-gcc --target atlas_diff_native
.\.venv\Scripts\python.exe -m ruff format --check python
.\.venv\Scripts\python.exe -m ruff check python
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
[ADR 0011](docs/decisions/0011-deterministic-differential-campaigns.md) for accepted rules. The
test-only process, workload, campaign, and failure schemas are documented in
[Differential testing interface](docs/differential-testing.md).

## Roadmap

1. Ordered book sides, a global active-order index, direct cancellation, and full book invariants.
2. Limit/market matching, GTC/IOC residuals, replace, canonical digests, and deterministic
   command-stream evidence.
3. Independent Python reference model, differential generation, shrinking, and fuzzing.
4. Command logging, deterministic replay, native-backed Python batch bindings and distribution,
   and analysis tooling.
5. Reproducible benchmarks and a profile-supported optimization study.
6. Optional versioned protocol and nonblocking Linux gateway after the core release is tagged.

More detail is maintained in [ROADMAP.md](ROADMAP.md).

## Non-goals

AtlasLOB is not a production exchange, broker, live trading system, or strategy. The initial
release does not include real exchange connectivity, financial-performance claims, distributed
consensus, kernel bypass, custom allocators, lock-free queues, or complex exchange order types.

## License

AtlasLOB is released under the [MIT License](LICENSE).
