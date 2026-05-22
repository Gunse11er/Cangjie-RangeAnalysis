# e2e_var_ref_query

This case checks whether contest range output can resolve source `var` bindings,
not only `let` SSA values. In CHIR, a source `var` is represented by a ref plus
load/store operations, so querying the source variable name must read the range
from the referenced abstract object.

Key checks:

- `direct` and `loopVar` query the mutable `var` itself at its declaration line.
- `boundedLower`, `boundedWindow`, `boundedAfterInc`, `boundedHigh`, and
  `boundedLow` check branch narrowing through loads from a mutable variable.
- `boolVar` checks a mutable Bool, while `boolTrueRead`/`boolFalseRead` check
  branch narrowing after loading it.
- `loopBody`, `loopAfterInc`, and `loopExit` check loop load/store propagation.

Run from this directory with a cjc binary containing the latest RangeAnalysis and
RangePropagation changes:

```bash
export CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output
../../build/build/bin/cjc main.cj --dump-chir -O2 --output-type=staticlib -o var_ref_query_static
diff -u expected_output.txt output.txt
```
