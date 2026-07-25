# 2026-07-25 - Phase 4 snapshot and recovery implementation

## Outcome

The Phase 4 PR3 persistence contract is frozen, implemented, and integrated on `main` through PR
#6. Its
authoritative design references are
[ADR 0014](../decisions/0014-persisted-snapshots-and-log-suffix-recovery.md) and the
[byte-level snapshot reference](../snapshot-format.md).

The 60-case focused recovery selection reports 59 passes plus one expected Windows
canonical-symlink skip. GCC Debug and Release each pass 482/482 CTest cases, the production-only
build passes, the 288-test non-campaign and 11-test marked Python selections pass, and the CLI
process-boundary script passes with Unicode snapshot paths and LF-only versioned JSON. Hosted
compiler, sanitizer, retained libFuzzer smoke, pull-request, and final push-to-main gates also
pass.

## Starting point

PR3 follows the Phase 4 router and command-log/replay slices. Those slices provide:

- the canonical `EngineSnapshot` and `ATLSME01` digest;
- one global sequence and active-order directory;
- an exclusive preparation/commit boundary;
- the `ATLSLG01` append-only authoritative history;
- bounded scanning, strict/valid-prefix tail policy, and deterministic replay; and
- the `ATLSCF01` configuration identity and opaque 16-byte log ID.

PR3 must preserve semantic version 6 and every frozen Phase 0 through Phase 3 encoding. It must not
modify `ATLSLG01`, log-only `ATLAS_REPLAY_REPORT_V1`, or the independent Python oracle boundary.

## Frozen byte contract

`ATLSSN01` V1 uses fixed-width big-endian values and no native layout:

- a 169-byte fixed header;
- the exact 28-byte `ATLSLG01` catalog entry encoding;
- one 36-byte-base instrument block per configured instrument;
- 32-byte-base nonempty price-level blocks;
- fixed 41-byte full order entries;
- explicit total, header, catalog, instrument-section, instrument-block, and level-block lengths;
- one `ATLSCF01` configuration digest and one reconstructed `ATLSME01` state digest; and
- one whole-file CRC32C excluding only the trailing four-byte checksum.

The default decode limit is 256 MiB. Only the snapshot API may explicitly raise that limit.

Each order repeats its instrument, side, and level price. Counts and aggregates are encoded and
recomputed. Duplicate or inconsistent values are rejected rather than normalized.

## Recovery boundary

The snapshot stores the authoritative covered sequence and exact log byte offset directly. This is
required because a rejected command consumes a sequence without changing resting state.

Snapshot-plus-suffix recovery must prove:

- semantic, log-ID, catalog, capacity, and configuration-digest compatibility;
- the covered record ends at the stored offset;
- sequence zero ends at the exact log-header boundary;
- the first complete suffix record is exactly `covered_sequence + 1`; and
- the snapshot is not newer than the selected valid log prefix.

Restoration is all-or-nothing. It decodes into temporary values; reserves storage, local indexes,
and both coordinator directories; allocates every level, node, local index, active-ID entry, and
active-priority entry before linking; reconstructs FIFO links without further allocation; restores
sequence state last; runs full invariants; and recomputes the state digest before returning a new
engine. The reverse `Sequence -> OrderId` directory removes the former pairwise global-priority
check; invariant validation now performs a linear number of visits and directory lookups.

## Publication and discovery boundary

The final name is:

```text
atlaslob-<32-lowercase-hex-log-id>-<20-digit-sequence>.snapshot
```

Publication synchronizes the log through the captured boundary, writes and synchronizes a unique
same-directory temporary file, closes and rereads it, fully validates it against the captured
source, and atomically renames without replacement. Existing snapshots are never overwritten.

Directory discovery considers exact canonical regular-file names for the selected log ID in parsed
sequence order from newest to oldest. It fully validates every considered candidate and records why
matching newer candidates were skipped. If no valid candidate remains, it falls back to the
authoritative full log with an explicit warning.

An explicit snapshot path does not silently fall back. Snapshot options remain mutually exclusive.
Canonical symlinks are inspected with non-following status and never opened through their targets.
Candidate-local type/status/read/codec/restore failures are recorded and skipped; directory
enumeration failure remains terminal.

