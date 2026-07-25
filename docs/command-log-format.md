# AtlasLOB command-log format V1

This document is the byte-level reference for `ATLSLG01`. It implements the decisions in
[ADR 0013](decisions/0013-command-log-and-replay.md).

The format is an append-only deterministic command history. It is not a native object dump, an
event-payload archive, a network protocol, an authentication format, or a durability guarantee.
Phase 4 PR2 implementation and validation are in progress; this reference does not claim that the
gate has passed.

## Encoding conventions

- Byte offsets are zero-based.
- `u8`, `u16`, `u32`, and `u64` are unsigned fixed-width integers.
- `i64` is encoded as its 64-bit two's-complement bit pattern.
- Every multibyte integer is most-significant byte first.
- No field has implicit alignment or padding.
- ASCII magic and domain-separation prefixes are the exact bytes shown.
- A SHA-256 value is 32 raw bytes in the file and 64 lowercase hexadecimal characters in reports.
- A log ID is 16 opaque bytes in the file and 32 lowercase hexadecimal characters in reports.
- `SIZE_MAX` used as an in-memory unbounded-capacity sentinel encodes as `UINT64_MAX`.
- Unless a field explicitly permits zero, zero has the domain meaning established by semantic
  contract v0.6.

The current engine semantic version is 6. A reader must compare the stored semantic version with
the semantics it implements; format version 1 does not permit silent semantic reinterpretation.

Ordinary create-new sessions fill the log ID from the C++ implementation's `std::random_device`.
That source is implementation-defined and is not claimed to be operating-system entropy. The
low-level codec and explicit C++ seam accept every 16-byte pattern for deterministic goldens and
controlled tooling. The bytes are not a UUID and provide no security or guaranteed uniqueness
property.

## Complete file

```text
header
record[0]
record[1]
...
record[N-1]
EOF
```

There is no footer, trailing index, record-count field, or end marker. Clean EOF immediately after
the header or a complete record terminates the log.

## Header

For `N` catalog entries, the header is exactly `96 + 28 * N` bytes.

| Offset | Width | Field | V1 rule |
| ---: | ---: | --- | --- |
| 0 | 8 | magic | ASCII `ATLSLG01` |
| 8 | 2 | format version | `1` |
| 10 | 2 | semantic version | engine semantic version |
| 12 | 4 | byte-order marker | `0x01020304` |
| 16 | 4 | header length | `96 + 28 * N`, including CRC |
| 20 | 4 | catalog length | `28 * N`, entries only |
| 24 | 16 | log ID | opaque identifier |
| 40 | 8 | first sequence | exactly `1` |
| 48 | 8 | maximum total active orders | canonical engine-wide capacity |
| 56 | 4 | catalog count | `N`, nonzero |
| 60 | `28 * N` | catalog | sorted entries below |
| `60 + 28 * N` | 32 | configuration digest | SHA-256 of `ATLSCF01` input |
| `92 + 28 * N` | 4 | header CRC32C | checksum of all preceding header bytes |

All length calculations are checked before multiplication or addition. `catalog_length` must be
divisible by 28, must equal `28 * catalog_count`, and must produce the exact declared
`header_length`. Extra header bytes are not extensions in V1; they are invalid data.

### Catalog entry

Each entry is exactly 28 bytes.

| Relative offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | instrument ID |
| 4 | 8 | maximum order quantity |
| 12 | 8 | tick increment (`i64`) |
| 20 | 8 | maximum active orders |

Instrument IDs are nonzero, unique, and strictly increasing. Maximum order quantity and tick
increment are positive. Capacity values must fit the host representation when an engine is
constructed; the codec itself retains their canonical `u64` representation.

### Configuration digest

The exact SHA-256 input is:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 8 | ASCII `ATLSCF01` |
| 8 | 2 | semantic version |
| 10 | 8 | maximum total active orders |
| 18 | 8 | catalog count as `u64` |
| 26 | `28 * N` | sorted catalog entries in header encoding |

The digest input length is exactly `26 + 28 * N`. The `u64` count in the digest input is
intentional even though the header count is `u32`.

### Header checksum coverage

For header length `H`, CRC32C input is bytes `[0, H - 4)`. The stored checksum occupies
`[H - 4, H)`.

## Records

The first record sequence is 1. Each following record sequence is exactly one greater than the
previous record sequence. A rejected command occupies its sequence exactly like a committed
command.

