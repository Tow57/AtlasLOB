# ADR 0014: Persisted snapshots and log-suffix recovery

- Status: accepted
- Date: 2026-07-25
- Implementation state: the Phase 4 PR3 contract and implementation are complete on the working
  branch. The 60-case focused recovery selection reports 59 passes plus one expected Windows
  canonical-symlink skip. GCC Debug and Release each pass 482/482 CTest cases, the production-only
  build passes, the 288-test non-campaign and 11-test marked Python selections pass, and the CLI
  process-boundary check passes. Hosted Clang, ASan/UBSan, actual Clang libFuzzer execution,
  pull-request, and merge gates remain pending.

## Context

ADR 0012 establishes one canonical `EngineSnapshot`, a global active-order directory, and one
authoritative command sequence across every instrument. ADR 0013 makes the append-only
`ATLSLG01` command log authoritative and freezes replay, corruption, repair, and reporting
behavior.

Replaying an entire valid log is sufficient for correctness, but recovery should not have to
re-execute every historical command. A persisted snapshot can provide a checked engine image at
one exact command-log boundary, after which recovery executes only the suffix. That image must
retain:

- the immutable catalog and engine capacity policy;
- the authoritative sequence even when the covered command was rejected;
- every resting order in exact best-price and FIFO order;
- enough redundant identity, side, price, count, and aggregate information to reject inconsistent
  hierarchy rather than infer around it;
- the log identity and byte boundary to which the image belongs; and
- deterministic configuration and state evidence that can be recomputed before publication.

A snapshot is derived evidence, not a second authoritative history. The command log remains the
source of truth. A damaged snapshot must be rejectable or skippable without modifying it or
preventing full-log recovery.

CRC32C detects accidental corruption. It does not authenticate a snapshot or log, establish writer
identity, or defend against intentional modification.

## Decision

### Ownership and compatibility boundary

The existing `AtlasLOB::persistence` target owns snapshot encoding, bounded decoding, publication,
inspection, discovery, and snapshot-plus-suffix recovery. The domain and deterministic core remain
independent of filesystems and persistence codecs.

The coordinator retains its active-ID directory and also owns a private
`Sequence -> OrderId` active-priority directory. The two directories are staged and committed
together for ordinary commands and rebuilt together during restoration. Whole-engine validation
walks the books and both directories with a fixed number of directory lookups per active order,
instead of comparing every priority with every other priority. This is an invariant-design
property, not a throughput or latency claim.

The canonical snapshot format is `ATLSSN01`, format version 1. Its exact byte layout is frozen in
[the snapshot format reference](../snapshot-format.md). All multibyte values are fixed-width
big-endian, signed ticks use their 64-bit two's-complement representation, and no native object
layout, padding, pointer, timestamp, path, or unordered iteration enters the file.

Semantic version remains 6. `ATLSLG01`, `ATLSCF01`, `ATLSME01`, `ATLSST01`, `ATLSEV01`,
`ATLAS_LOG_REPORT_V1`, and log-only `ATLAS_REPLAY_REPORT_V1` remain byte-for-byte unchanged.

Snapshot inspection introduces `ATLAS_SNAPSHOT_REPORT_V1`. Snapshot-aware replay introduces
`ATLAS_REPLAY_REPORT_V2` because recovery source and skipped-candidate diagnostics cannot be added
to the frozen V1 report without changing it. Invoking `atlas_replay` without a snapshot option
continues to produce V1.

### Snapshot identity and log boundary

Every snapshot stores:

- the 16-byte opaque log ID copied from its command-log header;
- the covered global sequence;
- the exact byte offset immediately after the covered record;
- the sequence-exhausted state;
- global and per-instrument configuration;
- global and per-instrument active counts;
- the `ATLSCF01` configuration digest; and
- the canonical `ATLSME01` state digest.

A snapshot may cover sequence zero. In that case it represents a fresh engine and its covered log
offset is exactly the end of the `ATLSLG01` header. For a positive covered sequence, the offset is
exactly the end of the record carrying that sequence. The authoritative sequence is stored
directly because a rejected command changes sequence state without changing any resting order.