Clean full-log, explicit-snapshot, and directory-snapshot recovery can return a writable
`LoggedEngine`. Recovery retains one read source, requires a clean tail, then reopens only the
exact validated extent with `_O_APPEND`/`O_APPEND` and no create or truncate. A torn tail remains
non-writable under both tail policies. `valid-prefix` can still reconstruct standalone state, but
copy-only repair to a distinct clean log is required before resuming submission.

## Report compatibility

Standalone inspection uses `ATLAS_SNAPSHOT_REPORT_V1`.

Log-only replay retains byte-identical `ATLAS_REPLAY_REPORT_V1`. Snapshot-aware replay uses
`ATLAS_REPLAY_REPORT_V2` so selected recovery source, covered/replayed counts, and deterministic
candidate-skip diagnostics do not alter PR2's frozen schema.

Reports contain canonical decimal strings for fixed-width values, lowercase hexadecimal IDs and
digests, no host paths, and no elapsed times.

## Implemented surfaces

- [x] Public bounded snapshot values and stable error vocabulary.
- [x] Checked `ATLSSN01` encoder/decoder and a reviewed fixed golden byte vector.
- [x] Private all-or-nothing core restore seam.
- [x] Bulk-staged node, level, local-index, active-ID, active-priority, and sequence reconstruction.
- [x] Unique synchronized publication with no-replace rename and filesystem fault injection.
- [x] Standalone snapshot inspection and deterministic reports.
- [x] Explicit snapshot and newest-to-oldest directory discovery.
- [x] Candidate-local I/O handling and non-followed canonical symlinks.
- [x] Log-boundary validation and suffix replay in fast, verify, and diagnostic modes.
- [x] Clean-tail exact-extent `LoggedEngine` recovery and torn-tail writable refusal.
- [x] Snapshot-aware `ATLAS_REPLAY_REPORT_V2` while preserving log-only V1.
- [x] Snapshot decoder fuzz target, retained canonical seed, and deterministic
  truncation/corruption tests.
- [x] Documentation, evidence, roadmap, changelog, and semantic-contract updates.

## Validation status

- [x] Empty-engine restoration and exact sequence-zero log-header boundary.
- [x] Reviewed byte-exact populated multi-instrument golden with both sides, several FIFO orders,
  and an empty configured instrument.
- [x] Snapshot covering a rejected command.
- [x] Exhausted-sequence codec and core restoration.
- [x] Every-byte truncation and single-bit corruption cases.
- [x] Length, count, offset, block, arithmetic, and configured-bound failures.
- [x] Duplicate instrument/order/priority and invalid hierarchy/sorting/crossed-book cases.
- [x] Canonical field/entry offsets across decode, host conversion, and encode validation.
- [x] Wrong aggregate/count/capacity/configuration/state-digest cases.
- [x] Restoration allocation and invariant failure atomicity.
- [x] Temp create/write/flush/sync/close/reread/verify/rename/cleanup faults.
- [x] Existing-final refusal and previous-good-snapshot preservation.
- [x] Filename/content sequence disagreement, newer-invalid/older-valid directory selection, and
  all-invalid full-log fallback.
- [x] Exact boundary, log identity, strict/valid-prefix tail policy, and explicit-corruption
  failures.
- [x] Full-log versus snapshot-plus-suffix state, next-sequence, subsequent-event-digest, and
  invariant equivalence.
- [x] 1,024-order bulk restore and all 26 injected restore allocation boundaries.
- [x] Focused local gate: 60 selected cases, with 59 passes and one expected Windows
  canonical-symlink skip.
- [x] Unicode-path and LF-only CLI process-boundary script.
- [x] GCC Debug and Release: 482/482 CTest cases in each configuration.
- [x] Production-only build and both Python selections.
- [ ] Hosted Clang.
- [ ] ASan/UBSan and actual decoder libFuzzer smoke.
- [ ] Pinned formatting, `git diff --check`, and machine-path/secret scan.
- [ ] Pull-request publication and merge.

## Claim boundary

This record claims only the implemented working-branch surfaces and focused local tests listed
above. It does not claim a completed full/hosted gate, merge, authentication, encryption,
replication, concurrent writers, distributed recovery, log rotation, pruning, in-place repair,
cross-platform package publication, benchmark results, universal power-loss durability, or
production readiness.
