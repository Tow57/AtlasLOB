# ADR 0013: Append-only command log and deterministic replay

- Status: accepted
- Date: 2026-07-24
- Implementation state: Phase 4 PR2 is complete locally; hosted Clang, sanitizer, libFuzzer, pull
  request, and merge gates remain pending.

## Context

ADR 0012 establishes one global sequence, one global active-order namespace, and an exclusive
prepare/commit token for the multi-instrument engine. That boundary permits a persistence layer to
observe a command's complete normalized result before publishing book, directory, or sequence
state.

Phase 4 needs an authoritative command history that can:

- retain accepted and rejected domain commands in one contiguous global order;
- survive a crash between persistence and in-memory publication;
- be scanned without constructing the matching engine;
- distinguish a torn final append from complete corruption;
- replay deterministically and verify recorded outcomes;
- support safe tail repair without modifying the original file; and
- remain independent of native object layout, padding, host byte order, pointers, clocks, and
  filesystem metadata.

The command log is not an event journal. It retains the normalized command, authoritative sequence,
outcome classification, rejection reason, event count, and the existing canonical event digest.
It deliberately does not duplicate complete event payloads. Replay can therefore verify a
recomputed batch exactly by count and digest, but a log alone cannot reconstruct the expected body
of a divergent event.

CRC32C detects accidental storage or transfer corruption. It does not authenticate a writer and
must not be described as protection against malicious modification.

## Decision

### Ownership and target boundary

A new `AtlasLOB::persistence` target owns command-log encoding, scanning, filesystem I/O, repair,
inspection, and replay. The deterministic domain and core targets do not depend on filesystems,
CRC codecs, persistence sessions, or command-line reporting.

One persistence session owns one logical writer, one open append-only log, and one
`MultiInstrumentEngine`. Distributed locking, concurrent writers, replication, rotation, pruning,
encryption, authentication, and in-place repair are outside Phase 4.

The canonical format is `ATLSLG01`, format version 1. Its complete byte layout is frozen in
[the command-log format reference](../command-log-format.md). All multibyte values are big-endian,
all lengths include the fields stated by that reference, and no alignment or implicit padding is
permitted.

### Header contract

The V1 header contains, in order:

1. eight ASCII bytes `ATLSLG01`;
2. format version (`u16`, exactly 1);
3. semantic version (`u16`);
4. byte-order marker (`u32`, exactly `0x01020304`);
5. total header length (`u32`, inclusive through the header CRC);
6. catalog-entry byte length (`u32`);
7. an opaque 16-byte log ID;
8. first sequence (`u64`, exactly 1 in V1);
9. canonical global active-order capacity (`u64`);
10. catalog count (`u32`);
11. the canonical sorted catalog;
12. a 32-byte catalog/configuration SHA-256 digest; and
13. a trailing CRC32C (`u32`).

