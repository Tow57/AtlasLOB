# Contributing to AtlasLOB

AtlasLOB is currently a solo educational project, but changes follow a reviewable workflow so the
repository history remains useful engineering evidence.

## Before implementation

- State the observable outcome and explicit non-goals.
- Link the relevant semantic rule or architecture decision.
- Identify inputs, outputs, ownership, errors, and compatibility implications.
- Name the success, boundary, and failure tests that will prove completion.

## Local checks

The first testing-enabled configure fetches the immutable GoogleTest revision recorded in
`THIRD_PARTY_NOTICES.md`. Third-party targets must not inherit AtlasLOB's warning policy.

```sh
cmake --preset dev-gcc
cmake --build --preset dev-gcc
ctest --preset dev-gcc
```

When Clang is available, also run the `dev-clang` and `asan-ubsan` presets. The formatting gate is
pinned to clang-format 18.1.8; use that version when running `format-check` before submitting C++
changes.

Phase 3 Python evidence requires Python 3.11 or newer. The runtime package uses only the standard
library; its pinned optional development tools are installed with:

```sh
python -m venv .venv
python -m pip install -e ".[dev]"
python -m ruff format --check python
python -m ruff check python
python -m mypy
python -m pytest
```

Build `atlas_diff_native` before parity tests. The normal `build/dev-gcc` location is discovered
automatically; otherwise set `ATLAS_DIFF_NATIVE` to the executable's absolute path.

## Pull requests

Keep changes small enough to review. Describe behavior before and after, commands run, evidence,
known limitations, and documentation changes. Performance claims require a named workload and raw
results; ordinary feature changes should state that they make no performance claim.

## Definition of done

- Behavior agrees with the written semantic contract.
- Appropriate unit, integration, differential, or failure tests pass.
- Invariants and sanitizer implications are covered.
- Documentation and compatibility notes are current.
- No secret, private fixture, machine path, temporary log, or unsupported claim remains.
