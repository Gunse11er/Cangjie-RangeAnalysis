# Stage 4 large complex range-analysis case

This package is a 275-line end-to-end stress case for the contest `input.txt`
query path. It contains 82 queries and combines:

- nested signed integer branch narrowing;
- bool equality, direct bool condition, and negated bool condition;
- match / `MULTIBRANCH` case constraints;
- simple, nested, and descending loops;
- stage 4 unary integer, bitwise, shift, and bool logical expressions.

Some queries intentionally cover conservative scenarios, such as variable-variable
relations and branch refinements inside loop bodies after widening. Those results
are expected to remain sound fallback ranges unless later precision work improves
them.

Run with:

```bash
cd /home/gunseller/project/cangjie_compiler/range_analysis_stage4/e2e_large_complex
export CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output
../../build/build/bin/cjc.stage4 main.cj --dump-chir -O2 --output-type=staticlib -o large_complex_static
diff -u expected_output.txt output.txt
```
