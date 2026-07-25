# AtlasLOB persisted snapshot format V1

This document is the byte-level reference for `ATLSSN01`. It implements
[ADR 0014](decisions/0014-persisted-snapshots-and-log-suffix-recovery.md).

The format is a canonical image of one multi-instrument engine at one exact `ATLSLG01` log
boundary. It is not a native object dump, a replacement for the command log, a network protocol,
an authentication format, or a universal durability guarantee.

Phase 4 PR3 implements this format on its working branch. The focused codec, restoration,
publication, recovery, inspection, report, and CLI process-boundary checks pass locally. GCC Debug
and Release each pass 482/482 CTest cases, and the production-only and Python gates pass. Hosted
Clang, sanitizer, actual Clang libFuzzer execution, pull-request, and merge gates remain pending,
so this reference does not claim final PR3 acceptance.

## Encoding conventions

- Byte offsets are zero-based.
- `u8`, `u16`, `u32`, and `u64` are unsigned fixed-width integers.
- `i64` is encoded as its 64-bit two's-complement bit pattern.
- Every multibyte integer is most-significant byte first.
- No field has implicit alignment or padding.
- ASCII magic and domain-separation prefixes are the exact bytes shown.
- A SHA-256 value is 32 raw bytes in the file and 64 lowercase hexadecimal characters in reports.
- A log ID is 16 opaque bytes in the file and 32 lowercase hexadecimal characters in reports.
- A boolean is one `u8` and is exactly 0 or 1.
- `SIZE_MAX` used as an in-memory unbounded-capacity sentinel encodes as `UINT64_MAX`.
- Collection and block lengths are inclusive or exclusive exactly as stated below.

The current engine semantic version is 6. A reader must compare the stored semantic version with
the semantics it implements. Format version 1 does not permit silent semantic reinterpretation.

## Complete file

For `N` configured instruments:

```text
fixed header                         169 bytes
catalog[N]                            28 bytes each
instrument blocks[N]                 variable
whole-file CRC32C                      4 bytes
EOF
```

The fixed header plus catalog is called the header. There is no footer, optional extension area,
index, timestamp, path, or end marker before the trailing CRC.

For an empty one-instrument engine, the smallest valid file is 237 bytes:

```text
169 fixed + 28 catalog + 36 empty instrument + 4 CRC
```

## Header

For `N` catalog entries, `catalog_length` is exactly `28 * N` and `header_length` is exactly
`169 + 28 * N`.

| Offset | Width | Field | V1 rule |
| ---: | ---: | --- | --- |
| 0 | 8 | magic | ASCII `ATLSSN01` |
| 8 | 2 | format version | exactly `1` |
| 10 | 2 | semantic version | engine semantic version |
| 12 | 4 | byte-order marker | exactly `0x01020304` |
| 16 | 8 | snapshot length | complete file length, including trailing CRC |
| 24 | 8 | header length | `169 + 28 * N` |
| 32 | 16 | log ID | copied from the paired `ATLSLG01` header |
| 48 | 8 | covered sequence | authoritative global sequence; zero is permitted |
| 56 | 8 | covered log byte offset | byte immediately after the covered record |
| 64 | 1 | sequence exhausted | exactly 0 or 1 |
| 65 | 8 | active-order count | global count |
| 73 | 8 | maximum total active orders | canonical engine-wide capacity |
| 81 | 4 | catalog count | `N`, nonzero |
| 85 | 8 | catalog length | exactly `28 * N` |
| 93 | 4 | instrument count | exactly `N` |
| 97 | 8 | instruments length | exact sum of all instrument-block lengths |
| 105 | 32 | configuration digest | SHA-256 of the `ATLSCF01` input |
| 137 | 32 | state digest | SHA-256 of the reconstructed `ATLSME01` input |
| 169 | `28 * N` | catalog | sorted entries below |
| `header_length` | `instruments_length` | instruments | sorted blocks below |
| `snapshot_length - 4` | 4 | whole-file CRC32C | checksum of every preceding file byte |

The following relationships are exact:

```text
catalog_length     = 28 * catalog_count
header_length      = 169 + catalog_length
instrument_count   = catalog_count
snapshot_length    = header_length + instruments_length + 4
```

Extra header, catalog, instrument, or trailing bytes are not V1 extensions. They are invalid.

### Catalog entry

