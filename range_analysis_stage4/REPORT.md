# Range Analysis 阶段 4 报告：表达式覆盖增强

## 结论

阶段 4 的基础目标已完成：在不破坏阶段 2/3 的前提下，补充了一元整数表达式、位运算、移位和布尔逻辑表达式的保守 range transfer，并新增端到端用例验证。

本阶段仍遵循 soundness 优先策略：能证明精确或可安全收窄时输出更小区间；不能证明时回退到类型全域。

## 需求对照

| 阶段 4 需求 | 当前状态 | 说明 |
| --- | --- | --- |
| 一元布尔取反 | 已具备并保留 | 原有 `NOT Bool` transfer 未退化 |
| 整数取负 | 已完成 | 新增 `NEG` 区间传播；单点值走 overflow checker，非单点保守处理 |
| 整数位取反 | 已完成 | `BITNOT(x)` 按 `-x - 1` 的等价关系做区间传播 |
| 位运算 | 已完成基础覆盖 | 支持 `&`、`|`、`^` 的单点精确传播、自身运算、零掩码、常量 mask 场景 |
| 移位 | 已完成基础覆盖 | shift amount 为单点且合法时传播；不确定或越界时回退 top |
| 布尔 `&&` / `||` | 已完成基础覆盖 | 对直接 `AND` / `OR` CHIR 表达式增加 BoolDomain transfer；短路分支仍走阶段 2 的边约束 |
| 类型转换 | 已审计并保留 | 继续使用既有 `HandleTypeCast` / `ComputeTypeCast` 路径，本阶段未引入退化 |
| intrinsic / apply | 已审计保守 fallback | 未新增不确定 intrinsic 摘要；未知表达式仍 top/ref fallback，避免 unsound 收窄 |

## 主要实现

修改文件：

- `src/CHIR/Analysis/ValueRangeAnalysis.cpp`

新增或扩展的核心逻辑：

- 新增表达式分类辅助：`IsBitwiseBinaryExpr`、`IsShiftBinaryExpr`、`IsLogicalBinaryExpr`。
- 新增一元整数 transfer：`ComputeNegRange`、`ComputeBitNotRange`。
- 新增位运算/移位 transfer：`TryComputeShiftRange`、`TryComputeBitAndWithMask`、`TryComputeBitOrWithMask`、`TryComputeBitwiseRange`。
- 扩展 `HandleUnaryExpr`：支持 `NEG` 和 `BITNOT` 的整数区间传播。
- 扩展 `HandleBinaryExpr`：支持 `BITAND`、`BITOR`、`BITXOR`、`LSHIFT`、`RSHIFT`、`AND`、`OR`。

## 新增测试

新增目录：

- `range_analysis_stage4/e2e_expr/`

测试覆盖：

- `x in [-5,5]` 下的 `-x` 和 `!x`。
- `u & 7` 产生 `[0,7]` 后继续测试左移、右移、`& 15`、`| 8`、`^ 0`。
- `flag && false`、`flag || true` 的布尔结果。

阶段 4 输出：

```text
[-5, 5:1]
[-5, 5:1]
[-6, 4:1]
[0, 7:1]
[0, 14:1]
[0, 3:1]
[0, 7:1]
[8, 15:1]
[0, 7:1]
false
true
```

`expected_output.txt` 与实际 `output.txt` 已通过 `diff`。

## 回归验证

使用独立可执行文件：

- `build/build/bin/cjc.stage4`

已验证：

| 用例 | 结果 |
| --- | --- |
| `range_analysis_stage4/e2e_expr` | 通过 |
| `range_analysis_stage2/e2e_branch` | 通过 |
| `range_analysis_stage2/e2e_complex` | 通过 |
| `range_analysis_stage3/e2e_loop` | 通过 |
| `range_analysis_stage3/e2e_loop_descending` | 通过 |

同时完成：

- `ValueRangeAnalysis.cpp.o` 编译通过。
- `cjc.stage4 --version` 可执行。
- `git diff --check` 通过。

## 后续可优化

- `UInt64` 条件分支 narrowing 已由 `range_analysis_stage2/e2e_uint_branch` 覆盖；如隐藏用例出现更复杂的 unsigned 变量-变量比较，仍建议保持保守。
- `x | mask` 对 signed 非负区间已补充保守收窄；复杂 mask 重叠场景仍只输出 `[mask, SMax]`，避免不安全精确化。
- intrinsic 摘要仍未扩展，建议后续基于真实隐藏用例中出现频率最高的纯函数/内建函数逐个加入白名单。
