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