Pairing a snapshot with a log requires exact semantic version, log ID, catalog, capacity,
configuration digest, covered sequence, and covered offset agreement. Recovery scans record
framing and checksums through the recorded boundary without executing the covered prefix. The first
record beginning at that boundary, if one exists, must carry exactly `covered_sequence + 1`.

An exhausted snapshot must cover `UINT64_MAX` and cannot have a complete suffix record. A
non-exhausted snapshot must cover a sequence below `UINT64_MAX`; its restored next sequence is
`covered_sequence + 1`, including one for a sequence-zero snapshot.

### Canonical hierarchy

The fixed header is 169 bytes before the catalog. Each catalog entry is the exact 28-byte encoding
used by `ATLSLG01`. The header is followed by one instrument block for every catalog entry and one
whole-file CRC32C.

Instrument blocks are strictly ascending by instrument ID and exactly match the catalog. Bid
levels are strictly descending; ask levels are strictly ascending. Orders appear in exact FIFO
order.

The hierarchy deliberately repeats values:

- an instrument block stores its active-order count;
- a level stores its aggregate quantity and order count;
- every order stores instrument, side, and price even though its parents imply them; and
- the file stores both configuration and state digests.

Decoding must recompute and compare all repeated values. It may not repair, normalize, reorder, or
silently prefer one copy.

### Bounds and validation

The default accepted snapshot size is 256 MiB. The snapshot API may explicitly receive a larger
bound; the log header and record limits remain fixed. No declared length, count, offset, block
extent, allocation size, aggregate, or active count is used before checked conversion,
multiplication, and addition.

Validation order is:

1. fixed-prefix availability, declared total length, configured bound, and exact file extent;
2. whole-file CRC32C;
3. magic, format version, semantic version, and byte-order marker;
4. exact header/catalog/instrument/block length and count relationships;
5. catalog validity and `ATLSCF01` digest;
6. hierarchy, domain values, ordering, uniqueness, capacity, counts, aggregates, book state, and
   sequence state;
7. reconstructed `ATLSME01` digest; and
8. host-representation conversion and full engine invariants during restoration.

The catalog is nonempty, strictly ascending, and contains valid nonzero instrument IDs, positive
maximum quantities and tick increments, and canonical capacity values. Every configured
instrument, including an empty one, appears exactly once.

Every stored level is nonempty. Prices are positive, tick-aligned, and ordered best to worst. The
book is not crossed. Every order has nonzero order/client/instrument identity, recognized side,
positive remaining quantity no greater than the instrument policy, the parent instrument, the
parent side and price, and a nonzero priority no newer than the covered sequence. FIFO priorities
are strictly increasing within a level.

Order IDs and priority sequences are globally unique. Level aggregates equal checked sums of
remaining quantities. Level, instrument, and global active counts agree. Active counts do not
exceed their per-instrument or engine-wide capacities.

Sequence zero requires an empty engine and a non-exhausted state. Exhaustion is true exactly when
the covered sequence is `UINT64_MAX`. Duplicate instruments, levels, orders, or priorities; empty
levels; zero quantities; wrong hierarchy; invalid price order; a crossed book; wrong
aggregates/counts; priorities beyond the covered sequence; incompatible configuration; and
inconsistent exhaustion are invalid snapshot schema.

Snapshots have no repairable-tail policy. Truncation, extra trailing bytes, a bad checksum, or any
complete structural inconsistency invalidates the file.

Every snapshot error carries a deterministic byte offset. Fixed-header errors point to the field
offset in the V1 header. Catalog, instrument, level, and order errors point to the first offending
entry or the specific repeated field whose value is inconsistent. A duplicate points to the later
occurrence; a crossed book points to the ask price that first violates the uncrossed rule; an
aggregate disagreement points to the stored aggregate; and trailing hierarchy data points to the
first unexpected byte before the CRC. The byte-level reference freezes the general formulas and
representative golden offsets. Pairing failures without one corresponding snapshot byte use offset
zero; covered-boundary failures carry the stored log offset.

