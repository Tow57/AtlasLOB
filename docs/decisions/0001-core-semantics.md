# ADR 0001: Initial core semantic boundaries

- Status: accepted
- Date: 2026-07-22

## Context

Mutable order-book structures are unsafe to implement before legal inputs, residual behavior, and
priority rules are explicit. Ambiguous rules would make tests depend on accidental implementation
behavior.

## Decision

- Use strong C++ value types for identifiers, prices, quantities, and sequences.
- Represent prices as positive integer ticks inside the core.
- Accept limit GTC, limit IOC, and market IOC in the initial vocabulary.
- Reject market GTC before mutation.
- Reserve FOK in the vocabulary but reject it until a non-mutating liquidity preflight is verified.
- Execute future trades at the resting order's price.
- Give future replacements new time priority using cancel-and-new semantics.
- Keep the deterministic core single-writer and free of I/O concerns.

## Alternatives considered

- Floating-point prices were rejected because equality, ordering, serialization, and replay must be
  exact.
- A fake sentinel price for market orders was rejected because it weakens type and invariant
  reasoning.
- Immediate FOK implementation was rejected because it adds a second liquidity traversal before
  the ordinary matching loop has independent correctness evidence.

## Consequences

Boundary adapters must perform checked conversion into strong values. Some vocabulary is available
before execution support, so public status documentation must distinguish recognized from
implemented behavior.

## Evidence

- `atlas_domain_tests` checks type separation and deterministic validation results.
- `atlas_cli validate-demo` demonstrates an accepted normalized limit order.
