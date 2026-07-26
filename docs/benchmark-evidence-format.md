# Benchmark evidence format

This document is the field-level contract for AtlasLOB Phase 5 benchmark evidence. It describes
the four public V1 evidence schemas, their canonical encoding, and the bindings that make a report
reproducible from its source files. It is a format specification, not a benchmark result.

The implementation is independent of `atlaslob._native_engine`. Evidence can therefore be
verified and analyzed on a machine that cannot import the native extension.

## Canonical document rules

Each V1 evidence file is exactly one canonical ASCII JSON object:

- the document ends with exactly one LF byte and contains no CR byte or additional newline;
- object keys are lexicographically sorted and JSON is compact, with no insignificant whitespace;
- non-ASCII string characters use JSON escapes;
- duplicate, missing, and unknown keys are rejected at every defined object level;
- JSON numbers, `NaN`, and infinities are forbidden;
- integer counts, byte counts, and nanoseconds are JSON strings containing canonical unsigned
  decimal: `0` or a nonzero digit followed by digits;
- signed derived values use an optional `-`, never `+` or `-0`;
- derived decimal fractions do not use exponents or trailing fractional zeroes;
- SHA-256 values are 64 lowercase hexadecimal characters;
- booleans are JSON booleans and absent optional values are JSON `null`; and
- schema-specific arrays and mappings obey the ordering and uniqueness rules below.

Unless a narrower range is stated, decimal integers fit an unsigned 64-bit value. Public free
text is nonempty, single-line ASCII and cannot contain an email marker, IP address, Windows path,
or `/home/<user>` or `/Users/<user>` path. Safe identifiers begin with an ASCII alphanumeric and
contain only the field's permitted ASCII characters.

Parsing is followed by a canonical re-encode check. Semantically equivalent JSON with different
spelling or layout is not accepted. Readers enforce these document-size limits before parsing:

| Schema | Maximum bytes |
| --- | ---: |
| `ATLAS_BENCH_WORKLOAD_V1` | 1 MiB |
| `ATLAS_BENCH_ENV_V1` | 1 MiB |
| `ATLAS_BENCH_OBSERVATION_V1` | 16 MiB |
| `ATLAS_BENCH_REPORT_V1` | 64 MiB |

## `ATLAS_BENCH_WORKLOAD_V1`

A workload manifest binds one resolved benchmark recipe to one canonical `ATLAS_DIFF_V2` command
stream and to independently computed expected evidence.

