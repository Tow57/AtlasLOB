# Phase 3 fuzz seed corpus

These hexadecimal byte streams seed the bounded command-sequence fuzzer:

- `minimal.hex` proves the smallest nonempty interpretation.
- `golden-mixed.hex` exercises all byte-to-command branches.
- `boundary.hex` concentrates zero and maximum byte values.
- `phase2-regression.hex` encodes a mixed lifecycle shape derived from the Phase 2
  partial-fill, cancel, and replacement regression family.

The semantic command shrinker remains authoritative after a byte-level failure is
found; these files are starting inputs, not expected-output fixtures.
