# e2e_var_nested_loop

Checks mutable loop variables in nested loops, including direct `var`
declaration queries, body ranges, updates, and exits. The inner exit currently
uses the conservative `[4, +inf]` form; this is sound and catches regressions in
loop guard propagation without requiring every nested loop exit to become a
singleton.

Run:

```bash
export CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output
../../build/build/bin/cjc main.cj --dump-chir -O2 --output-type=staticlib -o var_nested_loop_static
diff -u expected_output.txt output.txt
```