| Field | Type | Meaning and constraints |
| --- | --- | --- |
| `schema` | string | Literal `ATLAS_BENCH_WORKLOAD_V1`. |
| `workload_id` | string | One of W01-W10 or W12. W11 is reserved for Phase 6. |
| `generator_version` | decimal string | Benchmark generator version; verification currently requires `1`. |
| `seed` | decimal string | Explicit deterministic unsigned 64-bit seed. |
| `catalog` | array | One to 4,096 instrument configurations, sorted by unique ascending `instrument_id`. |
| `max_total_active_orders` | decimal string | Multi-engine active-order capacity. |
| `operation_distribution` | object | Sorted unique measured-region operation names and counts; at most 64 entries and counts sum to `measured_commands`. |
| `parameters` | object | Exact sorted resolved-parameter map described below; at most 64 entries. |
| `preload_commands` | decimal string | Commands before warm-up and outside the ordinary measured region. |
| `warmup_commands` | decimal string | Commands after preload and outside the ordinary measured region. |
| `measured_commands` | decimal string | Nonzero commands in the ordinary measured region. |
| `after_preload_active_order_count` | decimal string | Independently reproduced active count immediately after preload. |
| `measured_start_active_order_count` | decimal string | Independently reproduced active count after preload and warm-up. |
| `final_active_order_count` | decimal string | Independently reproduced active count after all regions. |
| `stream_file` | string | ASCII basename of the adjacent `.atlas` file; directories are forbidden. |
| `stream_sha256` | SHA-256 | Digest of the exact `.atlas` bytes. |
| `expected_events` | decimal string | Normalized events in the measured region. |
| `expected_committed` | decimal string | Committed measured commands. |
| `expected_rejected` | decimal string | Sequenced measured rejections. |
| `expected_engine_errors` | decimal string | Terminal measured engine errors. |
| `expected_event_digest` | SHA-256 | SHA-256 over ordered measured-command event digests. |
| `expected_final_digest` | SHA-256 | Final `ATLSME01` state digest after all regions. |
| `expected_empty_state_digest` | SHA-256 | State digest immediately after construction, before any command. |
| `expected_preload_events` | decimal string | Normalized events in the preload region. |
| `expected_preload_committed` | decimal string | Committed preload commands. |
| `expected_preload_rejected` | decimal string | Sequenced preload rejections. |
| `expected_preload_engine_errors` | decimal string | Terminal preload engine errors. |
| `expected_preload_event_digest` | SHA-256 | SHA-256 over ordered preload-command event digests. |
| `expected_preload_state_digest` | SHA-256 | State digest immediately after preload. |
| `timed_input_file` | string or null | Adjacent replay-log basename for W10; null otherwise. |
| `timed_input_kind` | string | `atlslg01` for W10 and `none` otherwise. |
| `timed_input_sha256` | SHA-256 or null | Exact timed replay-input digest for W10; null otherwise. |
| `timed_input_records` | decimal string | W10 replay record count; zero otherwise. |
| `cache_policy` | string | `warm_page_cache` for W10 and `none` otherwise. |

Every `catalog` entry contains exactly:

| Field | Type | Meaning and constraints |
| --- | --- | --- |
| `instrument_id` | decimal string | Nonzero unsigned 32-bit instrument ID. |
| `max_order_quantity` | decimal string | Nonzero unsigned 64-bit per-order quantity limit. |
| `tick_increment` | signed decimal string | Positive signed 64-bit tick increment. |
| `max_active_orders` | decimal string | Per-instrument active-order capacity. |

The measured outcome counts sum to `measured_commands`; preload outcome counts sum to
`preload_commands`. The three active-count fields cannot exceed the global capacity. The total of
preload, warm-up, and measured commands cannot exceed 100,000,000.

W10 is the only replay workload. It has zero preload and warm-up commands, a `.atlslg` timed input,
`timed_input_kind=atlslg01`, a nonzero record count equal to `measured_commands`, a matching
SHA-256, and `cache_policy=warm_page_cache`. Every non-W10 workload uses null/`none`/zero timed
input fields.

### Exact resolved parameter map

The `parameters` object is reproducible evidence, not an extensible bag of CLI arguments.
Materialization emits and verification reproduces exactly these keys:

| Parameter | Meaning |
| --- | --- |
| `active_order_target` | Requested total active-state shape. |
| `actual_instrument_command_counts` | Comma-separated commands actually emitted for each catalog instrument. |
| `after_preload_active_order_count` | Active count reproduced at the preload boundary. |
| `aggressive_basis_points` | Resolved underlying generator aggressiveness. |
| `boundary_quantity_basis_points` | Resolved underlying boundary-quantity setting. |
| `client_count` | Resolved client population. |
| `composition` | Literal `benchmark_specific` in V1. |
| `cycle_commands` | Recipe cycle length: workload-specific, including sweep depth plus one for W05. |
| `hot_level_count` | Eight for W02 and zero otherwise. |
| `instrument_count` | Catalog size. |
| `invalid_basis_points` | Invalid-command setting; benchmark streams resolve this to zero. |
| `latency_first_sample_index` | Zero-based first latency sample; V1 uses `31`. |
| `latency_sample_stride` | Latency sample stride; V1 uses `32`. |
| `market_basis_points` | Resolved underlying market-order setting. |
| `measured_commands` | Repeated measured-region size. |
| `measured_start_active_order_count` | Active count reproduced after warm-up. |
| `measured_sweep_count` | Complete W05 measured sweeps, or zero. |
| `mid_price` | Resolved generator midpoint. |
| `operation_weights` | Comma-separated New, Cancel, Replace weights. |
| `preload_commands` | Repeated preload size. |
| `price_model` | Resolved named price model. |
| `price_span_ticks` | Resolved tick span. |
| `profile` | Resolved Generator V2 profile. |
| `stream_command_budgets` | Comma-separated resolved per-instrument command budgets. |
| `sweep_depth` | W05 sweep depth, or zero. |
| `underlying_multi_generator_version` | Frozen multi-instrument generator version used by the recipe. |
| `w09_active_order_counts` | Comma-separated per-instrument W09 active targets, or zero. |
| `w09_primary_activity_basis_points` | W09 primary-instrument activity share, or zero. |
| `warmup_commands` | Repeated warm-up size. |

