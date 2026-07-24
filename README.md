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

**Phase 2 New/Cancel matching execution complete locally on the current development branch:
price-time matching, GTC/IOC/market outcomes, atomic cancellation, and normalized event batches
are implemented; Replace, digests, and hosted PR gates remain**

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
| Active-order index and direct cancellation | Complete locally; hosted gates pending | `core.ActiveOrderIndex*`, `core.InstrumentBook*`, ADR 0005 |
| Sequenced command admission and state validation | Complete locally; hosted gates pending | `core.CommandAdmission*`, ADR 0006 |
| Read-only match planning and final-capacity projection | Complete locally; hosted gates pending | `core.MatchPlan*`, ADR 0006 |
| Owned normalized event batches and prepared residuals | Complete locally; hosted gates pending | `core.EventBatchBuilder*`, `core.InstrumentBookPreparedRest*` |
| Atomic limit/market New execution | Complete locally; hosted gates pending | `core.CommandExecutor*`, ADR 0007 |
| Sequenced Cancel execution and normalized events | Complete locally; hosted gates pending | `core.CommandExecutorCancel*`, ADR 0007 |
| GCC and Clang CI | Passing on `main`; required per PR | `.github/workflows/ci.yml` |
| ASan and UBSan CI | Passing on `main`; required per PR | `asan-ubsan` preset and CI job |
| Pinned clang-format gate | Passing on `main`; required per PR | `format-check` CI job |
| Resting book structure | Complete locally; hosted gates pending | `stress.InstrumentBookStress*` |
| Matching and normalized command execution | New/Cancel complete; Replace pending | Phase 2 |
| Replay, Python bindings, benchmarks, gateway | Planned | Later gated phases |

## Quick start

Requirements:

- CMake 3.25 or newer
- Ninja
- Git, used by CMake to fetch the pinned test-only GoogleTest dependency
- A C++20 compiler: GCC 13+ or Clang 17+

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

See [the semantic contract](docs/semantics.md) and
[ADR 0001](docs/decisions/0001-core-semantics.md) plus
[ADR 0002](docs/decisions/0002-command-sequencing-and-identity.md) plus
[ADR 0003](docs/decisions/0003-stable-order-storage-and-price-levels.md) plus
[ADR 0004](docs/decisions/0004-ordered-book-sides.md) plus
[ADR 0005](docs/decisions/0005-indexed-order-book-and-cancellation.md) plus
[ADR 0006](docs/decisions/0006-command-admission-and-execution-preparation.md) plus
[ADR 0007](docs/decisions/0007-atomic-new-and-cancel-execution.md) for accepted rules.

## Roadmap

1. Ordered book sides, a global active-order index, direct cancellation, and full book invariants.
2. Limit/market matching, GTC/IOC residuals, replace, and normalized event digests.
3. Independent Python reference model, differential generation, shrinking, and fuzzing.
4. Command logging, deterministic replay, Python batch bindings, and analysis tooling.
5. Reproducible benchmarks and a profile-supported optimization study.
6. Optional versioned protocol and nonblocking Linux gateway after the core release is tagged.

More detail is maintained in [ROADMAP.md](ROADMAP.md).

## Non-goals

AtlasLOB is not a production exchange, broker, live trading system, or strategy. The initial
release does not include real exchange connectivity, financial-performance claims, distributed
consensus, kernel bypass, custom allocators, lock-free queues, or complex exchange order types.

## License

AtlasLOB is released under the [MIT License](LICENSE).
