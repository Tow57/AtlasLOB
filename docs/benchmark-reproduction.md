# Benchmark reproduction guide

This guide runs the Phase 5 benchmark-contract tools and produces a self-verifying exploratory
evidence bundle. It does not turn a Windows, WSL, container, virtual machine, dirty checkout, or
shared runner into an official measurement host.

The exact field contract is in
[Benchmark evidence format](benchmark-evidence-format.md). The interpretation and claim rules are
in [Performance methodology](performance-methodology.md).

## 1. Build the opt-in tools

The benchmark presets use Release, `-O3`, `NDEBUG`, debug symbols, retained frame pointers, and no
invariants, sanitizers, or LTO:

```sh
cmake --preset release-benchmark-gcc
cmake --build --preset release-benchmark-gcc --parallel
cmake --build --preset release-benchmark-gcc --target benchmark-smoke
```

Use `release-benchmark-clang` to build the corresponding Clang binaries.
`release-profile` is code-generation-equivalent to that Clang preset and exists for Linux
sampling profiles.

To include the focused benchmark tests in a clean build directory, override the preset's normal
`BUILD_TESTING=OFF`:

```sh
cmake --preset release-benchmark-gcc -DBUILD_TESTING=ON
cmake --build --preset release-benchmark-gcc --parallel
ctest --test-dir build/release-benchmark-gcc -L benchmark --output-on-failure
```

Ordinary builds keep `ATLAS_BUILD_BENCHMARKS=OFF` and do not fetch Google Benchmark.

Install the Python development package into an isolated environment:

```sh
python -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/python -m pip install -e ".[dev]"
```

On Windows, replace `.venv/bin/python` with `.venv/Scripts/python`.

## 2. Materialize and verify a tiny workload

Materialization writes an adjacent canonical `.atlas` stream and
`ATLAS_BENCH_WORKLOAD_V1` manifest. This W04 smoke point has 64 preload commands, two complete
20-command warm-up cycles, and ten complete measured cycles:

```sh
python -m atlaslob.performance materialize \
  --output out/phase5-smoke/workloads \
  --workload W04 \
  --seed 1 \
  --preload 64 \
  --warmup 40 \
  --measured 200 \
  --active-orders 64 \
  --sweep-depth 16
```

The command prints the workload ID, manifest filename, and stream SHA-256.

Use a checked-in `ATLAS_BENCH_PLAN_V1` plan for the complete catalog:

```sh
python -m atlaslob.performance materialize \
  --plan benchmarks/plans/ci-smoke-v1.json \
  --output out/phase5-smoke/workloads \
  --log-materializer build/release-benchmark-gcc/atlas_bench_log_materializer
```

Run `materialize` without `--plan` or `--workload` only when also supplying the native log
materializer required by W10. W11 is not an accepted choice because the gateway is Phase 6 work.
Workload-specific alignment rules are checked before generation; for example, W04 warm-up and
measured regions are multiples of 20, W05 regions contain complete `sweep_depth + 1` cycles, and
W09 churn regions are even.

Before execution, `run-suite`, `analyze`, and `verify-bundle` deeply verify the manifest and
stream. Verification regenerates and executes the complete stream, so modifying either file is
detected before it can become valid timing evidence.

## 3. Capture a sanitized environment

Capture a separate environment manifest for each runner binary because `binary_sha256` is part of
the environment identity. The following is intentionally exploratory:

```sh
python -m atlaslob.performance capture-environment \
  --binary build/release-benchmark-gcc/atlas_bench_runner \
  --build-directory build/release-benchmark-gcc \
  --output out/phase5-smoke/environment-timed.json \
  --repository . \
  --host-class exploratory-local \
  --storage-class local-nvme \
  --smt-sibling-idle unknown
```

The collector derives compiler identity, effective flags, target closure, and build policy from
the retained `CMakeCache.txt` and `compile_commands.json`; callers cannot assert those facts
manually. Do not put a username, hostname, serial number, IP address, email address, or private
path in any argument.

For the allocation runner, repeat the command with:

```text
--binary build/release-benchmark-gcc/atlas_bench_alloc_runner
--output out/phase5-smoke/environment-allocation.json
```

On the authoritative host, pin the capture and the entire suite to the same selected nonzero CPU,
leave its SMT sibling idle, supply `--smt-sibling-idle yes`, and add `--official`. The collector
fails if any official prerequisite is absent. It records but does not change the governor, turbo,
SMT, NUMA, or kernel configuration.

### Python measurement wheel

Build each CPython-minor-specific measurement wheel directly with scikit-build-core and retain its
build directory. The build directory is required provenance, not disposable scratch space:

```sh
python -m pip wheel . --no-deps \
  --wheel-dir out/phase5-python-wheel \
  -Cbuild-dir=build/python-measurement-cp312 \
  -Ccmake.define.CMAKE_BUILD_TYPE=Release \
  -Ccmake.define.CMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -Ccmake.define.ATLAS_ENABLE_INVARIANTS=OFF \
  -Ccmake.define.ATLAS_ENABLE_ASAN=OFF \
  -Ccmake.define.ATLAS_ENABLE_UBSAN=OFF \
  -Ccmake.define.ATLAS_ENABLE_TSAN=OFF \
  -Ccmake.define.ATLAS_WARNINGS_AS_ERRORS=ON \
  -Ccmake.define.CMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
  -Ccmake.define.CMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -g -fno-omit-frame-pointer"
```

