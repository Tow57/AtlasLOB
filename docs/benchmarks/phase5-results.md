# Phase 5 benchmark results

## Status and host

This is the official, finalized Phase 5 campaign for commit
`9c28c7e8a74c33ce0bcd61930d0fea72e120f26e`. All 510 observations are valid:
51 measurement shapes with 10 valid observations per shape and no invalid observations.

The qualified host was `ryzen-9800x3d-64g-ubuntu2404-a`: an AMD Ryzen 7 9800X3D system
running Ubuntu 24.04.4 LTS and kernel 7.0.0-28-generic, with local NVMe ext4 storage.
Measurements were pinned to logical CPU 1 and its SMT sibling was idle. These results describe
that host and campaign; they are not hardware-independent performance guarantees. Host sources:
`out/phase5-baseline/bundle/environments/core.json` fields `$.host_class`, `$.os`, `$.kernel`,
`$.storage_class`, `$.pinned_cpu`, and `$.smt_sibling_idle`.

## Methodology

- The campaign ran the `study`, `memory`, `replay`, `python`, and `headline` tiers in order, one
  observation at a time. Attempts were retained and resumable.
- Workloads were generated deterministically. Manifests, streams, binaries, environments, and
  observations were bound by SHA-256 identities.
- Core timing, allocation measurement, and CPython measurement used separate official environment
  manifests. Each has `$.classification = "official"`.
- Every shape contributed 10 valid observations. Rates below are medians across those observations;
  latency values are finalized aggregate quantiles. Counts are exact counts, not rates.
- Five deterministic tier checkpoints were created and independently verified. Finalization rebuilt
  three aggregate reports from retained evidence, and a separate bundle-verification pass checked
  their source closure, hashes, and canonical rendering.
- The core, allocation, and Python reports have `$.classification = "official"` and empty
  `$.limitations` arrays. They contain 320, 70, and 120 source observations respectively.

Methodology sources: `report-core.json`, `report-allocation.json`, and `report-python.json` fields
`$.classification`, `$.limitations`, `$.source_observations`, and
`$.groups[*].{valid_observations,invalid_observations}`. The detailed measurement contract is in
[Performance methodology](../performance-methodology.md).

## Key results

All rates in this table are medians of 10 valid observations for the named shape.

| Measurement | Finalized result | Unit and statistic | Authoritative report field |
| --- | ---: | --- | --- |
| W04 C++ matching, 65,536 active orders | 2,583,618.74 | commands/s, median | `report-core.json $.groups[15].median_commands_per_second` |
| W04 C++ matching, 65,536 active orders | 9,430,208.40 | events/s, median | `report-core.json $.groups[15].median_events_per_second` |
| W04 latency p50 / p99 / p99.9 | 310 / 511 / 802 | ns, finalized aggregate quantiles | `report-core.json $.groups[2].latency_quantiles_ns` |
| W04 latency samples | 312,500 | samples, count | `report-core.json $.groups[2].latency_sample_count` |
| Verified ten-million-record replay | 334,744.21 | records/s, median | `report-core.json $.groups[30].median_commands_per_second` |
| Verified ten-million-record replay | 1,171,604.75 | events/s, median | `report-core.json $.groups[30].median_events_per_second` |
| Python summary, batch 1,024 | 932,256.78 | commands/s, median | `report-python.json $.groups[9].median_commands_per_second` |
| Python summary, batch 1,024 | 3,402,737.26 | events/s, median | `report-python.json $.groups[9].median_events_per_second` |
| One-million-order preload | 1,456,462.28 | commands/s, median | `report-core.json $.groups[5].median_commands_per_second` |
| One-million-order preload | 2,958,441.92 | events/s, median | `report-core.json $.groups[5].median_events_per_second` |

The verified replay used the warm-cache policy and exactly 10,000,000 input records; sources are
`report-core.json $.groups[30].commands` and
`$.groups[30].measurement_parameters.cache_policy`. The preload shape contains exactly 1,000,000
commands and targets 1,000,000 active orders; sources are `report-core.json $.groups[5].commands`
and `$.groups[5].measurement_parameters.measured_start_active_order_count`.

### Allocation and memory

