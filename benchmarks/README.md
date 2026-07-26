# AtlasLOB benchmark tools

This directory contains opt-in Phase 5 measurement tools. It is not part of the installed C++ or
Python runtime.

Configure an exploratory benchmark build with one of the named presets:

```sh
cmake --preset release-benchmark-gcc
cmake --build --preset release-benchmark-gcc --parallel
cmake --build --preset release-benchmark-gcc --target benchmark-smoke
```

The Clang benchmark and `release-profile` presets retain symbols and frame pointers for Linux
`perf`. All three presets disable expensive invariants, sanitizers, and LTO. Their measurements
remain exploratory unless the environment and exact-tag requirements in
[`docs/performance-methodology.md`](../docs/performance-methodology.md) are satisfied.

## Boundaries

- `atlas_microbench` answers focused component questions using Google Benchmark.
- `atlas_bench_runner` emits exactly one `core_throughput`, `core_latency`,
  `core_construction`, `core_preload`, `replay_fast`, or `replay_verify` observation per process.
- `atlas_bench_alloc_runner` emits exactly one steady-region or construction-plus-preload
  allocation observation (`core_allocation` or `core_setup_allocation`) and must not be used for
  a latency claim.
- `atlas_bench_log_materializer` converts a verified W10 command stream into its deterministic
  `ATLSLG01` timed input before any replay observation.
- `atlas_python_bench_worker.py` is the standalone installed-wheel worker for the separately
  labeled `python_objects`, `python_columns`, and `python_summary` batch boundaries.

Those are the eleven closed V1 boundaries. There is no gateway boundary: W11 remains deferred to
Phase 6.

The Python command groups are `materialize`, `capture-environment`, `run-suite`, `analyze`, and
`verify-bundle`. Run `python -m atlaslob.performance <group> --help` for exact arguments.

See the [benchmark reproduction guide](../docs/benchmark-reproduction.md) for a copy-paste
smoke-to-bundle workflow and the
[benchmark evidence format](../docs/benchmark-evidence-format.md) for exact schema fields,
canonical JSON rules, and hash bindings.

For ordinary core execution, fixture generation and parsing, construction, preload, warm-up,
validation replay, event inspection, final state hashing, report analysis, and file output remain
outside timing. Returned `EngineResult` and owned event-batch destruction remain inside. The
dedicated construction and preload modes label those costs separately.

Replay timing uses an exact digest-bound W10 log after an untimed validation scan warms the page
cache. Python timing starts with already parsed commands and prepared batch slices; it includes
per-call conversion, native execution, result materialization, and result destruction, while
prefix execution, final hashing, and independent validation remain outside.

## Evidence integrity

Large benchmark streams are generated from checked plans into `out/` or another ignored
directory. Every runner invocation binds the exact manifest, stream, environment, host context,
binary, suite label, and measurement shape, then verifies expected counts and digests. W10 also
binds and verifies its `.atlslg` input. A suite label is a safe identifier of at most 32
characters. An observation that cannot complete its checks is retained as invalid rather than
reported as a timing result.

Bundle verification requires every workload and environment to be used, every observation to be
covered by exactly one report, every stream/log/rendering/experiment note to be referenced, and
every retained file to appear in the SHA-256 inventory.

Shared CI runs dry and tiny smoke cases only. It contains no wall-clock regression threshold and
does not authorize a public performance claim. Native Ubuntu baselines and official Phase 5
evidence have not started.
