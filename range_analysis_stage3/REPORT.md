# Range Analysis 阶段 3 报告：循环精度增强

## 1. 阶段目标

阶段 3 对应 `plan.md` 中 F4「循环分析增强」。本阶段目标是改善原实现依赖 `MAX_INQUEUE_TIMES = 4` 后直接清空状态造成的循环精度退化，让简单 `while` 与嵌套 `while` 在有限时间内收敛，并保留 loop guard 对循环体和退出边提供的边界信息。

本阶段重点覆盖：

- 简单递增计数器 `while (i < c)` 的循环体、递增后、退出边范围。
- 嵌套循环中外层计数器与内层计数器的范围。
- loop condition 不被 RangePropagation 错误折叠成常量 `true`。
- 阶段 2 的 `Branch` / `MultiBranch` 收窄结果不回退。

## 2. 主要实现

### 2.1 Loop widening

在 `ValueRangeAnalysis.cpp` 中将原先硬阈值清空改为轻量 widening：

- `LOOP_WIDENING_START_TIMES = 4`
- `MAX_INQUEUE_TIMES = 32`
- 对同一 block 多次入队后，对 block 内整数表达式、整数 operand、`Load` / `Store` 指向的整数对象做 one-sided widening。
- 上界持续增大时放宽为 `[currentMin, +inf]`，下界持续降低时放宽为 `[-inf, currentMax]`；wrapped 或双向扩张仍保守退回 top。

### 2.2 分支边约束增强

在阶段 2 的 edge-specific state 基础上继续增强：

- 关系条件的 true/false 边如果和当前值域相交为空，不再原样传播旧状态，而是把被约束值置为 bottom，避免 infeasible 早期迭代污染后继 block。
- 对 CHIR `Constant` 定义表达式增加单点反查，避免常量没有保存在当前 state 时丢失 `i < 4` 的退出边约束。
- 对 loop guard 的 `load(var) < const` 增加受限下界恢复：只有确认 header 存在 back edge，且回边对同一变量是 `var = load(var) + 非负常量` 时，才把非回边前驱中的初始化常量作为下界补回。
- 增加简单归纳变量 exit narrowing：识别常量初值、`i < C` / `i <= C` / `i > C` / `i >= C` guard，以及单一 backedge 更新 `i = i + const` / `i = i - const`。只有能用闭式计算证明退出值为单点且不越界时，才把 exit edge 收窄到精确值。
- 对负 step 场景补充受限上界恢复：当 backedge step 明确为负时，在 true edge 上保留初始化上界，避免递减循环体退化为 `[lower,+inf]`。

### 2.3 查询输出时机修正

`CHIR.cpp` 中将 `RangePropagation::EmitContestOutput` 前移到 RangeAnalysis 刚完成之后、RangePropagation 改写和 DCE 之前。

原因：比赛输出需要使用分析结果对应的原始 CHIR 状态。若先改写 IR 再用旧的 `Results` 二次模拟，会在更精确的循环结果下出现状态与 IR 错位。

### 2.4 Loop branch 保护

保留上一版阶段 3 中的 loop branch 保护：

- `RangeAnalysis::HandleBranchTerminator` 不对能回到当前 block 的 loop branch 做单后继改写。
- `RangePropagation` 不把 loop branch condition 表达式折叠为常量。

## 3. 新增测试

目录：

```text
range_analysis_stage3/e2e_loop/
```

测试程序包含：

- `simple()`：`while (i < 4)` 递增计数器。
- `nested()`：外层 `while (outer < 3)` 与内层 `while (inner < 2)`。

查询点与当前结果：

| 变量 | 当前结果 | 说明 |
| --- | ---: | --- |
| `bodyI` | `[0, 3:1]` | 循环体 true 边收窄 |
| `afterInc` | `[1, 4:1]` | 自增后范围 |
| `exitI` | `4` | 简单归纳变量退出值精确推导 |
| `outerBody` | `[0, 2:1]` | 外层下界经受限归纳变量恢复补回 |
| `innerBody` | `[0, 1:1]` | 内层循环体范围 |
| `innerAfter` | `[1, 2:1]` | 内层自增后范围 |
| `innerExit` | `2` | 内层退出点精确到单点 |
| `outerExit` | `3` | 外层简单归纳变量退出值精确推导 |

实际 `output.txt`：

```text
[0, 3:1]
[1, 4:1]
4
[0, 2:1]
[0, 1:1]
[1, 2:1]
2
3
```

补充递减循环用例：

```text
range_analysis_stage3/e2e_loop_descending/
```

该用例覆盖 `while (j >= 1) { j = j - 1 }`：

```text
[1, 3:1]
0
```

## 4. 验证结果

已执行对象编译与链接：

```bash
env PATH=/home/gunseller/project/cangjie_compiler/build/codex-tools:/usr/local/bin:/usr/bin:/bin \
  ninja -C build/codex-build \
  src/CHIR/CMakeFiles/CangjieCHIRExtra.dir/Analysis/ValueRangeAnalysis.cpp.o \
  src/CHIR/CMakeFiles/CangjieCHIRBase.dir/CHIR.cpp.o -j2
```

阶段 3 端到端验证：

```bash
cd /home/gunseller/project/cangjie_compiler/range_analysis_stage3/e2e_loop
CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output \
  /home/gunseller/project/cangjie_compiler/build/build/bin/cjc.stage3 \
  main.cj --dump-chir -O2 --output-type=staticlib -o stage3_loop_static
diff -u expected_output.txt output.txt
```

结果：`diff` 通过。

递减循环补充验证：

```bash
cd /home/gunseller/project/cangjie_compiler/range_analysis_stage3/e2e_loop_descending
CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output \
  /home/gunseller/project/cangjie_compiler/build/build/bin/cjc.stage3 \
  main.cj --dump-chir -O2 --output-type=staticlib -o stage3_desc_static
diff -u expected_output.txt output.txt
```

结果：`diff` 通过。

阶段 2 回归验证：

```bash
cd /home/gunseller/project/cangjie_compiler/range_analysis_stage2/e2e_complex
CANGJIE_HOME=/home/gunseller/project/cangjie_compiler/output \
  /home/gunseller/project/cangjie_compiler/build/build/bin/cjc.stage3 \
  main.cj --dump-chir -O2 --output-type=staticlib -o stage3_complex_static
diff -u expected_output.txt output.txt
```

结果：`diff` 通过，阶段 2 的分支、布尔条件、`MultiBranch` case 收窄未回退。

## 5. 验收结论

阶段 3 当前已满足 `plan.md` 中 F4 的阶段验收口径：

- 简单 `while` 计数器不再无条件退化为类型全域。
- 嵌套循环能在有限时间内收敛。
- loop exit false 边约束已经能体现在查询结果中。
- 外层递增归纳变量在受限模式下能恢复初始化下界。
- 简单递增 / 递减归纳变量的退出值在可证明时能精确到单点。
- 复杂或无法证明单调的循环仍保持保守 fallback。

结论：阶段 3 可以提交你确认；确认后再进入下一阶段。
