# Phase 3 injected-fault regressions

These two-command fixtures are deterministic outputs from the semantic
shrinker. They prove that the differential runner detects three intentionally
altered evidence behaviors:

- `newest-at-price.atlas`: reverses two FIFO orders at one price;
- `incoming-trade-price.atlas`: reports the aggressor limit rather than the
  passive execution price; and
- `stale-partial-aggregate.atlas`: leaves the pre-trade level aggregate after a
  partial fill.

The faults exist only in the Python evidence-transformer test seam. The C++
matching engine and native adapter contain no injected bug branches.