Each catalog entry is exactly 28 bytes: instrument ID (`u32`), maximum order quantity (`u64`),
tick increment (two's-complement `i64`), and maximum active orders (`u64`). For `N` entries,
`catalog_length` is exactly `28 * N` and `header_length` is exactly `96 + 28 * N`.

The in-memory `SIZE_MAX` unbounded-capacity sentinel encodes as `UINT64_MAX`, preserving the
cross-host rule established by ADR 0012.

The ordinary create-new session obtains 16 opaque bytes from the implementation's operating-system
randomness source. The low-level codec and an explicit C++ construction seam accept any
caller-supplied 16-byte value so golden tests and controlled recovery tools are deterministic.
The value has no UUID layout and carries no cryptographic uniqueness, secrecy, or authentication
claim.

The configuration digest input is:

```text
ATLSCF01
semantic_version:u16
max_total_active_orders:u64
catalog_count:u64
catalog_entry[0..N): 28 bytes each
```

It uses the same fixed-width big-endian and signed-tick rules as the header. Catalog entries must
be nonzero, unique, valid, and strictly ascending by instrument ID. The SHA-256 digest is
deterministic compatibility evidence, not authentication.

The header CRC covers every header byte from the first magic byte through the final configuration
digest byte. It excludes only the trailing four-byte CRC field.

### Record contract

Every record begins with a bounded total length, carries one complete command, and ends with its
own CRC32C. The fixed envelope, including the CRC but excluding the command payload, is 66 bytes:

1. total record length (`u32`, inclusive from this field through the CRC);
2. payload length (`u32`);
3. record version (`u16`, exactly 1);
4. command type (`u8`: New 1, Cancel 2, Replace 3);
5. outcome (`u8`: committed 1, rejected 2);
6. authoritative global sequence (`u64`);
7. rejection reason (`u16`);
8. event count (`u64`);
9. canonical event digest (32 bytes);
10. command payload; and
11. trailing CRC32C (`u32`).

`total_length` must equal `66 + payload_length`. V1 payload sizes are fixed by command type:

- New: 36 bytes, total record length 102;
- Cancel: 16 bytes, total record length 82; and
- Replace: 40 bytes, total record length 106.

New stores client (`u32`), order (`u64`), instrument (`u32`), raw side/order-type/time-in-force
bytes, one optional-price presence byte, an `i64` price slot, and quantity (`u64`). Presence is
exactly zero or one. An absent price requires a zero price slot. Raw enum bytes are retained even
when domain validation will reject them.

Cancel stores client (`u32`), order (`u64`), and instrument (`u32`). Replace stores client (`u32`),
old and new order IDs (`u64` each), instrument (`u32`), new price (`i64`), and new quantity (`u64`).

A committed record has rejection reason zero. A rejected record has one recognized nonzero
`RejectReason`. Event count is nonzero, and the digest is the existing `ATLSEV01` digest of the
complete prepared batch. The record CRC covers the record from `total_length` through the last
payload byte and excludes only the trailing CRC.

Records must have contiguous sequences beginning at the header's `first_sequence`. V1 neither
permits gaps nor stores caller-selected sequences.

### CRC32C

Header and record checksums use reflected CRC32C Castagnoli:

- reflected polynomial: `0x82f63b78`;
- initial value: `0xffffffff`;
- each input byte processed least-significant bit first; and
- final XOR: `0xffffffff`.

The check value for ASCII `123456789` is `0xe3069283`. A checksum mismatch is corruption even when
it occurs in the last complete record. CRC success does not waive structural, semantic-version,
catalog, length, sequence, command-schema, outcome, or digest validation.

### Included and excluded submissions

The log contains every command that successfully reaches domain preparation and receives a
sequenced event batch:

- committed commands; and
- pure, routing, policy, identity, capacity, and other state rejections.

The log excludes:

- text, binary, or Python conversion failures before domain submission;
- sequence-exhaustion attempts, because they receive neither a new sequence nor an event batch;
- allocation or other internal preparation failures that publish no sequence; and
- filesystem failures that prevent a complete accepted append operation.

A complete record may be visible after a flush or synchronization failure even though the
in-memory command was not committed. Recovery treats the validated log as authoritative and
replays that record.

### Write-ahead order and durability modes

Submission uses the exclusive engine token from ADR 0012:

1. prepare the command, reserving but not publishing its sequence;
2. encode the command and its complete expected result;
3. append the complete record;
4. apply the selected durability operation;
5. commit the prepared engine token through its production no-throw path; and
6. return the owned result.

Durability modes are:

- `buffered`: append to the process stream without a per-record flush;
- `flush_each_record`: flush language/runtime buffers to the operating system after each record;
  and
- `sync_each_record`: flush, then request file synchronization with `fsync` or `_commit`.

`sync_each_record` is the persistence API default. `buffered` explicitly makes no crash-durability
promise. A successful sync is evidence that the platform synchronization request succeeded; it is
not a claim about storage hardware, distributed durability, or recovery from every power-loss
model.

Snapshot publication in PR3 must synchronize the command log through the snapshot's covered
record, regardless of the session's ordinary durability mode.

### Poisoning and the impossible-commit boundary

Any partial append, append error, flush error, or synchronization error before engine commit
permanently poisons that persistence session. The prepared engine token is abandoned, so visible
book, directory, and published sequence state remain unchanged. A poisoned session accepts no
further submissions or snapshot publication and must be replaced by recovery from its
authoritative log.

The production engine-token commit has no expected failure channel: preparation has already
allocated and validated every required resource. Once write-ahead persistence succeeds, a failed
post-commit equivalence check would leave a durable record with potentially ambiguous in-memory
publication. The persistence adapter therefore reports `state_not_recoverable`, permanently
poisons the session, and requires recovery from the authoritative log. It is never translated to a
client rejection and the session may not continue.

A defensively representable internal anomaly detected before commit begins returns an internal
operational error without publishing the reserved sequence or any book/identity mutation.

### Bounded scanner

The scanner validates the header before allocating catalog storage, then walks records in byte
order. It performs checked arithmetic before addition, multiplication, offset advancement, or
allocation.

Default V1 limits are:

- complete log header: 1 MiB;
- one record: 64 KiB; and
- persisted snapshot input: 256 MiB for the later `ATLSSN01` codec.

Header and record limits are fixed safety bounds for the V1 log codec. The later snapshot API may
raise its snapshot bound explicitly.

Structured scanner outcomes carry the byte offset and one stable category:

- clean EOF;
- truncated final record;
- invalid length;
- excessive length;
- unsupported format version;
- unsupported record version;
- unknown record type;
- bad header checksum;
- bad record checksum;
- invalid command schema;
- duplicate sequence;
- missing sequence;
- semantic-version mismatch;
- catalog/configuration mismatch; or
- general I/O failure.

EOF exactly after the header or a complete record is clean. EOF after one to three bytes of a new
length field, or before the declared end of an otherwise structurally possible final record, is a
torn final tail. Once the four-byte total exists, an undersized/excessive total is corruption; once
the eight-byte length prefix exists, an impossible total/payload relationship is corruption. A
checksum-complete record with a bad CRC, invalid payload, unsupported version/type, or broken
sequence is likewise corruption, not a torn tail.

### Inspection and safe tail repair

The inspection interface is:

```text
atlas_inspect log <path> [--json] [--records]
atlas_inspect repair-tail <input> <new-output>
```

Snapshot inspection is added with the separate PR3 snapshot format.

Inspection never submits commands. `--records` adds bounded per-record metadata; it does not emit
event bodies because they are not stored.

Tail repair:

- scans the original without modification;
- accepts only a log whose sole defect is a truncated final record; a clean log needs no repair;
- copies only the complete validated prefix;
- creates the destination only after the first scan proves the tail is repairable, then repeats the
  scan while copying and requires identical extent, boundaries, sequences, termination, and
  validated-prefix digest;
- always writes a distinct new output and never truncates or overwrites the input;
- never skips interior corruption or a checksum-complete invalid final record; and
- reports the source size, copied valid-prefix size, and whether a torn tail was removed; and
- surfaces write, sync, close, and partial-output cleanup failures as operational errors.

An output path that resolves to the input or already exists is rejected rather than overwritten.

### Replay

The replay interface is:

```text
atlas_replay <log> [--mode fast|verify|diagnostic]
                   [--tail-policy strict|valid-prefix]
                   [--json]
```

PR3 extends this interface with `--snapshot` and `--snapshot-dir`; PR2 does not claim persisted
snapshot recovery.

Replay first scans the complete selected input and validates checksums, bounds, configuration, and
sequence continuity before constructing or mutating the recovered engine.

- `strict` rejects a torn final tail.
- `valid-prefix` replays only the complete validated prefix and emits an explicit warning.
  Corruption is never downgraded to a warning.
- `fast` replays validated commands and reports the final digest without comparing logged outcome
  evidence after every command.
- `verify` also compares committed/rejected classification, rejection reason, event count, and
  event digest for every command, with periodic and final whole-engine invariants.
- `diagnostic` performs invariants after every command and stops at the first divergence, reporting
  the command, record offset, logged expected metadata, and actual result metadata.

Because V1 records store only an event count and digest, diagnostic replay cannot display or
field-diff the expected event body from the log alone. It may display the actual recomputed events
and identify a count/digest mismatch, but any report that claims an expected field-level event
difference requires a separately retained exact event transcript.

Replay reports contain no elapsed times, wall-clock timestamps, host paths, pointer values, or
unordered iteration output.

### JSON reports and exit status

Machine-readable inspection/repair output uses schema `ATLAS_LOG_REPORT_V1`. Machine-readable
replay output uses `ATLAS_REPLAY_REPORT_V1`. The exact required keys, nullability, decimal-string
rules, and stable field order are frozen in the format reference.

All `u64`, `i64`, record-count, and byte-offset values are JSON strings containing canonical
decimal text. Digests are lowercase hexadecimal. Log IDs are 32 lowercase hexadecimal characters.
Optional structured warning, error, divergence, and record-list fields are explicit JSON `null`
when absent. Reports never include elapsed timing.

Command exit codes are:

- 0: clean success, including `valid-prefix` replay that explicitly reports its torn-tail warning;
- 1: invalid data, corruption, strict torn-tail rejection, or replay divergence;
- 2: command-line usage error; and
- 3: operational I/O failure.

## Alternatives considered

- Logging after engine commit was rejected because a crash or append failure could leave semantic
  state with no authoritative record.
- Logging only accepted commands was rejected because rejected commands consume global sequences
  and are part of deterministic replay evidence.
- Logging complete event payloads was deferred because the canonical event digest already verifies
  exact replay and full payloads materially increase log volume. This choice creates the explicit
  diagnostic limitation above.
- Native structs and host byte order were rejected because padding, enum representation, `size_t`,
  and endianness are not portable contracts.
- A checksum over payload only was rejected because record lengths, version, sequence, outcome, and
  digest metadata also require corruption detection.
- Treating any invalid final record as a torn append was rejected because it would silently discard
  complete corruption.
- In-place truncation was rejected because repair must preserve the original evidence.
- Continuing after a persistence error was rejected because a visible complete record may exist
  even when durability confirmation failed.
- Continuing after an impossible post-WAL commit mismatch was rejected because the process could
  no longer prove whether in-memory state matches its authoritative log. The adapter returns a
  sticky `state_not_recoverable` result solely to force recovery; it is not a recoverable
  continuation path.
- Using CRC32C as an authentication claim was rejected because an attacker can recompute it.

## Consequences

The authoritative history and in-memory engine share one sequence and one prepare/commit boundary.
A crash after a complete record but before engine publication is repaired by replaying the record.
A write failure before publication leaves the engine untouched and forces recovery rather than
allowing the writer to continue from an ambiguous prefix.

The format is deliberately conservative: fixed-width big-endian fields, exact payload sizes,
bounded allocation, per-record checksums, and two-pass replay cost additional code and I/O. These
are correctness and inspectability costs, not performance claims.

Changing any header or record byte, tag meaning, checksum coverage, or configuration-digest input
requires a new format or record version. Changing matching or event semantics requires a separate
semantic-version decision. Neither change may silently reinterpret V1 bytes.

The log is not sufficient for field-level expected-event diagnostics. Exact event bodies remain an
optional external evidence artifact rather than an implied property of `ATLSLG01`.

## Evidence

PR2 acceptance requires:

- independent golden bytes for empty headers and all three command records;
- configuration-digest and CRC32C vectors, including `123456789`;
- raw invalid enum, optional-price, signed-price, maximum-ID, and maximum-quantity records;
- every-byte truncation classification for header and records;
- length-bomb, overflow, unsupported-version/type, checksum, payload, gap, duplicate, semantic, and
  catalog mismatch tests;
- append, partial-write, flush, and sync fault injection proving no core commit and sticky session
  poisoning;
- compile-time no-throw commit evidence plus a post-commit sequence/count/digest/classification
  equivalence guard that poisons the session on any impossible returned mismatch;
- clean-log refusal, torn-tail inspection, and non-overwriting repair tests;
- fast, verify, and diagnostic replay with both tail policies;
- byte-identical reports and final digests across repeated verified replay; and
- compiler, sanitizer, formatting, and fuzz-smoke gates.

The implementation and local evidence are complete. Hosted Clang, sanitizer, libFuzzer, pull
request, and sequential merge gates must still succeed before PR2 is recorded as fully green.
