# Range Analysis 阶段 4 综合增强补充报告

## 结论

本轮综合增强已完成，重点修复了复杂循环中的 guard 上下界丢失、循环体内 `i +/- const` 更新后退化为 full，以及 Bool identity 条件的反向约束传播。

实现仍保持保守策略：只对能够从 CHIR 结构明确识别的循环 guard load、常量 Bool identity 和短路临时变量别名做收窄；复杂循环、多路径不确定更新、变量步长和 interval domain 不能表达的否定集合仍 fallback 到安全结果。

## 主要改动

修改文件：

- `src/CHIR/Analysis/ValueRangeAnalysis.cpp`

核心实现：

- 在 loop widening candidate 中增加 `preserveDuringBodyWidening` 标记。
- 对来自循环 header 条件的 load 值，在循环体普通块 widening 时保留其 guard 精度，避免 `while (i < 6)` 已知的 `i <= 5` 被 body widening 抹成 `+inf`。
- 保留原有 body widening 对 `sum` 等循环携带值的收敛作用，避免只在 header widening 导致非条件变量无法稳定。
- 分支收窄时同步尝试收窄 load 背后的整数 ref object，使后续同一存储位置的使用能继承边约束。
- 为 Bool 条件补充 identity 反向传播：
  - `x || false => x`
  - `x && true => x`
  - `x || true => true`
  - `x && false => false`
  - `x == true => x`
  - `x == false => !x`
- 识别 CHIR 短路表达式降级形态：`Branch(flag)` 后分别向临时 Bool ref 写入 `true/false`，随后 `Load(temp)` 再作为 if 条件时，将条件约束反向传播到原始 `flag`。

## 精度变化

大型综合用例中，本轮目标项已提升为：

```text
loopI         [0, 5:1]
earlyI        [0, 2:1]
lateI         [3, 5:1]
loopAfterInc  [1, 6:1]
loopExit      6
outerBody     [0, 3:1]
innerBody     [0, 2:1]
innerSmall    [0, 1:1]
innerLarge    2
innerAfter    [1, 3:1]
outerAfter    [1, 4:1]
outerExit     4
downBody      [2, 5:1]
downHigh      [4, 5:1]
downLow       [2, 3:1]
downAfter     [1, 4:1]
downExit      1
mixedFlagTrue true
mixedFlagFalse false
```

已有正确项保持不退化，包括普通整数分支、Bool 直接条件、MULTIBRANCH/match case、循环精确退出值，以及阶段 4 表达式 transfer。

## 验证

已执行：

```bash
env PATH=/home/gunseller/project/cangjie_compiler/build/codex-tools:/usr/local/bin:/usr/bin:/bin \
  ninja -C build/codex-build src/CHIR/CMakeFiles/CangjieCHIRExtra.dir/Analysis/ValueRangeAnalysis.cpp.o -j2
```

已回归：

```bash
cd /home/gunseller/project/cangjie_compiler/range_analysis_stage4/e2e_expr
CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output \
  ../../build/build/bin/cjc.stage4 main.cj --dump-chir -O2 --output-type=staticlib -o stage4_expr_static
diff -u expected_output.txt output.txt
```

```bash
cd /home/gunseller/project/cangjie_compiler/range_analysis_stage4/e2e_large_complex
CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output \
  ../../build/build/bin/cjc.stage4 main.cj --dump-chir -O2 --output-type=staticlib -o large_complex_static
diff -u expected_output.txt output.txt
```

两个 `diff` 均通过。

## 追加修复：contest query 类型 fallback

针对 GitHub Dijkstra 综合用例中暴露出的查询输出问题，补充了 `input.txt` 查询的源码类型提示逻辑。原先如果某个查询没有精确匹配到 CHIR Debug/operand，`output.txt` 会只能退回默认 Int64 full；这会让 `var seen0: Bool` 这类 Bool 变量错误显示为整数全区间。

本次补丁在加载 `[file, line, variable]` 查询时读取对应源码行，识别 `name: Bool`、`name: Int64`、`name: UInt64` 等显式类型。后续如果 CHIR `Type*` 缺失，则使用该类型提示生成 sound fallback：

```text
Bool   -> false, true
Int64  -> [-9223372036854775808, 9223372036854775807:1]
UInt64 -> [0, 18446744073709551615:1]
```

该逻辑只影响未解析查询的输出格式，不改变 RangeDomain、edge transfer、loop widening 或分支收窄语义。

验证时临时链接了包含本次对象文件的 `cjc.stage4.test`，然后执行：

```bash
cd /home/gunseller/project/cangjie_compiler/range_analysis_stage4/e2e_github_dijkstra_scalar
CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output \
  ../../build/build/bin/cjc.stage4.test main.cj --dump-chir -O2 --output-type=staticlib -o github_dijkstra_static
```

关键结果：

```text
[main.cj, 142, seen0] false, true
[main.cj, 143, seen1] false, true
[main.cj, 144, seen2] false, true
[main.cj, 145, seen3] false, true
[main.cj, 146, seen4] false, true
[main.cj, 147, seen5] false, true
```

