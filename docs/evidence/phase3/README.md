# Phase 3 correctness-evidence index

This directory records what proves the Phase 3 differential-testing release and distinguishes
implemented policy from campaigns that have actually completed. Generated streams and hosted
artifacts are intentionally not committed here.

## Implemented evidence surfaces

| Surface | Version or location | Purpose |
| --- | --- | --- |
| Generator | `GENERATOR_VERSION = 1` | Reproduce a stream from one resolved spec and seed |
| Workload manifest | `atlas_workload_manifest_v1` | Bind resolved inputs, stream digest, and intent statistics |
| Campaign suite | `atlas_campaign_suite_v1` | Name ordered cases and exact/compact mode |
| Campaign policy | `atlas_predefined_campaign_v1` | Bind tier sizes, profiles, seed provenance, and cadence |
| Reference evidence | `atlas_reference_evidence_v1` | Persist the complete independent reference pass |
| Native evidence | `atlas_diff_v1` | Observe the public C++ engine through the test-only process |
| Failure report | `atlas_failure_report_v1` | Reproduce and review the first semantic divergence |

Checked campaign policy documents are under `python/campaigns/v1`. Fixed fuzz seeds are under
`python/corpus/fuzz/v1`. The three minimized evidence-boundary fault demonstrations are under
`python/corpus/regressions/phase3-injected`.

## Campaign policy

| Tier | Cases × commands | Mode | Snapshot cadence | Seeds | Trigger | Artifact retention |
| --- | ---: | --- | ---: | --- | --- | ---: |
| PR | 10 × 5,000 | Exact | 100 | Fixed V1 | Every pull request | 14 days |
| Main | 20 × 100,000 | Exact | 1,000 | Explicit rotating epoch | Push to `main` or manual | 30 days |
| Nightly | 10 × 1,000,000 | Compact | 10,000 | Explicit rotating epoch | Nightly or manual | 30 days |
| Release | 10 × 10,000,000 | Compact | 50,000 | Published V1 | Manual | 90 days |
| Release sanitizer | 10 × 100,000 | Exact | 1,000 | Published V1 | Manual | 90 days |

By default, no semantic divergence is diagnosed from the initial process alone. The runner
regenerates and compares an exact replay from fresh engines. A command divergence takes a full
snapshot at the divergent command; a terminal divergence replays and snapshots the complete
workload.

Manual Release work is sharded by compiler and case behind a complete GCC/Clang Release
build-and-CTest prerequisite; the sanitizer subset is sharded per case. These capacity-bound jobs
pass `--exact-replay-command-limit 1000000`. A larger divergent prefix remains a semantic failure
with `diagnosis.status=deferred_command_limit`, retains its portable workload, manifest, first
difference, and digests, and requires manual exact reproduction on a suitable host. It does not
count as passing release evidence. Standard diagnostic bundles retain both evidence streams;
summary-only large-tier bundles may omit them.

## Evidence collected on 2026-07-24

- The local default Python selection passed 244 tests with 2 Windows-symlink skips and 11
  deselected.
- The local `campaign or differential_fuzz` selection passed 11 tests with 246 deselected.
- The local fixed PR campaign passed all 10 cases and all 50,000 commands in exact mode.
- The checked [million-command summary](nightly-one-million.json) records epoch-0 nightly case 0
  passing all 1,000,000 commands in compact mode.
- GCC Debug and Release CTest each passed 288/288.
- The `BUILD_TESTING=OFF` production build, pinned clang-format, Ruff, strict mypy, and wheel
  build/install smoke gates passed locally.
- Hosted CI [run 30141147909](https://github.com/Tow57/AtlasLOB/actions/runs/30141147909) on
  implementation head
  [`b13298572c353e139a58dcd6b077eb67536b01b2`](https://github.com/Tow57/AtlasLOB/commit/b13298572c353e139a58dcd6b077eb67536b01b2)
  passed GCC and Clang Debug, GCC and Clang Release, ASan/UBSan, Python 3.11-3.14, wheel smoke,
  pinned formatting, the fixed PR corpus, and both Linux link-safety tests.
- The retained
  [`phase3-pr-differential` artifact](https://github.com/Tow57/AtlasLOB/actions/runs/30141147909/artifacts/8614502951)
  records the hosted 10-by-5,000 exact campaign; its workflow retention period is 14 days.

The retained [PR smoke summary](pr-smoke.json) records every case seed, generated stream digest,
reference/native evidence digest, command mix, validity mix, and exercised intent-family count.
Automated evidence tests enforce both checked JSON schemas and frozen hashes, then regenerate all
50,000 PR commands plus the checked 1,000,000-command nightly workload from their V1 policies to
revalidate command digests and generation statistics.

These results establish the implemented generator, campaign, runner, failure, shrinker,
metamorphic, bounded-fuzz, long-case, and release-gate paths.

## Release gate result

The published Phase 3 PR implementation head passed all required hosted checks, including both
Linux link-safety tests and the fixed 10-by-5,000 corpus. The Phase 3 release gate is satisfied.
The manual 10-by-10,000,000 Release campaign remains an on-demand policy tier and is not claimed as
executed here. Phase 4 has not started.

## Reproduction

Build the test-only native adapter, install the development tools, and run the local selections:

```sh
cmake --preset dev-gcc
cmake --build --preset dev-gcc --target atlas_diff_native
python -m pip install -e ".[dev]"
python -m pytest -m "not campaign and not differential_fuzz"
python -m pytest -m "campaign or differential_fuzz"
python -m atlaslob.cli predefined \
  --tier pr \
  --native build/dev-gcc/atlas_diff_native \
  --output build/phase3-pr
```

Run one explicit rotating nightly campaign only when the machine and time budget are suitable:

```sh
python -m atlaslob.cli predefined \
  --tier nightly \
  --epoch 0 \
  --native build/dev-gcc/atlas_diff_native \
  --output build/phase3-nightly-epoch-0
```

The `campaign.json`, per-case summaries, command/evidence digests, and any failure bundles under the
chosen output directory are the reviewable evidence. Do not use elapsed time from these commands as
a performance result.
