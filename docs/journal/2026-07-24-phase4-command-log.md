# 2026-07-24 - Phase 4 command-log and replay implementation

## Outcome

The Phase 4 PR2 persistence contract is frozen and implemented in
[ADR 0013](../decisions/0013-command-log-and-replay.md) and the
[byte-level format reference](../command-log-format.md).

The implementation on `codex/phase4-command-log` passes the local Debug, Release,
production-only, persistence, Python, typing, linting, and formatting gates recorded below. It was
subsequently rerun through the hosted compiler, sanitizer, decoder-fuzz, Python, differential,
wheel, and formatting gates and squash-merged through PR #8 as `f5c2f11`.

## Starting point

PR2 branches from the local Phase 4 router commit `dd9bcc2`. That commit supplies the exclusive
multi-engine preparation token, global sequence, active-order directory, immutable catalog, and
canonical event/state evidence needed by write-ahead persistence.

The router slice has evidence recorded separately and was integrated first through PR #7. PR2
preserves semantic version 6 and every frozen Phase 0 through Phase 3 encoding while adding a
separate persistence format.

## Frozen byte contract

`ATLSLG01` V1 uses fixed-width big-endian values and no native layout:

- the header is exactly `96 + 28 * catalog_count` bytes;
- catalog entries are 28 bytes in ascending instrument-ID order;
- the header carries one opaque 16-byte log ID, first sequence 1, canonical capacities, an
  `ATLSCF01` configuration digest, and CRC32C;
- records have a 66-byte fixed envelope including CRC;
- New, Cancel, and Replace records are exactly 102, 82, and 106 bytes;
- raw New enum bytes and an explicit optional-price slot preserve semantically invalid commands;
- every record stores classification, rejection reason, event count, and the canonical
  `ATLSEV01` event digest; and
- per-header and per-record CRC32C excludes only the trailing checksum field.

CRC32C uses reflected Castagnoli polynomial `0x82f63b78`, initial and final XOR values
`0xffffffff`, and check vector `123456789 -> 0xe3069283`. It is corruption evidence, not
authentication.

Ordinary session creation sources the opaque log ID from the C++ implementation's
`std::random_device`; no operating-system entropy guarantee is claimed. The low-level codec and
explicit C++ seam accept any 16-byte value for deterministic evidence. No UUID, cryptographic
uniqueness, or authentication property is claimed.

Default decoder bounds are 1 MiB for a complete header and 64 KiB for one record. The separate
snapshot slice will default to 256 MiB and may explicitly raise only its own bound.

## Write-ahead and failure boundary

The session order is:

```text
prepare -> encode -> append -> flush/sync policy -> engine commit
```

Accepted and rejected domain commands are records because both publish a global sequence.
Conversion failures, sequence-exhaustion attempts, and internal preparation failures are excluded
because they publish no sequence or event batch.

The three durability modes are `buffered`, `flush_each_record`, and `sync_each_record`, with
`sync_each_record` as the API default. Buffered mode makes no crash-durability promise. Flush and
sync names describe the platform calls attempted; they do not support a hardware, replicated, or
production durability claim.

Any partial write, append error, flush error, or sync error abandons the prepared engine token and
permanently poisons the session. Recovery from the authoritative log is required before another
submission. A complete record may be visible even when synchronization reports failure, which is
why continuing the old in-memory session is unsafe.

The core commit after successful write-ahead persistence is a no-throw operation with no expected
error result. If the adapter nevertheless detects a post-commit sequence, event, outcome, or
identity disagreement, it returns `state_not_recoverable`, permanently poisons the session, and
requires authoritative-log recovery. The returned error is a stop signal, not permission to
continue, because a durable record already exists and in-memory equivalence could not be proven.

## Scanner, repair, and replay boundary

The scanner performs bounded checked arithmetic and validates complete object checksums before
interpreting their variable data. It reports stable offset/category diagnostics.

Only EOF interrupting the final record is a torn tail. A checksum-complete invalid final record is
corruption. Safe repair copies a validated prefix to a distinct nonexistent output and never
modifies or overwrites the input.

Replay scans before engine mutation:

- `fast` reconstructs from structurally valid records;
- `verify` compares classification, rejection reason, event count, and event digest;
- `diagnostic` also checks invariants after every command and stops on the first difference; and
- `strict` rejects a torn tail while `valid-prefix` replays the complete prefix with a warning.

The log does not contain expected event bodies. Diagnostic replay can identify the divergent
record and compare logged metadata/digest with actual replay metadata/events, but it cannot
manufacture a field-level expected event body without a separate retained transcript.

Machine-readable inspection and replay reports use `ATLAS_LOG_REPORT_V1` and
`ATLAS_REPLAY_REPORT_V1`. Fixed-width values, counts, sequences, and offsets are canonical decimal
strings. Reports contain no elapsed times or host paths.

Exit codes are 0 for success, 1 for invalid/corrupt/divergent evidence, 2 for usage, and 3 for
operational I/O. Successful `valid-prefix` replay exits 0 only while retaining an explicit warning.

## Implemented surfaces

PR2 supplies:

- the separate `AtlasLOB::persistence` target and shared checked SHA-256 utility;
- checked big-endian header/record codecs, CRC32C, canonical configuration hashing, and reviewed
  golden bytes for all command variants and boundary values;
- `LoggedEngine`, which prepares, appends, applies its durability policy, and only then commits the
  no-throw core token;
- all-byte partial-write failure injection plus flush/sync failure and sticky-poison coverage;
- a bounded frozen-extent scanner with every-byte header/record truncation classification;
- clean-log refusal, new-file-only torn-tail repair, and complete-corruption refusal;
- fast, verify, and diagnostic replay with invariant checking, exact evidence comparison, and a
  validated-prefix SHA-256 guard against same-size changes between structural and semantic scans;
- a real file-backed workload replayed into two independent engines in verify mode, comparing
  exact JSON/text report bytes, counts, reconstructed snapshots/digests, and the events and state
  from a subsequent crossing command;
- deterministic JSON/text reports and direct CLI parser/exit-code tests;
- `atlas_inspect` and `atlas_replay`;
- header and record libFuzzer targets with canonical seed generation and a bounded hosted smoke
  job; and
- a strict V2 native-transcript follow-up that closes request/error continuity evidence gaps without
  changing any frozen V1 bytes.

## Local validation

- GCC Debug: 418/418 CTest cases passed, including all 85 persistence tests.
- GCC Release: 418/418 CTest cases passed.
- Production-only Release with `BUILD_TESTING=OFF`: build passed.
- Python non-campaign gate: 288 passed, two expected Windows symlink-privilege skips, 11
  campaign/fuzz cases deselected.
- Ruff formatting/lint and strict mypy: passed.
- Pinned clang-format: passed.
- `git diff --check` and machine-path/secret scan: passed.

The local MinGW installation does not provide the ASan/UBSan runtime libraries, and no local Clang
libFuzzer toolchain is installed. The repository therefore carries those as explicit hosted gates
rather than treating their local absence as success. Persisted snapshots/log-suffix recovery and
native Python bindings remain PR3 and PR4.

## Claim boundary

This slice defines an append-only single-writer recovery format. It does not claim authentication,
encryption, replication, concurrent writers, distributed locking, log rotation, pruning, in-place
repair, snapshot recovery, benchmark results, durability across every platform/storage failure,
or production readiness.