### All-or-nothing restoration

Recovery never mutates an already published engine. It:

1. decodes into bounded value-only temporary structures;
2. completes every validation above;
3. creates a private temporary multi-instrument engine;
4. reserves each book's owning storage and active index plus both coordinator directories;
5. allocates every side level;
6. allocates every stable order node and inserts every per-book index, active-ID entry, and
   active-priority entry before any intrusive FIFO link is published;
7. links the already allocated nodes in persisted FIFO order without further allocation and marks
   each staged book complete;
8. restores the authoritative sequence and exhaustion state only after linkage and indexing are
   complete;
9. runs complete local and whole-engine invariants, reproduces the exact snapshot, and recomputes
   the state digest; and
10. returns or publishes the new engine only after every check succeeds.

Any allocation, conversion, codec, invariant, or I/O failure destroys only temporary state.
No partially restored engine, book, index, level, node, directory, or sequence becomes observable.
Allocation failure remains an operational resource failure rather than a domain rejection.
The restore fault seam covers engine creation, both global reserves, every per-book reserve,
level allocation, node allocation, local indexing, and both global insertions.

### Snapshot publication

Snapshot publication is serialized with submissions on the same single-writer persistence
session. A poisoned session cannot publish a snapshot.

The covered log is flushed and synchronized through the covered record regardless of the
session's ordinary durability mode. Failure of that log synchronization poisons the session,
matching ADR 0013's authoritative-log boundary.

The canonical final name is:

```text
atlaslob-<32-lowercase-hex-log-id>-<sequence-as-20-decimal-digits>.snapshot
```

The writer:

1. captures the engine snapshot and exact current log-end boundary;
2. synchronizes the log through that boundary;
3. encodes the complete snapshot;
4. creates one uniquely named temporary regular file in the destination directory;
5. writes, flushes, synchronizes, closes, reopens, and fully validates that temporary file;
6. compares the reread log ID, boundary, configuration digest, state digest, and snapshot values
   with the captured source; and
7. atomically renames without replacement to the unique final name.

An existing final path is never overwritten. A temporary-output failure leaves all previous good
snapshots untouched and is not itself an engine mutation. Temporary names do not match the
canonical discovery pattern and are ignored. Atomic visibility and synchronization are described
only in terms of the platform operations that returned success; Phase 4 makes no universal
hardware or power-loss durability claim. If the newest name is not retained after a crash, recovery
can use an older valid snapshot or the authoritative log.

Cleanup failure is surfaced rather than hidden. A late platform failure can occur after the new
final name has become visible—for example, while removing a linked temporary name or synchronizing
the containing directory. The result therefore reports whether the new final is visible even when
it also carries an I/O error. Such a file was fully reread and validated before publication, but
the failed operation is not reported as a successful publication. No prior canonical snapshot is
replaced in either case.

### Discovery and snapshot-plus-suffix recovery

An explicit `--snapshot` path must decode fully and pair with the selected log. Invalid data fails
recovery; an operational read failure is reported as I/O.

`--snapshot-dir` is opportunistic:

- only ASCII names with the exact canonical shape and selected log ID enter candidate handling;
- discovery uses non-following file status: a matching symlink is never opened through its target
  and is recorded as a candidate-local failure;
- a matching candidate must itself be a regular file;
- candidates are ordered by the parsed numeric sequence, newest to oldest, never by modification
  time or directory enumeration order;
- the filename sequence is not trusted and must equal the decoded covered sequence;
- each candidate is fully decoded, validated, and paired with the log boundary before selection;
- an invalid or unreadable candidate is recorded with its sequence, category, and offset, then the
  next older candidate is considered; and
- if no valid candidate exists, recovery falls back to full-log replay and reports the skipped
  candidates rather than making a derived artifact authoritative.