## 追加修复：嵌套循环回边识别与 var/ref 更新精度

针对 GitHub Dijkstra 综合用例中的嵌套 `while`，继续修复了两个问题：

1. 旧的回边/退出边判断只依赖 CFG 可达性。在嵌套循环里，内层 `while` 的 false 出口可能经过外层循环再次回到内层 header，导致内层 preheader 和 exit 被误判为回边/非出口。
2. `var` 被降低为 `Allocate + Load + Store` 后，`inner = inner + 1`、`outer = outer + 1` 这类更新在复杂 join 后可能因操作数状态退化而得到 full。

本次实现：

- `IsLoopBodySuccessor` 对 `SourceExpr::WHILE_EXPR` / `SourceExpr::DO_WHILE_EXPR` 使用结构化边信息：true 边是循环体入口。
- `IsLoopExitSuccessor` 对 `while/do-while` 明确识别 false 边为退出边，避免被外层循环可达路径干扰。
- 新增 `RangeAnalysis::TryComputeSimpleInductionUpdateRange`，只在入口常量、唯一回边 step、`load(location) +/- const` 更新、循环 guard 都能证明一致时，直接推导更新表达式范围。

关键效果：

```text
innerTop    [-inf, 2] -> [0, 2]
innerAfter  full      -> [1, 3]
innerExit   [3,+inf] -> 3
outerAfter  full      -> [1, 4]
outerExit   4         -> 4
```

该逻辑仍保持保守：多路径不同 step、变量 step、非线性更新、非 `while/do-while` 可证明结构、无入口常量或无法计算精确退出值时，都会回退到原有安全结果。

已回归：

```bash
cd /home/gunseller/project/cangjie_compiler/range_analysis_stage4/e2e_github_dijkstra_scalar
CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output \
  ../../build/build/bin/cjc.stage4.test main.cj --dump-chir -O2 --output-type=staticlib -o github_dijkstra_static
```

```text
[main.cj, 369, outerTop]   [0, 3:1]
[main.cj, 372, innerTop]   [0, 2:1]
[main.cj, 382, innerAfter] [1, 3:1]
[main.cj, 385, innerExit]  3
[main.cj, 387, outerAfter] [1, 4:1]
[main.cj, 391, outerExit]  4
```

同时回归 `e2e_expr` 和 `e2e_large_complex`；`e2e_large_complex` 的 `innerExit` 期望从 `[3,+inf]` 更新为 `3`。

## 追加增强：signed 非负区间上的 `x | constMask`

优先级 4 中的 UInt64 条件分支 narrowing 已由 `range_analysis_stage2/e2e_uint_branch` 覆盖并通过回归，因此本次继续补强 signed 非负区间上的 bitwise-or mask 精度。

旧逻辑只对 UInt64 或符号位 mask 做处理；当表达式是 `Int64` 且已知 `x >= 0` 时，`x | 8` 仍可能退化为 full。新增逻辑位于 `TryComputeBitOrWithMask`：

- 如果 `mask == 0`，保持原区间；
- 如果 signed mask 设置了符号位，保持原有 `[mask, -1]` 保守结果；
- 如果 signed 输入区间已证明非负且不跨符号 wrap，则安全收窄；
- 当 `range.max & mask == 0` 时，精确推导 `[range.min | mask, range.max | mask]`；
- 否则只保守给出 `[mask, SMax]`，避免对 bitwise-or 的非单调场景做不安全收窄。

新增回归用例在 `range_analysis_stage4/e2e_expr`：

```cangjie
if (x >= 0) {
    if (x <= 7) {
        let signedSmall: Int64 = x
        let signedOrMask: Int64 = signedSmall | 8
    }
    let signedNonNeg: Int64 = x
    let signedOrOne: Int64 = signedNonNeg | 1
}
```

期望输出：

```text
signedSmall   [0, 7:1]
signedOrMask  [8, 15:1]
signedNonNeg  [0, 9223372036854775807:1]
signedOrOne   [1, 9223372036854775807:1]
```

本次完整回归覆盖阶段 0-4 的全部 `expected_output.txt` 用例，均已通过：

```text
range_analysis_stage0/sample
range_analysis_stage1/e2e
range_analysis_stage1/e2e_complex
range_analysis_stage1/e2e_fallback
range_analysis_stage1/e2efalse
range_analysis_stage2/e2e_branch
range_analysis_stage2/e2e_complex
range_analysis_stage2/e2e_uint_branch
range_analysis_stage3/e2e_loop
range_analysis_stage3/e2e_loop_descending
range_analysis_stage4/e2e_expr
range_analysis_stage4/e2e_large_complex
range_analysis_stage4/e2e_var_branch_join
range_analysis_stage4/e2e_var_conservative
range_analysis_stage4/e2e_var_multifile
range_analysis_stage4/e2e_var_nested_loop
range_analysis_stage4/e2e_var_ref_query
```
