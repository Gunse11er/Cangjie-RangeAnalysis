# e2e_var_multifile

Checks that contest output can resolve mutable variable ranges in a multi-file
same-package compilation unit.

Run:

```bash
export CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output
../../build/build/bin/cjc helper.cj main.cj --dump-chir -O2 --output-type=staticlib -o var_multifile_static
diff -u expected_output.txt output.txt
```
