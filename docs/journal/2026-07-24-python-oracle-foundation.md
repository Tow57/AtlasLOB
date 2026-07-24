# 2026-07-24 - Independent Python oracle foundation

## Outcome

The first Phase 3 slice now compares the public C++ matching engine with an independent Python
implementation across a real process boundary.

The C++ side is a `BUILD_TESTING`-only executable. It accepts raw fixed-width numeric commands,
preserves unknown enum representations for domain validation, and emits versioned JSON Lines with
complete events, public observers, canonical digests, and checkpoint snapshots. It links only the
public core/domain interfaces and is neither installed nor presented as a performance protocol.

The Python side is a typed Python 3.11+ package with a standard-library-only runtime. Its matching
model uses dictionaries, deques, and explicit price sorting. It independently implements frozen
validation precedence, sequencing, price-time matching, cancellation, replacement, normalized
events, final-state capacity, same-level `uint64` aggregate checks, snapshots, and invariants.

## Review corrections

The initial native adapter review found that allocation inside `noexcept` parsers could terminate,
thrown exceptions were being collapsed into synthesized engine results, stream formatting could
change decimal JSON, and output failures could be detected too late. The corrected adapter:

- catches parser resource/internal failures at the process boundary;
- keeps thrown engine/resource exceptions distinct from returned engine errors;
- writes integers with locale/stream-flag-independent `to_chars`;
- stops on write failure and returns the fatal process code; and
- belongs to both the integration and differential CTest labels.

The Python review found that configuration accepted floats/booleans and that ordinary container
allocation failure could occur after oracle mutation. Fixed-width configuration now requires
integer values and explicitly rejects booleans. Once sequencing has begun, any unexpected Python
internal exception poisons that engine instance; the runner must stop and never treat the partial
state as recoverable evidence.

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
- Local Python 3.12 strict mypy, Ruff, and 89-test gates pass after final review hardening.

This is correctness evidence, not a latency, throughput, memory, or production-readiness claim.

## Deferred

Seeded Hypothesis strategies, fixed corpus persistence, minimized failure artifacts, shrinker
demonstrations, long campaigns, and fuzzing remain later Phase 3 pull requests. Python bindings,
batch APIs, replay, and packaging remain Phase 4.
