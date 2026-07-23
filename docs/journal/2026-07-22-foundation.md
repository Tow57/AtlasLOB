# 2026-07-22 - Executable foundation

## Goal

Create a public-ready repository whose first commit already builds, tests, and demonstrates one
deterministic domain behavior.

## What changed

- Added target-based C++20 CMake configuration and compiler presets.
- Added strong domain values and deterministic new-order validation.
- Added a CLI validation demonstration and dependency-free CTest suite.
- Added Linux GCC, Clang, formatting, ASan, and UBSan workflow definitions.
- Documented semantics, scope, contribution rules, roadmap, and the first architecture decision.

## Local evidence

Environment:

- Windows development checkout
- CMake 4.0.1
- Ninja 1.12.1
- MinGW-w64 GCC 15.1.0

Commands completed successfully:

```text
cmake --preset dev-gcc
cmake --build --preset dev-gcc --parallel 2
ctest --preset dev-gcc

cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release

build/dev-gcc/atlas_cli.exe validate-demo
```

Both configurations now pass three tests: `domain.validation`, `cli.validation_demo`, and
`cli.invalid_usage`. The negative-path test confirms that invalid CLI usage prints a diagnostic and
returns a nonzero exit code.

The first hosted GCC, Clang, and sanitizer jobs passed. The formatter job exposed output drift
between formatter packages, so the workflow now provisions clang-format 18.1.8 in an isolated
environment and passes its exact path into CMake. A fully green run remains the release gate.

## What surprised me

The local machine has a current GCC/Ninja toolchain but no local Clang installation or general
Ubuntu WSL distribution. Linux Clang and sanitizer support therefore needs to be established by
the pinned GitHub Actions workflow before it becomes verified evidence.

## Next decision

Choose and document stable owning storage for resting order nodes before implementing a mutable
price level.