Each entry is exactly 28 bytes and is byte-identical to the `ATLSLG01` catalog entry.

| Relative offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | instrument ID |
| 4 | 8 | maximum order quantity |
| 12 | 8 | tick increment (`i64`) |
| 20 | 8 | maximum active orders |

Instrument IDs are nonzero, unique, and strictly increasing. Maximum order quantity and tick
increment are positive. Capacity values retain their canonical `u64` form; restoration additionally
requires conversion to the host representation without loss.

### Configuration digest

The stored digest is the same `ATLSCF01` configuration identity used by the paired command log.
Its exact SHA-256 input is:

| Offset | Width | Field |
| ---: | ---: | --- |
| 0 | 8 | ASCII `ATLSCF01` |
| 8 | 2 | semantic version |
| 10 | 8 | maximum total active orders |
| 18 | 8 | catalog count as `u64` |
| 26 | `28 * N` | sorted catalog entries in file encoding |

The input length is exactly `26 + 28 * N`. The `u64` digest count is intentional even though the
file header count is `u32`.

## Instrument blocks

There is exactly one block for every catalog entry, in the same strictly ascending instrument-ID
order. An empty instrument has a 36-byte block with zero active orders and zero bid/ask levels.

| Relative offset | Width | Field | V1 rule |
| ---: | ---: | --- | --- |
| 0 | 8 | instrument-block length | inclusive of this field and all contained levels |
| 8 | 4 | instrument ID | exactly the corresponding catalog ID |
| 12 | 8 | active-order count | exact sum of contained level order counts |
| 20 | 8 | bid-level count | number of following bid blocks |
| 28 | 8 | ask-level count | number of ask blocks after all bids |
| 36 | variable | bid levels | strictly descending price |
| variable | variable | ask levels | strictly ascending price |

For `B` bids and `A` asks:

```text
instrument_block_length =
    36
  + sum(bid_level_block_length[0..B))
  + sum(ask_level_block_length[0..A))
```

The sum of all instrument-block lengths is exactly the header's `instruments_length`. Blocks may
not overlap, leave gaps, or extend into the trailing CRC.

## Price-level blocks

Every price-level block is nonempty.

| Relative offset | Width | Field | V1 rule |
| ---: | ---: | --- | --- |
| 0 | 8 | level-block length | `32 + 41 * order_count` |
| 8 | 8 | level price (`i64`) | positive and tick-aligned |
| 16 | 8 | aggregate quantity | exact checked sum of remaining quantities |
| 24 | 8 | order count | positive |
| 32 | `41 * order_count` | FIFO orders | fixed entries below |

Bid levels are strictly descending and ask levels are strictly ascending. Duplicate prices and
empty levels are invalid. If both sides are nonempty, best bid must be strictly less than best ask.

## FIFO order entries

Each order entry is exactly 41 bytes.

| Relative offset | Width | Field |
| ---: | ---: | --- |
| 0 | 8 | order ID |
| 8 | 4 | client ID |
| 12 | 4 | instrument ID |
| 16 | 1 | side: buy 1, sell 2 |
| 17 | 8 | price (`i64`) |
| 25 | 8 | remaining quantity |
| 33 | 8 | global priority sequence |

Order and client IDs are nonzero. Instrument ID, side, and price exactly repeat the containing
instrument, book side, and level price. Remaining quantity is positive and no greater than the
instrument's maximum-order-quantity policy.

Priority is nonzero, no greater than the covered sequence, globally unique, and strictly increasing
within each FIFO level. Order IDs are globally unique across every instrument. No terminal-order
history is stored.

## Canonical state digest

The stored state digest is the SHA-256 result of the existing `ATLSME01` encoding. File-only
identity and framing fields—log ID, covered log offset, block lengths, and CRC—do not enter this
digest.

The exact digest input is:

```text
ASCII ATLSME01                              8 bytes
semantic_version:u16                       2 bytes
maximum_total_active_orders:u64            8 bytes
catalog_count:u64                          8 bytes
catalog entries                            28 * N bytes
covered_sequence:u64                       8 bytes
sequence_exhausted:u8                      1 byte
global_active_order_count:u64              8 bytes
instrument_count:u64                       8 bytes
instrument state blocks                    variable
```

Each digest instrument state block omits the file block length and encodes:

```text
instrument_id:u32
active_order_count:u64
bid_level_count:u64
bid level states
ask_level_count:u64
ask level states
```

