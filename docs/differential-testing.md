# Differential testing interface

Phase 3 compares the public C++ matching engine with an independent Python implementation. The
native side of that comparison is the test-only `atlas_diff_native` executable. It is available
only when `BUILD_TESTING=ON`, is not installed, and is not a production protocol.

The design boundary and independence rules are recorded in
[ADR 0010](decisions/0010-independent-python-oracle-boundary.md).

## Input: `ATLAS_DIFF_V1`

Input is case-sensitive, whitespace-delimited text. Blank lines and lines whose first
non-whitespace character is `#` are ignored. The first non-ignored line is:

```text
ATLAS_DIFF_V1 <instrument_u32> <max_quantity_u64> <tick_increment_i64> <max_active_u64> <snapshot_interval_u64>
```

The instrument, maximum quantity, and tick increment must be valid nonzero engine values.
`max_active_u64` may be zero and must fit the native `size_t`. A snapshot interval of zero
disables per-command snapshots; the final record always contains one.

Commands use raw numeric enum representations:

```text
N <client_u32> <order_u64> <instrument_u32> <side_u8> <type_u8> <tif_u8> <price_present_0_or_1> <price_i64> <quantity_u64>
C <client_u32> <order_u64> <instrument_u32>
R <client_u32> <old_u64> <new_u64> <instrument_u32> <price_i64> <quantity_u64>
```

An absent New price uses `price_present=0` and a required zero placeholder. Unknown enum values
inside their underlying `u8` representation are submitted to domain validation. Token, width,
conversion, header, and absent-price-placeholder failures are adapter errors and are never
submitted, so they consume no engine sequence.

## Output: `atlas_diff_v1` JSON Lines

The executable writes exactly one JSON object per line and no diagnostic text to stdout. Domain
values that are signed or unsigned 64-bit integers are decimal JSON strings. Client and
instrument IDs are also strings for a uniform lossless decoder. Semantic versions and event
indices are JSON numbers; flags are JSON booleans; absent values are JSON `null`.

The first successful record has `kind="config"` and repeats the accepted policy plus
`semantics_version` and `mode`.

Every submitted command produces one `kind="result"` record containing:

- zero-based `command_index` and physical source `line`;
- `command_type`, `outcome`, authoritative `command_sequence`, and any `engine_error`;
- the canonical `event_digest`;
- complete ordered `events` in exact mode, or `null` in compact mode;
- current public observers and `state_digest` under `state`; and
- a complete canonical `snapshot` at the configured interval, otherwise `null`.

Every normal stream ends with `kind="final"`, `commands_processed`, current observers, and a full
snapshot. The final snapshot is present even when the interval is zero.

An adapter failure produces `kind="error"` with the physical line and a closed-vocabulary error
code, then stops. A returned native engine error is represented in its command result and is
followed by a final record. A thrown engine/resource exception is a process failure rather than a
synthesized domain or engine result.

## Modes and exit codes

Run:

```sh
atlas_diff_native exact
atlas_diff_native compact
```

`--mode exact`, `--mode=exact`, and the corresponding compact forms are also accepted. Exact is
the default.

| Exit | Meaning |
| --- | --- |
| `0` | The complete stream was processed, including any ordinary sequenced rejections. |
| `2` | Input/header/record syntax failed before domain submission. |
| `3` | The engine, adapter process, resource boundary, or output stream failed. |

Compact output retains per-command classification, sequence, event digest, public observers, and
state digest. A compact mismatch must be rerun through the first divergent command in exact mode;
compact mode is not sufficient evidence for diagnosing event-payload differences by itself.

## Comparison obligations

For each submitted command, the Python differential runner compares:

- commit/reject/error classification and command sequence;
- every event header, variant, payload, and event index in exact mode;
- event digest;
- active count, emptiness, next sequence, exhaustion, and top of book;
- state digest; and
- every canonical snapshot field at checkpoints and at the end.

The Python reference engine must produce its result before invoking the native adapter and may
not import C++ headers, private helpers, bindings, or native transition logic. Matching agreement
is supplemented by hand-derived named scenarios and independently encoded ADR 0009 hash vectors.

The decoder also binds the transcript to the submitted mode, configuration, instrument, command
types, source lines, checkpoint cadence, contiguous command sequences, process exit, and terminal
state. Internally consistent but unrelated records are protocol failures. Exact event envelopes
and snapshot values are revalidated before they may be used as correctness evidence.

## Runner executable selection

With `ATLAS_DIFF_NATIVE` unset, local Python tests search the normal MinGW/Windows and
Linux `build/dev-gcc` development locations. A complete evidence run fails when no adapter has
been built; unit-only modules can still be selected and run independently.

When `ATLAS_DIFF_NATIVE` is set, its value is authoritative. It must name an existing executable;
the suite fails instead of falling back to another build or skipping parity. Hosted evidence jobs
always set this variable after building the selected adapter.