### Fixed envelope

For payload length `P`, a record is exactly `66 + P` bytes.

| Relative offset | Width | Field | V1 rule |
| ---: | ---: | --- | --- |
| 0 | 4 | total length | `66 + P`, inclusive through CRC |
| 4 | 4 | payload length | exact size for command type |
| 8 | 2 | record version | `1` |
| 10 | 1 | command type | New 1, Cancel 2, Replace 3 |
| 11 | 1 | outcome | committed 1, rejected 2 |
| 12 | 8 | sequence | nonzero, contiguous |
| 20 | 2 | rejection reason | zero iff committed |
| 22 | 8 | event count | nonzero |
| 30 | 32 | event digest | raw `ATLSEV01` SHA-256 result |
| 62 | `P` | command payload | exact variant layout |
| `62 + P` | 4 | record CRC32C | checksum of all preceding record bytes |

The fixed envelope size of 66 includes the trailing CRC. CRC input is bytes `[0, 62 + P)` and the
stored checksum occupies `[62 + P, 66 + P)`.

A committed outcome requires rejection reason 0. A rejected outcome requires one currently defined
nonzero `RejectReason` value. Outcome and reason describe the prepared result whose event count and
digest are stored in the same record. Engine errors do not have records.

### New payload

Payload length is 36; total record length is 102.

| Relative offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | client ID |
| 4 | 8 | order ID |
| 12 | 4 | instrument ID |
| 16 | 1 | raw `Side` value |
| 17 | 1 | raw `OrderType` value |
| 18 | 1 | raw `TimeInForce` value |
| 19 | 1 | limit-price presence |
| 20 | 8 | limit-price slot (`i64`) |
| 28 | 8 | quantity |

Presence must be exactly 0 or 1. Presence 0 requires an all-zero price slot. Presence 1 interprets
the slot as the submitted signed price, including zero or negative values that domain validation
may reject.

Raw enum bytes are not required to name a recognized enum. Preserving an unknown byte is necessary
to replay structural domain rejections exactly.

### Cancel payload

Payload length is 16; total record length is 82.

| Relative offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | client ID |
| 4 | 8 | order ID |
| 12 | 4 | instrument ID |

### Replace payload

Payload length is 40; total record length is 106.

| Relative offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | client ID |
| 4 | 8 | old order ID |
| 12 | 8 | new order ID |
| 20 | 4 | instrument ID |
| 24 | 8 | new limit price (`i64`) |
| 32 | 8 | new quantity |

IDs, prices, and quantities retain their complete fixed-width submitted values. Domain-invalid
zero, signed-boundary, or policy-invalid values remain replayable.

## CRC32C definition

Both checksum fields use reflected CRC32C Castagnoli:

```text
polynomial = 0x82f63b78
initial    = 0xffffffff
final_xor  = 0xffffffff
```

Bits are processed least-significant first. The standard check vector is:

```text
input:  31 32 33 34 35 36 37 38 39   # ASCII 123456789
crc32c: e3069283
```

The checksum bytes themselves are big-endian. CRC32C detects accidental corruption; it is not a
message-authentication code.

## Bounds and checked decoding

Default maximum accepted sizes are:

| Object | Maximum |
| --- | ---: |
| complete log header | 1 MiB |
| one command record | 64 KiB |
| later `ATLSSN01` snapshot | 256 MiB |

A V1 log reader does not allocate from an unchecked length or count. It must:

1. obtain only the fixed prefix needed to read a declared length;
2. reject zero, undersized, excessive, or arithmetically inconsistent values;
3. validate multiplication and addition before computing buffer sizes or offsets;
4. read into a bounded temporary buffer;
5. validate CRC before interpreting variable data;
6. validate versions, counts, payload shape, catalog order, outcome, and sequence; and
7. construct domain values only after all fixed-width conversions succeed.

The V1 record bound is intentionally much larger than its 82-to-106-byte records so malformed or
future-version lengths are rejected before allocation. It does not authorize unknown V1 extension
bytes.

The later snapshot reader defaults to 256 MiB. Only that API may explicitly opt into a larger
bound.

## Scanner classification

Every non-success scanner result includes the byte offset where the condition was detected.
Stable categories are:

| Category | Meaning |
| --- | --- |
| `truncated_final_record` | EOF interrupted the final record length or valid declared body |
| `invalid_length` | a complete length field is undersized or inconsistent |
| `excessive_length` | a complete declared length exceeds its bound |
| `unsupported_format_version` | header format is not V1 |
| `unsupported_record_version` | record version is not V1 |
| `unknown_record_type` | command-type tag is outside V1 |
| `invalid_command_schema` | outcome, payload, presence, placeholder, reason, or count schema is invalid |
| `bad_header_checksum` | complete header CRC does not match |
| `bad_record_checksum` | complete record CRC does not match |
| `duplicate_sequence` | sequence repeats or moves backward |
| `missing_sequence` | sequence advances past the next expected value |
| `semantic_version_mismatch` | stored semantics differ from the reader |
| `catalog_configuration_mismatch` | digest/config differs from the requested engine context |
| `io_failure` | the underlying read operation fails |

A truncated or inconsistent header is `invalid_length`, not a repairable torn record. `clean_eof`
is a successful terminal status, not an error category.

For a final record, EOF before four length bytes is torn. Once the four-byte total is complete, an
undersized or excessive total is corruption because those bytes cannot prefix any V1 record. A
plausible total followed by EOF before the complete eight-byte length prefix is torn; once both
lengths are complete, an impossible `total_length = 66 + payload_length` relationship is
corruption. EOF later in an otherwise structurally possible declared record is torn.

Classification order is deterministic:

1. bounded length availability and arithmetic;
2. complete-object availability;
3. checksum;
4. format/record version and type;
5. structural schema and configuration; and
6. sequence continuity.

This order prevents corrupt payload bytes from being reported as a meaningful domain error before
the record checksum succeeds.

### Torn tail versus corruption

- EOF with no bytes after a complete header/record is clean.
- EOF after one to three bytes of a new total-length field is a torn final record.
- A complete, valid-sized length prefix whose declared final record extends past EOF is a torn
  final record.
- A complete final record with a bad checksum or invalid schema is corruption.
- A complete invalid length is corruption, even at EOF.
- No complete interior record may be skipped.

`valid-prefix` replay may ignore only `truncated_final_record`. All other categories fail.

## Write-ahead session behavior

The record is encoded from an exclusive prepared engine token and appended before token commit:

```text
prepare -> encode -> append -> durability action -> no-throw engine commit
```

Durability modes:

| Mode | Per-record action | Crash claim |
| --- | --- | --- |
| `buffered` | no explicit flush | none |
| `flush_each_record` | stream/runtime flush | bytes handed to OS only |
| `sync_each_record` | flush plus `fsync`/`_commit` | platform file-sync request succeeded |

The default is `sync_each_record`.

A partial append, append failure, flush failure, or sync failure abandons the prepared token and
permanently poisons the session. The session may not submit another command or publish a snapshot.
Recovery reopens the authoritative validated log.

If a complete record reached the file but synchronization returned failure, recovery may replay
that record even though the originating API call returned an operational failure. Continuing the
old in-memory session is forbidden precisely because record visibility is ambiguous.

After a successful write-ahead action, engine commit has no expected failure channel. If the
adapter nevertheless detects a post-commit sequence, event, outcome, or identity mismatch, it
returns `state_not_recoverable`, permanently poisons the session, and requires authoritative-log
recovery. The mismatch cannot be returned as a client rejection and the session may not continue.

## Inspection and repair

```text
atlas_inspect log <path> [--json] [--records]
atlas_inspect repair-tail <input> <new-output> [--json]
```

`repair-tail` writes a distinct, previously nonexistent output only when the final record is torn.
It copies the validated prefix ending immediately before that incomplete record. A clean log needs
no repair and is refused.

It refuses checksum-complete corruption, interior corruption, a damaged header, an output that
resolves to the input, and an existing output. It never truncates or overwrites the source.
Validation completes before the output is created. Copying then performs an identity-checked second
scan; extent, boundaries, sequence metadata, termination, and validated-prefix SHA-256 must still
match. A failed copy, synchronization, close, identity check, or cleanup is reported as an
operational I/O failure. In particular, failure to remove a partial output is never hidden.

## Replay modes

```text
atlas_replay <log> [--mode fast|verify|diagnostic]
                   [--tail-policy strict|valid-prefix]
                   [--json]
```

The scanner completes before replay begins.

| Mode | Required behavior |
| --- | --- |
| `fast` | validate file structure/config/sequence, replay commands, report final digest |
| `verify` | fast behavior plus per-command outcome, reason, event-count, and event-digest comparison |
| `diagnostic` | verify behavior, invariant check after each command, stop and report first difference |

