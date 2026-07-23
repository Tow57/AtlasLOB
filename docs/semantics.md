# Semantic Contract v0.1

This document defines the behavior implemented by the current domain-validation slice. Matching
semantics will be added before mutable order-book behavior is implemented.

## Strong values

`OrderId`, `InstrumentId`, `Sequence`, `PriceTicks`, and `Quantity` are distinct C++ types. Zero is
an invalid client-supplied ID or quantity and is rejected by validation. Prices use integer ticks;
floating-point prices are excluded from the core API.

## Supported order combinations

| Order type | Time in force | Current behavior |
| --- | --- | --- |
| Limit | GTC | Accepted when it has a positive limit price and quantity. |
| Limit | IOC | Accepted when it has a positive limit price and quantity. |
| Market | IOC | Accepted without a limit price. Residual quantity will never rest. |
| Limit or market | FOK | Vocabulary reserved; rejected as unsupported in this release. |
| Market | GTC | Rejected as an invalid combination. |

## Deterministic validation order

When several fields are invalid, the first applicable rule determines the stable rejection reason:

1. Nonzero order ID.
2. Nonzero instrument ID.
3. Positive quantity.
4. Known side, order type, and time-in-force values.
5. Supported time in force.
6. Order-type-specific price requirements.
7. Order-type and time-in-force compatibility.

Boundary adapters must validate enum and integer conversions before constructing these domain
values. The core validation function does not log, mutate state, read the clock, or throw.

## Matching rules reserved for the next phases

- Best price, then earliest sequence at that price.
- Execution at the resting order's price.
- Limit GTC residuals rest at the tail of their price level.
- IOC and market residuals terminate without resting.
- Replace uses atomic cancel-and-new semantics and resets time priority.
