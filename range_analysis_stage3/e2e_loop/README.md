# Stage 3 Loop E2E Case

This case covers the stage 3 loop precision work.

- `simple`: a monotonic `while (i < 4)` counter. Stage 2 reported all queried loop values as full; stage 3 keeps the body counter at `[0, 3]` and the post-increment counter at `[1, 4]`.
- `exitI` / `outerExit`: verifies simple induction loop exits are narrowed to exact singleton values.
- `nested`: nested `while` loops. The inner loop converges with `[0, 1]` / `[1, 2]`, `innerExit` becomes `2`, and the outer loop keeps `[0, 2]` in the body.
- The loop condition in the dumped CHIR remains a `Branch(LT(...))` instead of being folded to a constant `true`, which guards soundness after widening.

Run:

```bash
cd /home/gunseller/project/cangjie_compiler/range_analysis_stage3/e2e_loop
CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output \
  /home/gunseller/project/cangjie_compiler/build/build/bin/cjc.stage3 \
  main.cj --dump-chir -O2 --output-type=staticlib -o stage3_loop_static
diff -u expected_output.txt output.txt
```
