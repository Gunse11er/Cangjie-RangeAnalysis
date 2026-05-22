# Stage 2 UInt64 branch narrowing case

This case verifies unsigned integer branch narrowing in the stage 2 branch
precision work:

- true and false edges for `u < C`, `u <= C`, `u > C`, and `u >= C`;
- nested intersection of an outer unsigned guard with an inner guard;
- unsigned lower bound `0` is preserved on false and true edges.

Run:

```bash
cd /home/gunseller/project/cangjie_compiler/range_analysis_stage2/e2e_uint_branch
export CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output
../../build/build/bin/cjc.stage2.uint main.cj --dump-chir -O2 --output-type=staticlib -o stage2_uint_branch_static
diff -u expected_output.txt output.txt
```

`cjc.stage2.uint` is the local verification binary relinked from the current
`ValueRangeAnalysis.cpp.o`; any freshly rebuilt `cjc` containing this patch can
be used in its place.
