# Phase 5 native-host baseline runbook

This runbook executes the unchanged Phase 4 matching engine through the Phase 5 campaign tools.
It does not install Ubuntu, repartition a disk, change firmware, change CPU policy, weaken
`perf_event_paranoid`, or publish a performance claim.

The official campaign requires bare-metal Ubuntu 24.04 x86-64. Until every qualification step
passes, use an exploratory bundle and do not copy its timings into portfolio material.

## 1. Prepare the dedicated Ubuntu installation

Use the dedicated local NVMe/ext4 Ubuntu installation on the Ryzen workstation. Boot Ubuntu
directly, close interactive applications, and do not run another AtlasLOB process on the selected
core or its SMT sibling.

Install the required development packages:

```sh
sudo apt-get update
sudo apt-get install --yes \
  build-essential clang cmake git ninja-build \
  linux-tools-common "linux-tools-$(uname -r)" \
  python3.12 python3.12-dev python3.12-venv python3-pip
```

Start from a clean checkout of the exact PR head:

```sh
git status --short
git rev-parse HEAD
git submodule status
```

The first command must print nothing. Record the full commit in the campaign journal. Dependency
downloads during configuration are allowed; disconnecting the host is not required.

## 2. Select one CPU without changing host policy

Inspect the topology:

```sh
lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE
for path in /sys/devices/system/cpu/cpu[0-9]*/topology/thread_siblings_list; do
  printf '%s %s\n' "$path" "$(cat "$path")"
done
```

Choose the lowest online nonzero logical CPU whose `thread_siblings_list` contains two CPUs.
Record the other member as its SMT sibling. If the lowest member is CPU 0, select the nonzero
member only when CPU 0 is its sibling; otherwise advance to the next physical core.

Set task-specific variables after inspecting the actual topology:

```sh
export ATLAS_PHASE5_CPU=<selected-nonzero-cpu>
export ATLAS_PHASE5_SIBLING=<idle-smt-sibling>
export ATLAS_PHASE5_HOST=ryzen-9800x3d-64g-ubuntu2404-a
export ATLAS_PHASE5_STORAGE=local-nvme-ext4
```

Do not offline the sibling or change SMT, turbo, the governor, NUMA policy, or kernel security
settings. Confirm manually that no user workload is assigned to the sibling. Every capture and
campaign command below is launched through `taskset -c "$ATLAS_PHASE5_CPU"`; child benchmark
processes inherit that affinity.

## 3. Build the frozen binaries and Python controller

```sh
python3.12 -m venv .venv-phase5
.venv-phase5/bin/python -m pip install --disable-pip-version-check -e ".[dev]"

cmake --preset release-benchmark-gcc
cmake --build --preset release-benchmark-gcc --parallel

cmake --preset release-benchmark-clang
cmake --build --preset release-benchmark-clang --parallel

cmake --preset release-profile
cmake --build --preset release-profile --parallel
```

GCC is a build and correctness cross-check. The official timed runner is
`build/release-benchmark-clang/atlas_bench_runner`; call graphs use the code-generation-equivalent
`build/release-profile/atlas_bench_runner`.

Build the unrepaired CPython 3.12 measurement wheel and install it into a clean target environment:

```sh
mkdir -p out/phase5-python-wheel
.venv-phase5/bin/python -m pip wheel . --no-deps \
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

python3.12 -m venv out/phase5-python-target-cp312
out/phase5-python-target-cp312/bin/python -m pip install \
  --disable-pip-version-check --no-deps out/phase5-python-wheel/atlaslob-*.whl
out/phase5-python-target-cp312/bin/python -m pip check
```

Resolve and record the single wheel path:

```sh
set -- out/phase5-python-wheel/atlaslob-*.whl
test "$#" -eq 1
export ATLAS_PHASE5_WHEEL="$1"
```

## 4. Run the official qualification smoke

First repeat the checked CI plan in a disposable exploratory directory. Use one observation per
shape and confirm every result remains valid. This step does not produce official evidence:

```sh
smoke=out/phase5-native-smoke
mkdir -p "$smoke/workloads" "$smoke/environments" "$smoke/harness"
cp benchmarks/python/atlas_python_bench_worker.py "$smoke/harness/"

.venv-phase5/bin/python -m atlaslob.performance materialize \
  --plan benchmarks/plans/ci-smoke-v1.json \
  --output "$smoke/workloads" \
  --log-materializer build/release-benchmark-clang/atlas_bench_log_materializer
```

Capture exploratory environments without `--official`:

```sh
for role in core allocation; do
  binary=build/release-benchmark-clang/atlas_bench_runner
  if test "$role" = allocation; then
    binary=build/release-benchmark-clang/atlas_bench_alloc_runner
  fi
  taskset -c "$ATLAS_PHASE5_CPU" \
    .venv-phase5/bin/python -m atlaslob.performance capture-environment \
    --binary "$binary" --build-directory build/release-benchmark-clang \
    --output "$smoke/environments/$role.json" --repository . \
    --host-class "$ATLAS_PHASE5_HOST" --storage-class "$ATLAS_PHASE5_STORAGE" \
    --smt-sibling-idle yes
done

taskset -c "$ATLAS_PHASE5_CPU" \
  .venv-phase5/bin/python -m atlaslob.performance capture-environment \
  --target-python out/phase5-python-target-cp312/bin/python \
  --wheel "$ATLAS_PHASE5_WHEEL" \
  --python-worker "$smoke/harness/atlas_python_bench_worker.py" \
  --build-directory build/python-measurement-cp312 \
  --output "$smoke/environments/python-cp312.json" --repository . \
  --host-class "$ATLAS_PHASE5_HOST" --storage-class "$ATLAS_PHASE5_STORAGE" \
  --smt-sibling-idle yes
```

Run and finalize the complete smoke plan:

```sh
taskset -c "$ATLAS_PHASE5_CPU" \
  .venv-phase5/bin/python -m atlaslob.performance run-campaign \
  --plan benchmarks/plans/ci-smoke-v1.json --bundle "$smoke" \
  --runner build/release-benchmark-clang/atlas_bench_runner \
  --environment "$smoke/environments/core.json" \
  --allocation-runner build/release-benchmark-clang/atlas_bench_alloc_runner \
  --allocation-environment "$smoke/environments/allocation.json" \
  --python-runner out/phase5-python-target-cp312/bin/python \
  --python-environment "$smoke/environments/python-cp312.json" \
  --wheel "$ATLAS_PHASE5_WHEEL" \
  --python-worker "$smoke/harness/atlas_python_bench_worker.py" \
  --suite-label phase5-host-smoke --valid-observations 1 --max-attempts 1

.venv-phase5/bin/python -m atlaslob.performance finalize-campaign \
  --plan benchmarks/plans/ci-smoke-v1.json --bundle "$smoke" \
  --valid-observations 1 --allow-exploratory
```

## 5. Materialize and qualify the official campaign

Create the ignored bundle and materialize every frozen point once:

```sh
bundle=out/phase5-baseline/bundle
mkdir -p "$bundle/workloads" "$bundle/environments" "$bundle/harness"
cp benchmarks/python/atlas_python_bench_worker.py "$bundle/harness/"

taskset -c "$ATLAS_PHASE5_CPU" \
  .venv-phase5/bin/python -m atlaslob.performance materialize \
  --plan benchmarks/plans/phase5-study-v1.json \
  --output "$bundle/workloads" \
  --log-materializer build/release-benchmark-clang/atlas_bench_log_materializer
```

Capture the three environment identities:

```sh
taskset -c "$ATLAS_PHASE5_CPU" \
  .venv-phase5/bin/python -m atlaslob.performance capture-environment \
  --binary build/release-benchmark-clang/atlas_bench_runner \
  --build-directory build/release-benchmark-clang \
  --output "$bundle/environments/core.json" \
  --repository . --host-class "$ATLAS_PHASE5_HOST" \
  --storage-class "$ATLAS_PHASE5_STORAGE" --smt-sibling-idle yes --official

taskset -c "$ATLAS_PHASE5_CPU" \
  .venv-phase5/bin/python -m atlaslob.performance capture-environment \
  --binary build/release-benchmark-clang/atlas_bench_alloc_runner \
  --build-directory build/release-benchmark-clang \
  --output "$bundle/environments/allocation.json" \
  --repository . --host-class "$ATLAS_PHASE5_HOST" \
  --storage-class "$ATLAS_PHASE5_STORAGE" --smt-sibling-idle yes --official

taskset -c "$ATLAS_PHASE5_CPU" \
  .venv-phase5/bin/python -m atlaslob.performance capture-environment \
  --target-python out/phase5-python-target-cp312/bin/python \
  --wheel "$ATLAS_PHASE5_WHEEL" \
  --python-worker "$bundle/harness/atlas_python_bench_worker.py" \
  --build-directory build/python-measurement-cp312 \
  --output "$bundle/environments/python-cp312.json" \
  --repository . --host-class "$ATLAS_PHASE5_HOST" \
  --storage-class "$ATLAS_PHASE5_STORAGE" --smt-sibling-idle yes --official
```

Every command must print `official`. A failure is a qualification failure, not permission to edit
the captured manifest.

## 6. Run the resumable 510-observation baseline

Define the shared campaign arguments:

```sh
campaign_args=(
  --plan benchmarks/plans/phase5-study-v1.json
  --bundle "$bundle"
  --runner build/release-benchmark-clang/atlas_bench_runner
  --environment "$bundle/environments/core.json"
  --allocation-runner build/release-benchmark-clang/atlas_bench_alloc_runner
  --allocation-environment "$bundle/environments/allocation.json"
  --python-runner out/phase5-python-target-cp312/bin/python
  --python-environment "$bundle/environments/python-cp312.json"
  --wheel "$ATLAS_PHASE5_WHEEL"
  --python-worker "$bundle/harness/atlas_python_bench_worker.py"
  --suite-label phase5-baseline
  --valid-observations 10
  --max-attempts 20
  --timeout 900
)
```

Run tiers in this order:

```sh
mkdir -p out/phase5-baseline/checkpoints
for tier in study memory replay python headline; do
  taskset -c "$ATLAS_PHASE5_CPU" \
    .venv-phase5/bin/python -m atlaslob.performance run-campaign \
    "${campaign_args[@]}" --tier "$tier" --resume

  checkpoint="out/phase5-baseline/checkpoints/after-$tier.sha256"
  find "$bundle" -type f -print0 |
    sort -z |
    xargs -0 sha256sum >"$checkpoint"
  sha256sum --check "$checkpoint"
done
```

The driver runs attempts round-robin within each tier. It stops after retaining the first invalid
attempt. Diagnose the cause, keep the attempt, and use the same command with `--resume`; the next
attempt receives a new directory. Never delete a timeout or invalid observation.

Each successful tier writes and immediately verifies a deterministic file-level checkpoint outside
the bundle. Preserve those checkpoint files with the final archive; later tiers add evidence but do
not rewrite an earlier attempt.

Replay uses the manifest's `warm_page_cache` policy. Stream generation, deep verification, and
page-cache warming occur outside the measured runner region.

## 7. Capture separate W04 and W05 profiles

Capture the profiling runner environment outside the finalizable bundle:

```sh
profiles=out/phase5-baseline/profiles
mkdir -p "$profiles"
taskset -c "$ATLAS_PHASE5_CPU" \
  .venv-phase5/bin/python -m atlaslob.performance capture-environment \
  --binary build/release-profile/atlas_bench_runner \
  --build-directory build/release-profile \
  --output "$profiles/environment-profile.json" \
  --repository . --host-class "$ATLAS_PHASE5_HOST" \
  --storage-class "$ATLAS_PHASE5_STORAGE" --smt-sibling-idle yes --official
```

Use these exact study manifests:

```sh
w04="$bundle/workloads/w04-g1-s104-p65536-w100000-m1000000-i1-a65536-d16.json"
w05="$bundle/workloads/w05-g1-s564-p65536-w65000-m999960-i1-a65536-d64.json"

for profile_point in "w04:$w04" "w05:$w05"; do
  workload="${profile_point%%:*}"
  manifest="${profile_point#*:}"
  taskset -c "$ATLAS_PHASE5_CPU" \
    .venv-phase5/bin/python -m atlaslob.performance capture-profile \
    --manifest "$manifest" --output "$profiles/$workload-stat" \
    --runner build/release-profile/atlas_bench_runner \
    --environment "$profiles/environment-profile.json" \
    --perf /usr/bin/perf --suite-label phase5-profile \
    --kind stat --observations 10 --timeout 900

  taskset -c "$ATLAS_PHASE5_CPU" \
    .venv-phase5/bin/python -m atlaslob.performance capture-profile \
    --manifest "$manifest" --output "$profiles/$workload-record" \
    --runner build/release-profile/atlas_bench_runner \
    --environment "$profiles/environment-profile.json" \
    --perf /usr/bin/perf --suite-label phase5-profile \
    --kind record --observations 1 --timeout 900
done
```

If counters are inaccessible, retain the failed capture, record the limitation, and do not change
kernel security settings. `perf.data` remains local; only sanitized text reports and hashes enter
the PR evidence.

## 8. Finalize, verify, and checkpoint

```sh
taskset -c "$ATLAS_PHASE5_CPU" \
  .venv-phase5/bin/python -m atlaslob.performance finalize-campaign \
  --plan benchmarks/plans/phase5-study-v1.json \
  --bundle "$bundle" --valid-observations 10

.venv-phase5/bin/python -m atlaslob.performance verify-bundle "$bundle"
```

Finalization emits separate byte-regenerable reports for core, allocation, and CPython build
contexts while verifying that every environment describes the same physical host.

Create a local checkpoint only after verification:

```sh
tar --create --file out/phase5-baseline-bundle.tar -C out/phase5-baseline bundle profiles
sha256sum out/phase5-baseline-bundle.tar \
  >out/phase5-baseline-bundle.tar.sha256
sha256sum --check out/phase5-baseline-bundle.tar.sha256
```

Copy both files to a second local filesystem and re-run `sha256sum --check` there. The PR commits
only sanitized compact reports, profile text, limitations, hotspot analysis, and raw evidence
hashes. Large streams, latency observations, and `perf.data` remain outside Git until the exact-tag
release workflow.