Install that exact wheel into a clean target environment outside the checkout, then capture its
identity:

```sh
python -m atlaslob.performance capture-environment \
  --target-python /opt/atlaslob-cp312/bin/python \
  --wheel out/phase5-python-wheel/<atlaslob-cp312-wheel>.whl \
  --python-worker benchmarks/python/atlas_python_bench_worker.py \
  --build-directory build/python-measurement-cp312 \
  --output out/phase5-smoke/environment-python-cp312.json \
  --repository . \
  --host-class exploratory-local \
  --storage-class local-nvme \
  --smt-sibling-idle unknown
```

Capture verifies that the installed wrapper and extension match the supplied wheel and that the
extension digest occurs in the retained build directory. A repaired manylinux release wheel may
change native bytes, so use the direct, unrepaired wheel for measurement provenance; release
manylinux artifacts remain a separate packaging deliverable.

## 4. Run independent process observations

Use the manifest path printed by materialization. One runner process produces one retained
observation:

```sh
python -m atlaslob.performance run-suite \
  --manifest out/phase5-smoke/workloads/<w04-manifest>.json \
  --output out/phase5-smoke/observations-throughput \
  --runner build/release-benchmark-gcc/atlas_bench_runner \
  --environment out/phase5-smoke/environment-timed.json \
  --suite-label phase5-smoke-w04-throughput \
  --mode throughput \
  --observations 10 \
  --timeout 900
```

Repeat into a new empty output directory with `--mode latency`. For allocation evidence, use
`atlas_bench_alloc_runner`, its matching environment file, and `--mode allocation`.

The output directory must be empty. Domain or state rejections do not stop a workload; a terminal
engine error does. A timeout or invalid runner result is retained with `valid=false` and is not
silently retried.

For a controlled experiment, add both:

```text
--candidate-runner <candidate-atlas_bench_runner>
--candidate-environment <candidate-environment.json>
```

With a candidate, `--observations 5` means five A-B-B-A blocks and therefore twenty process
observations. Baseline and candidate binary digests must differ, while their host-context digests
must match.

For a Python batch point, keep the target interpreter, wheel, worker, and output mode explicit:

```sh
python -m atlaslob.performance run-suite \
  --manifest out/phase5-smoke/workloads/<w04-manifest>.json \
  --output out/phase5-smoke/observations-python-summary-1024 \
  --runner /opt/atlaslob-cp312/bin/python \
  --environment out/phase5-smoke/environment-python-cp312.json \
  --wheel out/phase5-python-wheel/<atlaslob-cp312-wheel>.whl \
  --python-worker benchmarks/python/atlas_python_bench_worker.py \
  --suite-label phase5-smoke-w04-python-summary-1024 \
  --mode python-summary \
  --batch-size 1024 \
  --observations 10 \
  --timeout 900
```

## 5. Analyze without hiding invalid runs

Analysis needs every manifest and environment referenced by its input observations:

```sh
python -m atlaslob.performance analyze \
  out/phase5-smoke/observations-throughput \
  --manifest out/phase5-smoke/workloads/<w04-manifest>.json \
  --environment out/phase5-smoke/environment-timed.json \
  --output out/phase5-smoke/report.json \
  --markdown out/phase5-smoke/report.md \
  --svg out/phase5-smoke/report.svg
```

Regenerating the report from the same canonical inputs produces byte-identical JSON, Markdown, and
SVG. Rates are derived here from raw integer counts and elapsed nanoseconds. Invalid observations
remain counted but are excluded from aggregates.

## 6. Assemble and verify a portable bundle

Place these files under one new bundle directory:

- each workload manifest and its adjacent `.atlas` stream;
- each referenced environment manifest;
- every raw observation;
- the canonical report JSON; and
- optional Markdown, SVG, `perf`, experiment, and limitation artifacts.

Paths inside the bundle may use subdirectories. They must be relative and the tree must contain no
symbolic links. Create the SHA-256 inventory and immediately verify the cross-file graph:

```sh
python -m atlaslob.performance verify-bundle \
  out/phase5-smoke/bundle \
  --create-inventory

python -m atlaslob.performance verify-bundle out/phase5-smoke/bundle
```

Verification checks the exact file set, every file digest, canonical schemas, regenerated
workloads, observation-to-workload/environment bindings, and byte-exact report regeneration.

## Official-run additions

The native-host run book adds controls that are intentionally absent from exploratory smoke:

1. start from a clean checkout of the exact commit or tag;
2. record one sanitized host class and select one nonzero physical-core logical CPU;
3. invoke environment capture and every suite under the same OS affinity;
4. confirm the SMT sibling remains idle without changing security settings automatically;
5. retain at least ten valid independent processes per result point;
6. use five A-B-B-A blocks for every baseline/candidate comparison;
7. retain timeouts and all invalid runs;
8. collect `perf stat` and call graphs separately from core timing;
9. keep replay, Python, allocation, memory, throughput, and latency boundaries separate; and
10. inventory the exact-tag bundle before publishing any number.

Until all official prerequisites and scales in
[Performance methodology](performance-methodology.md#required-scales) are satisfied, generated
reports must remain labeled exploratory and cannot support a portfolio performance claim.