Each digest level state omits the file block length and encodes:

```text
price:i64
aggregate_quantity:u64
order_count:u64
FIFO order entries in the same 41-byte field layout
```

The decoder reconstructs this stream independently from decoded values and requires exact digest
agreement.

## Sequence and log-boundary rules

`covered_sequence` is stored directly and is not inferred from active orders.

- Sequence 0 requires a non-exhausted, empty engine. Its covered log offset is exactly
  `96 + 28 * catalog_count`, the paired V1 log-header length.
- A positive sequence requires a covered offset greater than the paired log-header length.
- Exhaustion is 1 exactly when the covered sequence is `UINT64_MAX`; otherwise it is 0.
- Every active priority is at most the covered sequence.

Standalone snapshot inspection can validate the sequence/state relationship and the minimum
possible offset. Pairing with a log additionally requires:

1. matching semantic version, log ID, catalog, capacities, and configuration digest;
2. a structurally valid, checksum-valid, contiguous log prefix through the recorded offset;
3. sequence 0 ending exactly at the log header, or the covered record ending exactly at the
   recorded offset; and
4. the first record starting at that offset carrying exactly `covered_sequence + 1`, if such a
   record exists.

An exhausted snapshot cannot have a complete suffix record.

## CRC32C definition and coverage

The trailing checksum uses reflected CRC32C Castagnoli:

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

For declared snapshot length `S`, CRC input is bytes `[0, S - 4)`. The stored big-endian checksum
occupies `[S - 4, S)`. There are no per-block checksums.

CRC32C detects accidental corruption; it is not a message-authentication code. A valid checksum
does not waive format, semantic, catalog, hierarchy, digest, sequence, or log-pairing validation.

## Bounds and checked decoding

The default maximum accepted snapshot size is exactly 256 MiB:

```text
268435456 bytes
```

Only the snapshot API may explicitly select a larger positive bound. Raising it does not relax any
length, count, host-conversion, or schema check and does not change the fixed log header and record
bounds.

A V1 reader:

1. obtains a stable observed extent and rejects an excessive or nonrepresentable file before
   allocation; a streaming length-inspection seam requires only the first 24 bytes to obtain
   `snapshot_length`;
2. rejects an undersized, truncated, overlong, or declared/observed-length mismatch before
   interpreting variable hierarchy;
3. performs checked arithmetic before every conversion, multiplication, addition, and offset
   advance;
4. reads exactly one bounded temporary byte image;
5. validates the whole-file CRC before interpreting variable hierarchy;
6. validates magic, versions, marker, all redundant lengths/counts, and every canonical rule; and
7. constructs host values only after lossless representation checks.

Classification order is deterministic:

1. fixed-prefix and total-length availability;
2. configured bound and exact file extent;
3. whole-file checksum;
4. magic, format version, semantic version, and byte-order marker;
5. header, section, block, and count relationships;
6. catalog and configuration digest;
7. hierarchy and sequence schema; and
8. state digest.

Snapshots do not have a torn-tail exception. Any truncation or extra byte is `invalid_length`; a
complete checksum failure is `bad_checksum`.

## Required schema validation

A checksum-valid V1 image is still invalid if any of the following is observed:

- empty, duplicate, unsorted, zero-ID, or policy-invalid catalog;
- instrument count/order disagreement with the catalog;
- a missing, duplicate, unsorted, or unconfigured instrument;
- wrong section or block length, overflow, overlap, gap, or trailing bytes;
- duplicate/unsorted level price, empty level, invalid tick, or crossed book;
- zero/unknown identity, side, price, quantity, or hierarchy mismatch;
- duplicate global order ID or priority;
- zero priority, priority newer than the covered sequence, or nonmonotonic FIFO priority;
- aggregate, level count, instrument count, or global count disagreement;
- active count beyond local or global capacity;
- inconsistent covered sequence, offset, or exhaustion;
- configuration-digest disagreement; or
- state-digest disagreement.

The decoder never repairs, sorts, deduplicates, recomputes-and-substitutes, or ignores these faults.

## Stable snapshot errors

Every error carries the first deterministic byte offset associated with the failure and one stable
category. Public numeric values are frozen:

| Value | Category | Meaning |
| ---: | --- | --- |
| 0 | `none` | no error |
| 1 | `invalid_length` | truncated, overlong, undersized, or inconsistent length/count extent |
| 2 | `excessive_length` | declared complete snapshot exceeds the selected bound |
| 3 | `unsupported_format_version` | magic, format version, or byte-order marker is unsupported |
| 4 | `semantic_version_mismatch` | stored semantics differ from the reader |
| 5 | `bad_checksum` | whole-file CRC32C does not match |
| 6 | `configuration_digest_mismatch` | recomputed `ATLSCF01` or paired-log configuration identity differs |
| 7 | `invalid_catalog` | catalog identity, order, count, or policy is invalid |
| 8 | `invalid_snapshot_schema` | hierarchy, value, order, count, aggregate, capacity, or sequence state is invalid |
| 9 | `state_digest_mismatch` | recomputed `ATLSME01` differs |
| 10 | `log_id_mismatch` | snapshot and selected log IDs differ |
| 11 | `log_boundary_mismatch` | covered byte offset is not the required validated log boundary |
| 12 | `sequence_mismatch` | covered or first suffix sequence disagrees with the paired log |
| 13 | `io_failure` | an underlying file or directory operation fails |

Categories 10 through 12 arise while pairing an otherwise decoded snapshot with a log. Snapshot
inspection alone does not claim boundary verification.

### Error-offset rules

Offsets identify the first deterministic field or byte associated with the reported failure. The
same canonical offsets are used by value validation before encoding, host conversion after
decoding, and byte decoding:

- fixed-header errors use the offsets in the header table;
- catalog entry `i` begins at `169 + 28 * i`;
- an instrument block begins after the header and every preceding instrument block;
- within an instrument block, instrument ID and active count are at `base + 8` and `base + 12`;
- a level begins after the 36-byte instrument prefix and every preceding level block;
- within a level, price, aggregate, and order count are at `base + 8`, `base + 16`, and
  `base + 24`;
- FIFO order `j` begins at `level_base + 32 + 41 * j`, with client, instrument, side, price,
  remaining quantity, and priority at relative offsets 8, 12, 16, 17, 25, and 33; and
- the first unexpected hierarchy byte before the trailing CRC is the offset for otherwise
  well-framed trailing hierarchy data.

A duplicate identity or priority points to the later occurrence. A noncanonical level order or
crossed book points to the later level price that first violates the rule. Count and aggregate
disagreements point to the stored count or aggregate field. When a pairing failure is about the
stored log boundary, its public offset is the stored covered-log offset; a pairing or candidate
failure with no single corresponding snapshot byte uses zero.

The reviewed 561-byte populated golden freezes these representative diagnostics:

| Failure | Category | Offset |
| --- | --- | ---: |
| wrong byte-order marker | `unsupported_format_version` | 12 |
| wrong configuration digest | `configuration_digest_mismatch` | 105 |
| wrong state digest | `state_digest_mismatch` | 137 |
| duplicate second catalog instrument | `invalid_catalog` | 197 |
| wrong level aggregate | `invalid_snapshot_schema` | 277 |
| wrong order side | `invalid_snapshot_schema` | 309 |
| zero remaining quantity through every value entry point | `invalid_snapshot_schema` | 318 |
| duplicate order ID | `invalid_snapshot_schema` | 334 |
| duplicate global priority | `invalid_snapshot_schema` | 367 |
| crossed ask price | `invalid_snapshot_schema` | 456 |
| duplicate second instrument block | `invalid_snapshot_schema` | 529 |
| first extra hierarchy byte before CRC | `invalid_length` | 557 |

## All-or-nothing restoration

Restoration uses decoded temporary values and a private core boundary. Before returning a new
engine it must:

- losslessly convert capacities and counts to the host representation;
- eagerly create the exact configured catalog;
- reserve each book's owning storage and local index plus both coordinator directories;
- allocate every side level;
- allocate every stable node and insert every local index, active-ID entry, and
  `Sequence -> OrderId` active-priority entry before publishing FIFO links;
- link already allocated nodes in persisted FIFO order with exact remaining quantities and
  priorities, without allocation;
- restore `next_sequence = covered_sequence + 1` when not exhausted, or the exhausted zero-next
  state after `UINT64_MAX`, only after the books and directories are complete;
- run full book and whole-engine invariants, including reciprocal checks between nodes and both
  global directories; and
- reproduce the stored `ATLSME01` digest.

