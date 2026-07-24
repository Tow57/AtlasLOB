# 2026-07-24 - Independent Python oracle foundation

## Outcome

The first Phase 3 slice now compares the public C++ matching engine with an independent Python
implementation across a real process boundary.

The C++ side is a `BUILD_TESTING`-only executable. It accepts raw fixed-width numeric commands,
preserves unknown enum representations for domain validation, and emits versioned JSON Lines with
complete events, public observers, canonical digests, and checkpoint snapshots. It links only the
public core/domain interfaces and is neither installed nor presented as a performance protocol.

The Python side is a typed internal Python 3.11-through-3.14 package with a
standard-library-only runtime. Its matching model uses dictionaries, deques, and explicit price
sorting. It independently implements frozen validation precedence, sequencing, price-time
matching, cancellation, replacement, normalized events, final-state capacity, same-level
`uint64` aggregate checks, snapshots, and invariants.

## Review corrections

The initial native adapter review found that allocation inside `noexcept` parsers could terminate,
thrown exceptions were being collapsed into synthesized engine results, stream formatting could
change decimal JSON, and output failures could be detected too late. The corrected adapter:

- catches parser resource/internal failures at the process boundary;
- keeps thrown engine/resource exceptions distinct from returned engine errors;
- writes integers with locale/stream-flag-independent `to_chars`;
- covers reads, snapshots, digests, serialization, writes, and terminal flushing with one fatal
  process boundary;
- never appends a second JSON record after a partial serialization failure;
- flushes the terminal record before selecting success; and
- belongs to both the integration and differential CTest labels.

The Python review found that configuration accepted floats/booleans and that ordinary container
allocation failure could occur after oracle mutation. Fixed-width configuration now requires
integer values and explicitly rejects booleans. Once sequencing has begun, any unexpected Python
internal exception poisons that engine instance; the runner must stop and never treat the partial
state as recoverable evidence. The final protocol review also bound native output to the submitted
request, sequence timeline, checkpoint cadence, exact event grammar, process exit, and terminal
state. It rejects malformed numeric JSON, duplicate fields, impossible snapshots, and internally
consistent records spliced from another run.

## Evidence

- Empty, representative, signed-price, and all-event Python encodings reproduce ADR 0009 hashes.
- An independently computed rejection digest replaces a tautological C++ assertion.
- Hand-derived tables cover New, Cancel, and Replace pure/state precedence, submitted instrument,
  relevant order identity, and atomic rejection state.
- Named process scenarios cover both sides, FIFO partials, multi-level sweeps, GTC/IOC/market
  residuals, cancellation, active-ID reuse, final-count capacity, aggregate overflow, and
  same-price/crossing/full-fill replacements.
- Each named command compares classification, sequence, every event/header/payload, event digest,
  public observers, state digest, and full snapshot.
- A separate read-only review spot-checked 10,000 mixed native/Python commands without a
  classification, sequence, event-digest, or state-digest divergence.
- The hardened request-bound decoder accepted a further 5,000-command state-aware exact stream
  containing 3,532 commits, 1,468 rejections, and 807 trades without divergence.
- Debug GCC, Release GCC, and Clang 19 ASan/UBSan each pass all 284 CTest cases; the sanitizer run
  includes the fixed-seed stress suites and reports no sanitizer diagnostic.
- Local Python 3.12 strict mypy, Ruff, and 109-test gates pass after final review hardening.
- CI covers Python 3.11 through 3.14 and separately builds, inspects, installs, and imports a
  normal wheel without development dependencies.

This is correctness evidence, not a latency, throughput, memory, or production-readiness claim.

## Deferred

Seeded Hypothesis strategies, fixed corpus persistence, minimized failure artifacts, shrinker
demonstrations, long campaigns, and fuzzing remain later Phase 3 pull requests. Native Python
bindings, batch APIs, replay, and production-facing distribution packaging remain Phase 4; the
current pure-Python package is only the internal correctness oracle.
