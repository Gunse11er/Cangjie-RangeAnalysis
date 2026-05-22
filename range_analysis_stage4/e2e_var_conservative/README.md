# e2e_var_conservative

Checks sound fallback for mutable variables when values come from unknown
parameters, variable-variable comparisons, and non-expressible `!=` intervals.

Run:

```bash
export CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output
../../build/build/bin/cjc main.cj --dump-chir -O2 --output-type=staticlib -o var_conservative_static
diff -u expected_output.txt output.txt
```
