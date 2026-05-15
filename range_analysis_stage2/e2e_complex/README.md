# Stage 2 Complex E2E Case

This case covers the stage 2 branch-narrowing features in one source file.

- `x > pivot`: variable-to-variable comparison where `pivot` is a tracked single-value local.
- nested `x > pivot` then `x < 10`: verifies intersection across successive branch edges.
- `x < 10`: variable-to-constant comparison with true/false edge intervals.
- `x >= y`: unknown variable-to-variable comparison, exercising symbolic constraints while keeping output sound.
- `flag == knownTrue`: boolean equality constraint.
- `if (flag)`: direct boolean branch.
- `if (!flag)`: unary NOT plus inverted branch constraint.
- `match (tag)`: lowers to CHIR `MultiBranch`; case 1/2 queries now narrow `tag` to `1` and `2`.

Run:

```bash
cd /home/gunseller/project/cangjie_compiler/range_analysis_stage2/e2e_complex
CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output \
  /home/gunseller/project/cangjie_compiler/build/build/bin/cjc.stage2 \
  main.cj --dump-chir -O2 --output-type=staticlib -o stage2_complex_static
diff -u expected_output.txt output.txt
```