For W04's one-million-command core measured region, the median allocation result was 8.35
allocations/command, 8,350,000 allocations, 1,183,600,000 allocated bytes, 12,058,624 live bytes,
12,061,216 peak live bytes, and a 34,002,944-byte process RSS delta. Each value is a median across
10 valid observations; the tracked allocation metrics had zero IQR for this shape. Sources:
`report-allocation.json $.groups[1].commands` and
`$.groups[1].{allocations_per_command,allocation_count,allocated_bytes,live_bytes,
peak_live_bytes,process_rss_delta_bytes}.median`.

The report visualizations preserve the finalized aggregate data:

- [Core timing and replay](report-core.svg)
- [Allocation and memory](report-allocation.svg)
- [Python batch interfaces](report-python.svg)

## Python output comparison

Each row measures 1,000,000 commands with 65,536 active orders. Command and event rates are
medians across 10 valid observations, in commands/s and events/s respectively. The table reports
the three distinct public output modes without interpreting one as universally preferable.

| Output mode | Batch size | Median commands/s | Median events/s | Source group and fields |
| --- | ---: | ---: | ---: | --- |
| Objects | 1 | 67,981.300825 | 248,131.748012 | `report-python.json $.groups[4].{median_commands_per_second,median_events_per_second}` |
| Objects | 64 | 86,650.993155 | 316,276.125014 | `report-python.json $.groups[6].{median_commands_per_second,median_events_per_second}` |
| Objects | 1,024 | 68,065.191085 | 248,437.947459 | `report-python.json $.groups[5].{median_commands_per_second,median_events_per_second}` |
| Objects | 65,536 | 51,105.574342 | 186,535.346348 | `report-python.json $.groups[7].{median_commands_per_second,median_events_per_second}` |
| Columns | 1 | 13,889.034369 | 50,694.975448 | `report-python.json $.groups[0].{median_commands_per_second,median_events_per_second}` |
| Columns | 64 | 38,183.968002 | 139,371.483208 | `report-python.json $.groups[2].{median_commands_per_second,median_events_per_second}` |
| Columns | 1,024 | 40,707.651905 | 148,582.929453 | `report-python.json $.groups[1].{median_commands_per_second,median_events_per_second}` |
| Columns | 65,536 | 40,242.193810 | 146,884.007408 | `report-python.json $.groups[3].{median_commands_per_second,median_events_per_second}` |
| Summary | 1 | 229,253.461573 | 836,775.134742 | `report-python.json $.groups[8].{median_commands_per_second,median_events_per_second}` |
| Summary | 64 | 887,917.937373 | 3,240,900.471410 | `report-python.json $.groups[10].{median_commands_per_second,median_events_per_second}` |
| Summary | 1,024 | 932,256.783555 | 3,402,737.259977 | `report-python.json $.groups[9].{median_commands_per_second,median_events_per_second}` |
| Summary | 65,536 | 895,341.596608 | 3,267,996.827619 | `report-python.json $.groups[11].{median_commands_per_second,median_events_per_second}` |

For every row, the batch size and output mode are also recorded in the same group's
`measurement_parameters`; `commands`, `valid_observations`, and `invalid_observations` establish
the common command and sample counts.

## Reproducibility and evidence

- Frozen source commit: `9c28c7e8a74c33ce0bcd61930d0fea72e120f26e`.
- Measurement wheel: `out/phase5-python-wheel/atlaslob-0.2.0-cp312-cp312-linux_x86_64.whl`;
  SHA-256 `d50ccd8f59540c920d2cfa547d48cd7c250e9aaac6b3ff6c186be534e36b1209`.
- Workload inventory: `out/phase5-baseline/workloads.sha256`.
- Official environment inventory: `out/phase5-baseline/official-environments.sha256`.
- Final reports: `out/phase5-baseline/bundle/reports/report-{core,allocation,python}.{json,md,svg}`.
- Verified local archive: `out/phase5-baseline-bundle.tar`; SHA-256
  `50d9abc77ef3bcc433232815542a1ef681c5605de3ad19c76d2a76e483845973`.

The raw observations, large ATLAS workload streams, replay logs, profiling data, and local archive
are deliberately not committed to Git. The lightweight finalized SVG renderings above are exact
copies of the generated reports; the JSON reports remain the authority for numeric claims.

## Profiling limitation

Hardware performance counters were unavailable because the host's
`kernel.perf_event_paranoid` value was 4 and `/usr/bin/perf` had no elevated capability. The
security policy was not weakened. Consequently, this project makes no hardware-counter or
call-graph claim from those captures. This separate profiling limitation does not invalidate the
finalized timing, allocation, correctness, or replay reports, which passed independent bundle
verification.