Failure to enumerate the requested directory is an operational I/O failure. Invalid candidate
names and unrelated log IDs are ignored rather than treated as snapshots. Skipping one or more
matching candidates produces a warning even when an older snapshot or full-log replay succeeds.
Candidate-local type, status, open, read, extent, codec, compatibility, or restoration failures
are recorded and do not terminate directory recovery.

The command log is always scanned under the selected strict or valid-prefix tail policy before a
snapshot is published as recovered state. Strict mode rejects a torn tail. Valid-prefix mode may
recover the complete prefix with a warning, but it never suppresses corruption. A selected
snapshot must end at or before the validated prefix boundary.

Fast, verify, and diagnostic modes retain their ADR 0013 meanings for every suffix record. Verify
compares recorded outcome evidence for the suffix. Diagnostic additionally checks invariants
immediately after restoration and after every suffix command. The covered prefix is structurally,
checksum-, and digest-checked but not semantically re-executed; its state evidence is the fully
validated snapshot and `ATLSME01` digest.

All scans for one recovery use one retained `LogSource` and compare validated source identity
between passes. Boundary retention is proportional to the explicit snapshot or canonical
directory candidates, not to the number of log records.

### Writable recovery sessions

`LoggedEngine::recover`, `recover_from_snapshot`, and `recover_from_snapshot_directory` turn a
successful clean recovery into a new writable persistence session. They:

1. retain one read source while validating and replaying;
2. require a clean tail even when the requested standalone replay policy is `valid-prefix`;
3. require the recovered header and final validated extent;
4. reopen the path without create or truncate, using `_O_APPEND` on Windows or `O_APPEND` on
   POSIX;
5. reject the reopen if the descriptor extent is not exactly the validated extent; and
6. mark the existing log as already published so later session destruction cannot remove it.

The existing single-process, one-logical-writer assumption still applies. A missing path is never
created by recovery. A torn tail can be inspected or reconstructed as a standalone engine with
`valid-prefix`, but neither strict nor valid-prefix policy returns a writable `LoggedEngine` for
that file. Safe `repair-tail` must first copy the valid prefix to a distinct new clean log; that
new log can then be recovered as writable. Complete corruption is never resumable or repairable.

### Inspection, reports, and exit status

The interfaces become:

```text
atlas_inspect snapshot <path> [--json]

atlas_replay <log> [--snapshot <path>|--snapshot-dir <dir>]
                   [--mode fast|verify|diagnostic]
                   [--tail-policy strict|valid-prefix]
                   [--json]
```

Snapshot selection options are mutually exclusive. Snapshot inspection fully decodes the file and
recomputes its configuration and state evidence, but cannot validate a log boundary without a log.

Snapshot errors use stable categories and offsets frozen in the format reference. Human and JSON
reports contain no elapsed times or host paths. Snapshot-aware replay records its recovery source,
selected boundary, suffix count, and every skipped matching candidate.

Exit codes remain:

- 0: successful inspection or recovery, including explicit warnings for valid-prefix replay,
  skipped directory candidates, or full-log fallback;
- 1: invalid snapshot/log data, corruption, strict torn-tail rejection, incompatibility, or replay
  divergence;
- 2: command-line usage error; and
- 3: terminal operational I/O failure that prevents inspection or recovery. A candidate-local
  directory read failure that is explicitly skipped and followed by successful recovery remains a
  warning under exit 0.

## Alternatives considered

- Inferring the covered sequence from active priorities was rejected because a rejected command
  advances the sequence without adding an order.
- Omitting the log byte offset was rejected because sequence alone does not bind restoration to one
  exact append boundary.
- Persisting native objects or intrusive pointers was rejected because addresses, padding,
  allocator state, and container representation are neither portable nor valid after restart.
- Omitting repeated instrument, side, price, aggregate, and count values was rejected because the
  decoder would have less evidence for hierarchy corruption and would need to infer missing state.
- Restoring through ordinary command execution was rejected because it would regenerate history,
  matching, and priority rather than reconstruct the exact covered state.