`strict` rejects a torn final record. `valid-prefix` replays the complete prefix and reports a
warning; it never suppresses corruption.

PR3 adds snapshot selection and log-suffix boundary validation. Snapshot options are not part of
the PR2 completion claim.

### Diagnostic limitation

The log stores:

```text
outcome + rejection_reason + event_count + event_digest
```

It does not store expected event payloads. Diagnostic mode can show the command, record offset,
logged metadata, actual replay metadata, and actual recomputed events in human diagnostics. It
cannot derive or display a field-by-field expected event body from `ATLSLG01` alone.

## Machine-readable reports

JSON serializers emit keys in the order below so identical operations can produce byte-identical
reports. JSON object order is not used for parsing correctness, but the writer order is part of
deterministic evidence. Every byte offset, size, count, sequence, and signed/unsigned 64-bit value
is canonical decimal text in a JSON string. There are no timing or path fields.

### `ATLAS_LOG_REPORT_V1`

Required top-level keys, in order:

```text
schema
operation
status
format_version
semantics_version
log_id
first_sequence
header_length
catalog_length
catalog_count
configuration_digest
records_scanned
last_sequence
input_bytes
valid_prefix_bytes
output_bytes
tail
warning
error
records
```

Rules:

- `schema` is `ATLAS_LOG_REPORT_V1`.
- `operation` is `inspect_log` or `repair_tail`.
- `status` is `ok`, `warning`, `invalid`, or `io_error`.
- Header-derived fields are decimal strings or `null` when no valid header exists.
- `log_id` and `configuration_digest` are lowercase hexadecimal or `null`.
- `last_sequence` is `null` when no record exists.
- `output_bytes` is `null` for inspection.
- `tail` is `clean`, `torn`, or `unknown`.
- `warning` and `error` are `null` or an object with keys `category` then `offset`.
- `records` is `null` unless `--records` was requested.

Each record-summary object has keys, in order:

```text
offset
total_length
payload_length
record_version
sequence
command_type
outcome
rejection_reason
event_count
event_digest
```

Numeric values are decimal strings. `command_type`, `outcome`, and `rejection_reason` use stable
lowercase names; committed records report rejection reason `none`.

### `ATLAS_REPLAY_REPORT_V1`

Required top-level keys, in order:

```text
schema
status
mode
tail_policy
semantics_version
log_id
first_sequence
last_sequence
records_available
records_replayed
committed
rejected
final_state_digest
tail
warning
error
divergence
```

Rules:

- `schema` is `ATLAS_REPLAY_REPORT_V1`.
- `status` is `ok`, `warning`, `diverged`, `invalid`, or `io_error`.
- `mode` and `tail_policy` echo the canonical selected values.
- `final_state_digest` is lowercase hexadecimal or `null` if replay never constructed a valid
  final engine.
- `warning` and `error` follow the log-report category/offset shape.
- `divergence` is `null` or the object below.

A divergence object has keys, in order:

```text
sequence
record_offset
category
command
expected
actual
actual_engine_error
actual_events
```

`category` is one of `engine_error`, `outcome`, `rejection_reason`, `event_count`, `event_digest`,
or `invariant`. `expected` and `actual` are objects with keys `outcome`, `rejection_reason`,
`event_count`, and `event_digest`; unavailable values are explicit `null`. `command` is the owned
normalized command decoded from the record, including raw numeric enum values and explicit
optional-price presence; it is `null` only for a whole-engine invariant failure discovered after
the final record. `actual_engine_error` is the stable engine-error name. `actual_events` contains
the complete owned recomputed events in canonical event order, using the same field vocabulary as
`ATLAS_DIFF_V2`; every numeric field is a decimal string and every absent optional is `null`.

Expected event bodies are intentionally absent because they are not recoverable from the stored
digest. The command and actual-event bodies still make the first observed replay difference
actionable without inventing expected data.

## Exit codes

| Code | Meaning |
| ---: | --- |
| 0 | clean success, or successful `valid-prefix` replay with an explicit warning |
| 1 | invalid data, corruption, strict torn-tail rejection, or replay divergence |
| 2 | CLI usage error |
| 3 | operational I/O failure |

No report includes elapsed time, throughput, latency, durability, security, scalability, or
production-readiness claims.
