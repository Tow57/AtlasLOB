# Contributing to AtlasLOB

AtlasLOB is currently a solo educational project, but changes follow a reviewable workflow so the
repository history remains useful engineering evidence.

## Before implementation

- State the observable outcome and explicit non-goals.
- Link the relevant semantic rule or architecture decision.
- Identify inputs, outputs, ownership, errors, and compatibility implications.
- Name the success, boundary, and failure tests that will prove completion.

## Local checks

```sh
cmake --preset dev-gcc
cmake --build --preset dev-gcc
ctest --preset dev-gcc
```

When Clang is available, also run the `dev-clang` and `asan-ubsan` presets. Run `format-check`
before submitting C++ changes.

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
