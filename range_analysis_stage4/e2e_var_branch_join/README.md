# e2e_var_branch_join

Checks mutable `var` ranges across branch stores and post-join narrowing.

Run:

```bash
export CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output
../../build/build/bin/cjc main.cj --dump-chir -O2 --output-type=staticlib -o var_branch_join_static
diff -u expected_output.txt output.txt
```