- Publishing before reread verification was rejected because a successful write call alone does
  not prove the file can be decoded into the captured state.
- Overwriting a fixed snapshot name was rejected because failure could destroy the last good
  recovery point.
- Selecting by modification time was rejected because timestamps are mutable, platform-dependent,
  and outside deterministic evidence.
- Failing directory recovery on the first damaged snapshot was rejected because snapshots are
  derived accelerators; an older valid snapshot or full authoritative log remains safe.
- Extending `ATLAS_REPLAY_REPORT_V1` in place was rejected because PR2 freezes its exact fields and
  deterministic writer order.
- Treating CRC32C or SHA-256 digests as authentication was rejected because an intentional writer
  can recompute them.

## Consequences

Recovery can rebuild exact price-time state without re-executing the covered prefix, while the log
continues to define authoritative history. Snapshot creation and restoration require full-state
traversal, redundant encoding, checked allocations, synchronization, and invariant work. These are
correctness and recovery costs, not throughput or latency claims.

Changing any `ATLSSN01` field, width, tag, order, block-length rule, checksum coverage, or hierarchy
meaning requires a new snapshot format version. Changing matching or event semantics requires a
separate semantic-version decision. Neither change may silently reinterpret V1 bytes.

Snapshot-aware tooling has a separate report version so existing log-only evidence remains stable.
A directory can contain damaged newer candidates without preventing safe recovery, but every
matching skipped candidate remains visible in deterministic diagnostics.

## Evidence

The implemented local evidence includes:

- one reviewed byte-exact populated `ATLSSN01` golden plus exact round-trip coverage for empty
  configured instruments, rejected-command boundaries, and the only valid exhausted state;
- every-byte truncation and single-bit corruption checks, deterministic checksum-first
  classification, configured-bound enforcement, and malformed length/count bomb rejection;
- catalog, hierarchy, sorting, tick, quantity, aggregate, count, capacity, crossed-book, sequence,
  exhaustion, configuration-digest, and state-digest rejection cases;
- exact FIFO, identity, quantity, priority, index, directory, sequence, and digest reconstruction,
  including a 1,024-order bulk restore, the reverse active-priority directory, and deterministic
  allocation failure at all 26 injected restoration allocation boundaries;
- log-sync and temporary create/write/flush/sync/close/reread/verification/rename/cleanup failure
  injection, surfaced cleanup failure, no-overwrite publication, and preservation of an older good
  snapshot;
- explicit and directory selection, filename/content sequence checks, newer-invalid/older-valid
  selection, candidate-local I/O, non-followed canonical symlinks, all-invalid full-log fallback,
  exact log-boundary enforcement, and strict versus valid-prefix tail behavior;
- clean full-log, explicit-snapshot, and directory-snapshot recovery followed by new appends and
  snapshots, plus refusal to reopen torn or missing logs;
- full verified-log versus snapshot-plus-suffix equivalence for final state, next sequence,
  invariants, and the next submitted command's event digest;
- exact field, hierarchy, digest, value-entry, and trailing-byte error offsets; and
- deterministic `ATLAS_SNAPSHOT_REPORT_V1` and exact JSON/text `ATLAS_REPLAY_REPORT_V2` rendering
  without host paths, plus a passing Unicode-path/LF-only CLI process-boundary check.

The selected recovery gate contains 60 cases: 11 core restoration, 21 codec, 22
publication/recovery/inspection/report, two existing-append sink, and four exact V2 report
goldens. It reports 59 passes plus one expected Windows canonical-symlink skip. The complete GCC
Debug and Release suites each pass 482/482 CTest cases; the production-only build, 288-test
non-campaign Python selection, and 11-test marked Python selection also pass. The snapshot decoder
libFuzzer target and retained canonical seed are implemented, but an actual Clang libFuzzer run
remains pending. Hosted Clang, ASan/UBSan, pull-request, and merge gates also remain pending. The
reviewed golden is a checked literal; this ADR does not mislabel it as an independently generated
implementation.
