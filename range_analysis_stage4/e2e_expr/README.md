# Stage 4 expression coverage case

This package verifies phase 4 range-analysis transfer functions for unary integer expressions, bitwise
operations, shifts, and boolean logical expressions.

Covered queries:

- Signed branch narrowing plus `NEG` and `BITNOT`.
- Unsigned `BITAND` mask narrowing plus `LSHIFT`, `RSHIFT`, `BITAND`, `BITOR`, and `BITXOR`.
- Boolean `AND` / `OR` with a known constant side.