The engine is published only after every step succeeds. Failure destroys the temporary engine and
all temporary values. No partial state becomes observable. Whole-engine priority validation makes
one traversal of active nodes and both directories with a fixed number of hash lookups per entry,
removing the former pairwise priority scan. This describes the validation algorithm; it is not a
benchmark claim or a worst-case hash-table bound.

## Publication and canonical names

The final filename is exactly:

```text
atlaslob-<log-id>-<sequence>.snapshot
```

where:

- `<log-id>` is exactly 32 lowercase hexadecimal characters; and
- `<sequence>` is exactly 20 zero-padded decimal digits.

Examples:

```text
atlaslob-00112233445566778899aabbccddeeff-00000000000000000000.snapshot
atlaslob-00112233445566778899aabbccddeeff-18446744073709551615.snapshot
```

Publication synchronizes the command log through the covered offset, writes and synchronizes a
unique temporary regular file in the same directory, closes and rereads it, performs a complete
decode and source-value comparison, then uses an atomic no-replace rename to the final unique name.

Existing final files are never overwritten. Temporary names must not match the canonical pattern.
A failed publication leaves prior final snapshots untouched; leftover temporary files are ignored
by discovery. Temporary cleanup failure is surfaced. A late unlink or containing-directory sync
failure can be reported after the new final has become visible, so the publication result carries
`final_file_visible` independently of its error. The new file was fully reread and validated
before that point, but the operation remains failed and no prior final was replaced.

## Discovery and suffix recovery

Directory discovery considers ASCII filenames that exactly match the canonical pattern for the
selected log ID. It parses the 20-digit sequence and orders candidates numerically newest to
oldest. Discovery queries non-following status: a canonical symlink is never opened through its
target and is recorded as a candidate-local failure. A usable candidate must itself be a regular
file.

For each candidate it:

1. fully decodes and validates the snapshot;
2. requires decoded log ID and sequence to match the filename;
3. requires exact log semantic/configuration identity;
4. validates the log prefix and exact covered byte boundary; and
5. requires the first complete suffix record to be `covered_sequence + 1`.

Invalid or unreadable matching candidates are recorded by candidate sequence, stable category, and
offset before discovery tries the next older candidate. Unrelated files, malformed names, temporary
names, and names for another log ID are ignored.

If no valid candidate remains, directory recovery performs full-log replay and reports that
fallback. An explicit snapshot path does not fall back: invalid snapshot data exits as invalid, and
an operational read failure exits as I/O.

Candidate-local status, type, open, read, extent, codec, compatibility, and restore errors are
skippable. Directory enumeration failure is terminal. Recovery retains one opened log source
across its validation passes, compares validated source identity, and retains only the requested
snapshot candidate boundaries rather than one boundary per record.

The selected log tail policy still applies to the whole file. `strict` rejects a torn final command
record. `valid-prefix` may recover the complete prefix with an explicit warning. Corruption is
never downgraded.

### Writable `LoggedEngine` resumption

Standalone replay and snapshot recovery can return a value-only engine for a `valid-prefix` torn
tail. Writable recovery is stricter:

- `LoggedEngine::recover`, `recover_from_snapshot`, and
  `recover_from_snapshot_directory` require a clean tail;
- they reopen only an existing log, never create or truncate it;
- the append descriptor uses `_O_APPEND` on Windows or `O_APPEND` on POSIX;
- the descriptor extent must still equal the validated end offset; and
- the existing log is marked published at construction, so later destruction never removes it.

Consequently, neither strict nor valid-prefix policy turns a torn file into a writable session.
`atlas_inspect repair-tail` must first copy the valid prefix to a distinct new clean file, after
which that new file can be recovered for append. Complete corruption remains non-repairable.
These rules operate under Phase 4's one-process, one-logical-writer assumption.

## Command-line behavior

```text
atlas_inspect snapshot <path> [--json]

atlas_replay <log> [--snapshot <path>|--snapshot-dir <dir>]
                   [--mode fast|verify|diagnostic]
                   [--tail-policy strict|valid-prefix]
                   [--json]
```

Snapshot options are mutually exclusive. Snapshot inspection performs a full standalone decode,
including CRC and both digest recomputations. It reports the covered offset but cannot prove that
offset belongs to a log without a log argument.

Log-only replay retains the exact `ATLAS_REPLAY_REPORT_V1` behavior frozen by the command-log
reference. Snapshot-aware replay uses V2 below.

