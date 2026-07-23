# AtlasLOB

[![CI](https://github.com/Tow57/AtlasLOB/actions/workflows/ci.yml/badge.svg)](https://github.com/Tow57/AtlasLOB/actions/workflows/ci.yml)

AtlasLOB is a deterministic, in-memory limit order book and matching engine built primarily in
C++20, with Python reserved for independent validation, workload generation, bindings, and
benchmark analysis.

The project is being developed as a sequence of evidence-backed releases. Correctness,
reproducibility, and clear engineering tradeoffs take priority over feature count or unsupported
latency claims.

## Current status

**Phase 0: executable foundation**

| Capability | Status | Evidence |
| --- | --- | --- |
| Target-based C++20 build | Complete | `CMakeLists.txt`, named presets |
| Strong order and instrument values | Complete | Compile-time separation in unit tests |
| Deterministic new-order validation | Complete | `domain.validation` test |
| GCC and Clang CI | Configured; first run pending | `.github/workflows/ci.yml` |
| ASan and UBSan CI | Configured; first run pending | `asan-ubsan` preset and CI job |
| Resting book and matching | Planned | Phase 1 and Phase 2 |
| Replay, Python bindings, benchmarks, gateway | Planned | Later gated phases |

## Quick start

Requirements:

- CMake 3.25 or newer
- Ninja
- A C++20 compiler: GCC 13+ or Clang 17+

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

See [the semantic contract](docs/semantics.md) and
[ADR 0001](docs/decisions/0001-core-semantics.md) for the current rules.

## Roadmap

1. Stable order storage, FIFO price levels, book sides, cancellation, and full invariants.
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
