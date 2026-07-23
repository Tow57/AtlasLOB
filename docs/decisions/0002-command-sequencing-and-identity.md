# ADR 0002: Command sequencing and active-order identity

- Status: accepted
- Date: 2026-07-22

## Context

Cancel, replace, replay, and normalized events require one authoritative command order and an
explicit definition of order-ID uniqueness. Leaving either policy to a future adapter would make
the same command stream produce different priority and rejection behavior.

## Decision

- Client commands do not provide the authoritative engine sequence.
- A future engine assigns one monotonically increasing, nonzero sequence to every well-formed
  command admitted to domain processing.
- Domain rejects consume a sequence. Text, protocol, and numeric-conversion parse failures happen
  before domain processing and do not.
- A resting new or replacement order uses its accepted command sequence as priority sequence.
- Sequence wraparound is an internal capacity failure; it never silently rolls over.
- Order IDs are unique while active. An ID may be reused only after its previous order becomes
  terminal; no terminal-ID cache is retained in the baseline.
- Cancel and replace commands carry client and instrument identity. Future stateful validation must
  require both to match the active order.
- Replace requires a distinct new order ID and resets priority through cancel-and-new semantics.
- Normalized events contain deterministic semantic values only. They exclude addresses, container
  handles, wall-clock timestamps, and adapter-specific data.
- The event variant alternative is the sole event-type discriminator; the common header does not
  duplicate it. A present best bid or ask couples price and aggregate quantity in one value.

## Alternatives considered

- Client-assigned priority sequences were rejected because separate adapters could disagree about
  ordering or supply gaps and duplicates.
- Assigning sequences only to accepted commands was rejected because domain rejections would lack a
  stable audit position relative to accepted commands.
- Never reusing an order ID during an engine lifetime was deferred because it requires an
  ever-growing consumed-ID set or an explicit retention policy.
- Allowing replace to retain the old ID was rejected because a distinct ID makes lineage and
  priority reset explicit.

## Consequences

The future engine must assign sequence before state-dependent validation but after representation
and conversion checks. Domain fixtures may validate command shape without assigning a sequence and
must label that output as validation rather than execution. Replay and event comparison can use
command sequence plus event index as a stable ordering key.

## Evidence

- `atlas_domain_tests` covers command variants, explicit enum validity, validation precedence, and
  the complete normalized event vocabulary.
- `atlas_cli domain-fixture <path>` distinguishes parse failures from domain validation results.