## Machine-readable reports

JSON serializers emit keys in the order below. Object order is not needed for parsing correctness,
but deterministic writer order is part of the evidence contract. Every byte offset, length, count,
sequence, capacity, quantity, and signed/unsigned 64-bit value is canonical decimal text in a JSON
string. Digests are lowercase hexadecimal. There are no elapsed times or host paths.

### `ATLAS_SNAPSHOT_REPORT_V1`

Required top-level keys, in order:

```text
schema
operation
status
format_version
semantics_version
log_id
covered_sequence
covered_log_offset
declared_snapshot_length
header_length
catalog_length
instruments_length
catalog_count
instrument_count
active_order_count
sequence_exhausted
configuration_digest
state_digest
input_bytes
error
```

Rules:

- `schema` is `ATLAS_SNAPSHOT_REPORT_V1`.
- `operation` is `inspect_snapshot`.
- `status` is `ok`, `invalid`, or `io_error`.
- `input_bytes` is the observed input extent as a decimal string when available.
- Snapshot-derived numeric fields are decimal strings or `null` when their containing structure was
  not validated far enough to trust them.
- `sequence_exhausted` is a JSON boolean or `null`.
- `log_id`, `configuration_digest`, and `state_digest` are lowercase hexadecimal or `null`.
- `error` is `null` or an object with keys `category` then `offset`; offset is a decimal string or
  `null` when no file offset exists.

### `ATLAS_REPLAY_REPORT_V2`

This schema is used only when `--snapshot` or `--snapshot-dir` is present. Required top-level keys,
in order:

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
records_covered_by_snapshot
records_replayed
committed
rejected
final_state_digest
tail
recovery_source
snapshot
skipped_snapshots
warnings
error
divergence
```

Rules:

- `schema` is `ATLAS_REPLAY_REPORT_V2`.
- `status` is `ok`, `warning`, `diverged`, `invalid`, or `io_error`.
- `mode` and `tail_policy` echo their canonical selected values.
- `records_available`, `committed`, and `rejected` describe the complete validated log prefix.
- `records_covered_by_snapshot` is the number of contiguous records represented by the selected
  snapshot, which equals its covered sequence in V1, or zero for full-log fallback.
- `records_replayed` is the number of suffix records actually executed.
- Whenever the log has a validated prefix, `committed + rejected = records_available`, including
  reports where snapshot selection later fails.
- After successful recovery,
  `records_covered_by_snapshot + records_replayed = records_available`. Before a snapshot is
  successfully selected, covered and replayed counts remain zero even when records are available.
- `recovery_source` is `full_log`, `explicit_snapshot`, or `directory_snapshot`.
- `snapshot` is `null` for full-log recovery or an object with keys `covered_sequence`,
  `covered_log_offset`, and `state_digest`.
- `skipped_snapshots` is an array. Each object has keys `candidate_sequence`, `category`, and
  `offset`. It is empty outside directory selection.
- `warnings` is an array of objects with keys `category` then `offset`. Stable categories are
  `truncated_final_record`, `snapshot_candidates_skipped`, and `snapshot_fallback_full_log`.
- `error` follows the snapshot-report error shape.
- `divergence` is `null` or the exact divergence object frozen by
  `ATLAS_REPLAY_REPORT_V1`: `sequence`, `record_offset`, `category`, `command`, `expected`,
  `actual`, `actual_engine_error`, then `actual_events`.

Skipping a matching directory candidate makes status `warning` even when recovery succeeds.
Falling back to full-log replay adds `snapshot_fallback_full_log`; it also adds
`snapshot_candidates_skipped` when one or more matching candidates were actually rejected. A
valid-prefix torn tail adds `truncated_final_record`.

## Exit codes

| Code | Meaning |
| ---: | --- |
| 0 | valid inspection or successful recovery, including explicitly reported warnings |
| 1 | invalid data, corruption, incompatibility, strict torn-tail rejection, or replay divergence |
| 2 | command-line usage error |
| 3 | terminal operational I/O failure that prevents inspection or recovery |

A candidate-local directory read failure may be recorded and skipped under exit 0 when an older
snapshot or full-log replay succeeds. Directory enumeration failure and explicit-snapshot I/O
failure are terminal exit 3 conditions.

No report or snapshot-format claim includes elapsed time, throughput, latency, authentication,
universal durability, scalability, or production readiness.