Verification regenerates the command sequence, compares every canonical stream line, executes
every command through a fresh independent `ReferenceRouter`, recomputes region boundaries,
per-instrument command counts, operation counts, outcomes, event digests, active counts, and state
digests, and hashes the exact stream bytes.

### Stream layout

The adjacent `.atlas` file uses the strict LF-delimited `ATLAS_DIFF_V2` grammar from
[Differential testing interface](differential-testing.md#phase-4-multi-instrument-evidence-v2).
It contains one header, the declared sorted instrument records, and exactly
`preload_commands + warmup_commands + measured_commands` command records. Each line is at most
1,024 bytes. Generation, stream parsing, and verification occur outside every timed interval.

## `ATLAS_BENCH_ENV_V1`

An environment manifest describes one exact executable and its sanitized execution context.

| Field family | Fields | Meaning and constraints |
| --- | --- | --- |
| Schema and source | `schema`, `commit`, `tag`, `dirty` | Literal schema, source commit, optional exact tag, and checkout state. |
| Executable | `binary_sha256` | Exact runner digest; for a CPython environment this is the loaded `_native_engine` extension. |
| Platform | `os`, `kernel`, `host_class`, `architecture` | Sanitized platform identity. `host_class` is a lowercase reusable class, never a hostname. |
| CPU | `cpu_model`, `physical_cores`, `logical_cpus`, `microcode` | Sanitized CPU and topology facts; core counts are nonzero and physical does not exceed logical. |
| Memory | `memory_bytes` | Nonzero physical-memory size. |
| Compiler receipt | `compiler`, `compiler_flags`, `build_receipt_sha256`, `build_target_profiles` | Compiler identity, ordered unique effective flags, digest of the canonical target-closure receipt, and sorted normalized profiles for every timed target dependency. |
| Build facts | `build_type`, `optimization`, `ndebug`, `frame_pointers`, `invariants`, `sanitizers`, `lto`, `benchmark_build`, `warnings_as_errors`, `debug_symbols`, `cxx20`, `release_flags_locked` | Facts derived from `CMakeCache.txt` and `compile_commands.json`, not accepted as caller assertions. |
| CPU placement | `affinity`, `pinned_cpu`, `smt_sibling_idle` | Sorted CPU set; an official run has a singleton nonzero pinned CPU and a qualified idle sibling. |
| NUMA | `numa_nodes`, `numa_cpu_policy`, `numa_memory_policy` | Node count and observed process policies. |
| Storage | `filesystem`, `storage_class` | Filesystem type and sanitized device/storage class. |
| Host controls | `governor`, `turbo`, `smt`, `virtualization`, `perf_version` | Observed states. Virtualization is `none`, `detected`, or `unknown`; `perf_version` may be null only for exploratory evidence. |
| Runtime | `runtime_kind` | `native` or `cpython`. |
| CPython identity | `python_implementation`, `python_version`, `python_cache_tag`, `atlaslob_version`, `interpreter_sha256`, `wheel_sha256`, `package_sha256`, `wrapper_sha256`, `harness_sha256` | All null for `native`; all present for `cpython`. The digests bind the interpreter, wheel, installed package tree, public wrapper, and standalone worker. |
| Qualification | `classification`, `host_context_sha256`, `limitations` | `official` or `exploratory`, the canonical comparison-context digest, and unique sorted public limitations. |

`build_target_profiles` records every normalized compile profile in the executable's required
target closure. Source and build roots are replaced by `<SOURCE>` and `<BUILD>`. The receipt proves
the effective C++20/O3/NDEBUG/frame-pointer/debug-symbol/warnings/invariant/sanitizer/LTO state,
including the exact frozen Release flags and absence of extra code-generation flags.

The host-context digest includes platform, CPU, memory, compiler receipt and build facts, CPU
placement, NUMA, storage, host controls, `runtime_kind`, and the CPython implementation, version,
cache tag, and interpreter digest. It intentionally excludes commit, tag, dirty state, executable
digest, AtlasLOB/wheel/package/wrapper/harness digests, classification, and limitations. Distinct
baseline and candidate binaries can therefore share a comparison context only when their
host/build configuration is otherwise identical.

Official classification is fail-closed. It requires:

- a clean bare-metal Ubuntu 24.04 x86-64 checkout;
- every frozen CPU, memory, NUMA, filesystem, storage, governor, turbo, SMT, microcode, and
  `perf` fact;
- affinity to exactly one recorded nonzero CPU and an idle SMT sibling;
- `Release`, `O3`, `NDEBUG`, frame pointers, debug symbols, warnings-as-errors, and C++20;
- invariants, sanitizers, and LTO disabled; and
- `benchmark_build=true` for native runners, or complete installed-wheel identity for CPython.

The collector observes but never mutates governor, turbo, SMT, NUMA, affinity, or kernel security
policy. Omitting the official request always produces exploratory evidence. Requesting official
classification on an unqualified host fails instead of silently downgrading.

## `ATLAS_BENCH_OBSERVATION_V1`

One worker process emits one observation. The orchestrator writes the same schema when a process
times out, is interrupted, exits inconsistently, or cannot produce trustworthy output, so the
attempt remains visible.

| Field | Type | Meaning and constraints |
| --- | --- | --- |
| `schema` | string | Literal `ATLAS_BENCH_OBSERVATION_V1`. |
| `boundary` | string | One of the eleven closed V1 boundary names below. |
| `timed_input_kind` | string | `atlslg01` for replay and `none` otherwise. |
| `timed_input_sha256` | SHA-256 or null | Exact replay log digest for replay; null otherwise. |
| `measurement_parameters` | object | Exact grouping parameters described below. |
| `workload_id` | string | Bound workload identifier. |
| `workload_sha256` | SHA-256 | Exact `.atlas` stream digest. |
| `workload_manifest_sha256` | SHA-256 | Exact canonical workload-manifest digest. |
| `binary_sha256` | SHA-256 | Executing native runner or extension digest. |
| `environment_sha256` | SHA-256 | Exact canonical environment-manifest digest. |
| `host_context_sha256` | SHA-256 | Host/build context copied from that environment. |
| `suite_label` | string | Shared safe suite identifier, at most 32 characters. |
| `run_label` | string | Unique safe observation identifier, at most 128 characters. |
| `variant` | string | `standalone`, `baseline`, or `candidate`. |
| `block_index` | decimal string | Zero for standalone; nonzero comparison-block number otherwise. |
| `block_position` | decimal string | Zero for standalone; one through four for A-B-B-A otherwise. |
| `preload_commands` | decimal string | Bound manifest preload count. |
| `warmup_commands` | decimal string | Bound manifest warm-up count. |
| `commands` | decimal string | Commands validated for this boundary. |
| `events` | decimal string | Corresponding normalized events. |
| `committed` | decimal string | Corresponding committed commands. |
| `rejected` | decimal string | Corresponding sequenced rejections. |
| `engine_errors` | decimal string | Corresponding terminal engine errors. |
| `elapsed_ns` | decimal string | Boundary duration; zero for allocation-only boundaries. |
| `rss_before_bytes` | decimal string | Process RSS at the boundary-specific capture point. |
| `rss_after_bytes` | decimal string | Process RSS at the boundary-specific end point. |
| `peak_rss_bytes` | decimal string | Process high-water RSS, not a resettable interval-only maximum. |
| `latency_ns` | array of decimal strings or null | Raw deterministic samples only for `core_latency`. |
| `allocations` | object or null | Allocation payload only for `core_allocation` and `core_setup_allocation`. |
| `event_digest` | SHA-256 | Recomputed event digest for the applicable command region. |
| `final_digest` | SHA-256 | Recomputed `ATLSME01` state digest for the applicable boundary. |
| `valid` | boolean | True only after all runner and cross-file checks succeed. |
| `failure_reason` | public text or null | Null exactly when `valid=true`. |

The `allocations` object contains exactly `allocation_count`, `deallocation_count`,
`allocated_bytes`, `live_bytes`, and `peak_live_bytes`, each a decimal string. It covers the
complete replaceable global `new`/`delete` surface, including array, sized, aligned, and nothrow
forms. `live_bytes` cannot exceed `peak_live_bytes`.

Every observation's common `measurement_parameters` are exactly:

```text
instrument_count
measured_start_active_order_count
sweep_depth
```

Python boundaries additionally require `batch_size` and `output_mode`; W04 is mandatory and batch
size is exactly one of 1, 64, 1,024, or 65,536. Replay boundaries additionally require
`cache_policy`, `record_count`, `replay_mode`, and `timed_input_sha256`; W10 and
`warm_page_cache` are mandatory. Core boundaries permit no additional measurement keys.

### Boundary vocabulary and intervals

| Boundary | Exact interpretation |
| --- | --- |
| `core_throughput` | Times only measured-region public C++ `execute` calls and destruction of each owned `EngineResult`/event batch. Engine construction, preload, warm-up, validation replay, event inspection, hashing, and output are outside timing. The RSS envelope begins before engine construction and ends after measured execution. |
| `core_latency` | Same C++ service boundary, measured separately with `steady_clock`. Every 32nd command starting at index 31 is timed into a preallocated buffer capped at 200,000; throughput rates are not derived from this run. RSS uses the same broader envelope as core throughput. |
| `core_allocation` | Allocation hooks surround only measured-region public execution and result destruction after construction, preload, and warm-up. It is untimed. RSS still spans from before construction through measured execution. |
| `core_construction` | Times eager `MultiInstrumentEngine` construction only. It records zero commands/events, the empty event digest, and `expected_empty_state_digest`. Prefix reproducibility checks occur after timing. |
| `core_preload` | Constructs the engine first, then times preload commands and result destruction. RSS begins after construction and before preload. It records the manifest's preload counts/digests/state. |
| `core_setup_allocation` | Allocation hooks and RSS begin before engine construction and cover construction plus preload. It is untimed and records preload counts/digests/state. |
| `replay_fast` | Releases the parsed `.atlas` workload, scans and validates the W10 log before timing to warm the page cache, then times public fast replay plus destruction of the recovered result. Post-timing source-stability and full-verify checks are excluded. |
| `replay_verify` | Same warmed-log envelope, using public verify replay. It remains distinct from fast replay. |
| `python_objects` | Commands and measured batch slices already exist; timing includes per-call Python-to-native conversion, native batch execution, owned object-result materialization, and result destruction. Prefix submission, final digest, and independent validation are excluded. |
| `python_columns` | Same Python boundary with owned column materialization. |
| `python_summary` | Same Python boundary with summary materialization and without per-event objects. |

W11/gateway timing is not a V1 boundary and remains deferred to Phase 6.

For a valid observation, outcomes sum to `commands`. Construction records zero command evidence;
preload and setup-allocation records equal the manifest's preload evidence; every other boundary
equals measured-region evidence. Latency sample count is
`min(floor(measured_commands / 32), 200000)`. Invalid observations retain their scheduled identity
and failure reason but need not contain completed result evidence.

## `ATLAS_BENCH_REPORT_V1`

A report is a deterministic derivation from retained observations, workload manifests, environment
manifests, and an optional experiment decision plan. Rates exist only here, never in raw evidence.

### Report fields

| Field | Type | Meaning and constraints |
| --- | --- | --- |
| `schema` | string | Literal `ATLAS_BENCH_REPORT_V1`. |
| `classification` | string | `official` only when every referenced environment is official and every group has at least ten valid observations. |
| `suite_label` | string | The one shared suite label, at most 32 characters. |
| `host_context_sha256` | SHA-256 | The one shared host/build context. |
| `workload_manifest_sha256s` | sorted array of SHA-256 | Nonempty exact workload-manifest digests. |
| `environment_sha256s` | sorted array of SHA-256 | Nonempty exact environment-manifest digests. |
| `source_observations` | sorted array of SHA-256 | Nonempty exact observation-document digests. |
| `groups` | array | Unique statistics groups in canonical identity order. |
| `comparisons` | array | Unique A/B comparisons sorted by `comparison_id`. |
| `experiments` | array | Unique `EXP-###` decisions sorted by experiment ID. |
| `limitations` | sorted array of public text | Unique inherited and derived limitations. |

Invalid attempts are excluded from statistics but retained in group counts and limitations. Their
presence does not by itself force an otherwise qualified report to exploratory. A group with no
valid attempt remains visible as an invalid-only group with zero elapsed/count evidence and no
rate, memory, latency, or allocation distributions.

### Group fields

A group identity is:

```text
(boundary, timed_input_kind, timed_input_sha256, measurement_parameters,
 workload_id, workload_sha256, workload_manifest_sha256, binary_sha256)
```

Every group repeats those eight identity fields and contains:

| Field family | Fields | Meaning |
| --- | --- | --- |
| Observation counts | `valid_observations`, `invalid_observations` | Retained counts. |
| Shape | `commands`, `resting_order_denominator` | Common command count and manifest-bound resting-order denominator. |
| Elapsed statistics | `minimum_elapsed_ns`, `maximum_elapsed_ns`, `median_elapsed_ns`, `mad_elapsed_ns`, `iqr_elapsed_ns` | Exact elapsed distribution over valid attempts. |
| Command-rate statistics | `minimum_commands_per_second`, `maximum_commands_per_second`, `median_commands_per_second`, `mad_commands_per_second`, `iqr_commands_per_second` | Complete family for throughput-like boundaries, null otherwise. |
| Event-rate statistics | `minimum_events_per_second`, `maximum_events_per_second`, `median_events_per_second`, `mad_events_per_second`, `iqr_events_per_second` | Complete family with the same presence rule. |
| Latency | `latency_sample_count`, `minimum_latency_ns`, `maximum_latency_ns`, `latency_quantiles_ns` | Empty outside latency; otherwise extrema and exactly p50, p90, p95, p99, and p99.9 using nearest rank. |
| Process memory | `peak_rss_bytes`, `process_rss_delta_bytes`, `process_rss_delta_bytes_per_command`, `process_rss_delta_bytes_per_resting_order` | Scalar distributions over valid attempts; the last two are null when their denominators are zero. |
| Allocation | `allocation_count`, `deallocation_count`, `allocated_bytes`, `live_bytes`, `peak_live_bytes`, `allocations_per_command` | Complete scalar family for the two allocation boundaries and null otherwise. |

The resting-order denominator is zero for construction,
`after_preload_active_order_count` for preload/setup-allocation, and
`measured_start_active_order_count` for all other boundaries. Rate families are absent for
latency, allocation, construction, and setup-allocation. They are present for core throughput,
core preload, replay, and Python batch boundaries.

Each scalar statistics object has canonical decimal-string `minimum`, `maximum`, `median`, `mad`,
and `iqr` fields. RSS deltas may be signed; peak/allocation values and dispersion are nonnegative.

### Comparison fields

Each comparison contains exactly:

| Field | Meaning |
| --- | --- |
| `comparison_id` | SHA-256 of the canonical comparison identity. |
| `boundary`, `timed_input_kind`, `timed_input_sha256`, `measurement_parameters` | Exact measurement point. |
| `workload_id`, `workload_sha256`, `workload_manifest_sha256` | Exact workload identity. |
| `host_context_sha256` | Shared A/B context. |
| `baseline_binary_sha256`, `candidate_binary_sha256` | Distinct executable identities. |
| `target_metric`, `direction` | Boundary-fixed metric and `higher_is_better` or `lower_is_better`. |
| `classification` | `official` or `exploratory`. |
| `target_median_change_percent` | Direction-normalized candidate improvement. |
| `baseline_relative_mad_percent`, `candidate_relative_mad_percent` | Relative median absolute deviations. |
| `peak_rss_change_percent` | Raw candidate peak-RSS change, or null if the baseline is zero. |
| `abba_blocks` | Complete all-valid A-B-B-A blocks used. |

The comparison identity digest covers boundary, measurement parameters, workload ID and both
workload digests, host context, and both binary digests. Its target metric is fixed by boundary:
commands/s for throughput/preload/replay/Python, p99 nanoseconds for latency, allocation count for
allocation/setup-allocation, and elapsed nanoseconds for construction.

Blocks must have positions 1-4 in exact A-B-B-A binary order. Incomplete blocks and blocks
containing an invalid observation remain raw evidence but are excluded from comparison statistics.
An official comparison requires at least five complete valid blocks and official environment
qualification for both binaries.

### Experiment plan and experiment fields

Analysis accepts an optional canonical, LF-terminated, at-most-1-MiB
`ATLAS_BENCH_EXPERIMENT_PLAN_V1` document with `schema` and a sorted `experiments` array. Each plan
entry has exactly:

```text
experiment_id
policy
target_comparison_id
control_comparison_ids
correctness_gate
complexity_gate
note_path
note_sha256
rationale
```

The report's derived experiment record contains:

| Field | Meaning |
| --- | --- |
| `experiment_id` | Unique `EXP-###`. |
| `policy` | `general` or `capacity_reservation`. |
| `classification` | `official`, `exploratory`, or `not_run`. |
| `target_comparison_id`, `control_comparison_ids` | Exact measured evidence, or null/empty for a deferred experiment. |
| `threshold_result` | `passed`, `failed`, or `not_run`. |
| `decision` | `accepted`, `rejected`, `neutral`, or `deferred`. |
| `target_median_change_percent` | Target comparison change, or null if deferred. |
| `noise_gate_percent` | Twice the larger target-role relative MAD, or null if deferred. |
| `worst_control_change_percent` | Minimum control improvement, or null if deferred. |
| `worst_peak_rss_change_percent` | Maximum target/control peak-RSS increase, or null if deferred. |
| `correctness_gate`, `complexity_gate` | `passed`, `failed`, or `not_run`. |
| `note_path`, `note_sha256` | Safe bundle-relative experiment note and exact digest. |
| `rationale` | Public deterministic explanation. |

Measured experiment targets and controls must all be core-throughput/commands-per-second
comparisons with the same binary pair and host context. The numeric fields are recomputed from
those comparisons. The threshold requires at least 5% target improvement, improvement greater
than twice the larger relative MAD, no control below -5%, and the applicable RSS gate. Acceptance
additionally requires official comparisons plus passed correctness and complexity gates.
Otherwise the derived decision is neutral or rejected according to the frozen gate. A null target,
no controls, null metrics, and `not_run` gates derive a deferred experiment.

## Invalid-run and qualification policy

- Every scheduled process attempt is retained once; there is no silent retry or deletion.
- Timeouts, interruptions, malformed output, status/output contradictions, and runner failures
  become canonical invalid observations when possible.
- Standalone suites default to ten independent processes. A/B suites default to five A-B-B-A
  blocks, producing ten observations per role.
- One process observation has a configurable one-to-900-second cap; official runs use the
  900-second maximum from the Phase 5 policy.
- Invalid observations never enter statistics. They remain in group counts and limitations.
- Official reports require official environments and at least ten valid observations in every
  group. Official comparisons independently require five complete valid A-B-B-A blocks.
- Mixed host contexts, suite labels, runtime kinds, workload identities, binaries, region
  bindings, or digests are rejected rather than normalized away.

## Cross-file bindings

The evidence chain is deliberately redundant:

```text
workload manifest --stream_file + stream_sha256------> exact ATLAS_DIFF_V2 stream
workload manifest --timed_input_*--------------------> exact W10 ATLSLG01 log
environment manifest --binary_sha256-----------------> exact runner/extension
observation --workload_manifest_sha256/workload_sha256> exact manifest and stream
observation --environment_sha256---------------------> exact environment JSON
observation --host_context_sha256/binary_sha256------> matching environment fields
report --workload_manifest_sha256s-------------------> exact workload JSON files
report --environment_sha256s-------------------------> exact environment JSON files
report --source_observations-------------------------> exact observation JSON files
report experiment --note_path + note_sha256----------> exact experiment note
bundle inventory --path + sha256---------------------> every retained bundle file
```

The suite verifies the executable and current host context against its environment before launch;
the native executable hashes its loaded image and parsed stream. Workload verification uses the
independent router. Report verification recomputes the report and its Markdown/SVG renderings
byte-for-byte from named raw sources.

## Evidence bundle closure

`ATLAS_BENCH_BUNDLE_V1` is an inventory envelope rather than a fifth measurement schema:

```json
{"files":[{"path":"relative/portable/path","sha256":"..."}],"schema":"ATLAS_BENCH_BUNDLE_V1"}
```

Inventory records use unique sorted safe POSIX-relative paths. The inventory covers every regular
file except itself. Absolute paths, backslashes, drive prefixes, empty/`.`/`..` components,
symbolic links, junctions, and other reparse points anywhere in the tree are rejected.

A verified bundle has exact closure:

- at least one workload, environment, observation, and report document;
- no duplicate canonical evidence document;
- every workload and environment is referenced by an observation, and every observation is
  covered by exactly one report;
- every `.atlas` file is exactly a referenced workload stream, with no missing or orphan stream;
- every `.atlslg` file is exactly a referenced W10 timed input, with no missing or orphan log;
- each replay log has a bounded valid `ATLSLG01` header/record sequence, matches the manifest
  catalog/configuration, contains the exact `.atlas` commands in sequence, and reproduces record,
  outcome, event-count, and event-digest evidence;
- every report JSON regenerates exactly from its sources and has exactly one byte-identical
  same-stem Markdown and SVG rendering;
- every Markdown file is either such a report rendering or one digest-bound experiment note,
  with no collision or orphan; every SVG is a report rendering;
- every CPython environment's `harness_sha256` matches an inventoried file; and
- all additional retained assets, such as text profiles, are still covered by the inventory.

## Failure and exit behavior

Native runners use:

| Exit | Meaning |
| ---: | --- |
| 0 | One canonical observation was emitted with `valid=true`. |
| 1 | One canonical observation was emitted with `valid=false` for invalid data or divergence. |
| 2 | CLI usage error; no observation is promised. |
| 3 | Operational, resource, or output failure; an invalid observation is emitted when possible. |

The Python performance CLI returns zero on success, one for caught evidence/validation/I/O
failure, and argparse's two for usage errors. Shared CI exercises schemas, hashes, dry runs, and
tiny exploratory scenarios without a wall-clock threshold.
