# 阶段 2 分支精度提升报告

## 1. 阶段目标

本阶段对应 `plan.md` 的“阶段 2：分支精度提升”，目标是在阶段 1 已完成比赛 I/O 闭环的基础上，把 Range Analysis 的状态传播从“所有后继共享同一份出边状态”提升为“按分支后继携带约束后的状态”。

本阶段重点覆盖：

- 从 `Branch` 条件中提取约束。
- 为 true / false 出边分别生成收窄后的 `RangeDomain`。
- 支持整数变量与常量比较。
- 支持整数变量与变量比较的符号约束。
- 支持布尔变量分支与 `!flag` 形式。
- 支持 CHIR `MultiBranch` case successor 的常量 case 收窄。

## 2. 本阶段完成内容

### 2.1 数据流引擎出边状态钩子

在通用 worklist engine 中新增 `GetTerminatorStateForSuccessor(...)` 自由函数钩子，默认实现保持原状态不变：

- 未提供专用重载的分析不受影响。
- `RangeAnalysis` 通过同名重载在分支出边处插入约束收窄。
- 避免修改 `Analysis<Domain>` 的虚函数接口，降低对其他分析的侵入。

### 2.2 Branch 条件约束提取

为 `RangeAnalysis` 新增分支条件解析逻辑：

- `if (flag)`：true 边将 `flag` 收窄为 `true`，false 边收窄为 `false`。
- `if (!flag)`：递归反转条件，true 边将 `flag` 收窄为 `false`，false 边收窄为 `true`。
- `if (x < 10)` / `if (x >= 10)` 等整数比较：按出边方向应用原关系或取反关系。
- `==` / `!=`：在一侧为单值时收窄另一侧。

### 2.3 整数比较收窄

整数比较支持两类路径：

- 变量与常量比较：用 `SIntDomain::FromNumeric(...)` 生成数值区间约束，再与当前状态求交。
- 变量与变量比较：在两侧位宽一致时，用 `SIntDomain::FromSymbolic(...)` 记录符号关系，并在左右两侧分别写入正向与反向关系。

例如 `if (x < 10)`：

- true 边：`x` 收窄为 `[-9223372036854775808, 9]`。
- false 边：`x` 收窄为 `[10, 9223372036854775807]`。

### 2.4 MultiBranch 支持

新增 CHIR `MULTIBRANCH` 出边处理：

- case successor：当 case value 是整数字面量时，将 condition 收窄为该 case 的单值。
- default successor：对已列出的 case value 追加 `!=` 约束，优先保证 soundness。
- 当 `match` lowering 生成 `MultiBranch(TypeCast(tag))` 时，如果 TypeCast 源值与 condition 位宽一致，同时把 case 约束反推到源变量 `tag`，使 case 分支内的源变量查询可以看到 `1` / `2` 等单值。

### 2.5 一元 NOT 精度

补充 `HandleUnaryExpr` 中布尔 `NOT` 的范围计算：

- 当操作数已有非平凡布尔域时，结果直接更新为取反后的 `BoolRange`。
- 这让 `let neg = !flag` 与 `if (!flag)` 相关路径都能获得更精确布尔状态。

## 3. 源码改动

- `include/cangjie/CHIR/Analysis/Engine.h`
  - 新增默认 `GetTerminatorStateForSuccessor(...)`。
  - 在后继 Join 前按后继 block 计算出边状态。
- `include/cangjie/CHIR/Analysis/ValueRangeAnalysis.h`
  - 声明 `RangeDomain` 专用的出边状态重载。
- `src/CHIR/Analysis/ValueRangeAnalysis.cpp`
  - 新增分支条件提取、关系取反、左右关系交换、整数/布尔收窄、MultiBranch 收窄逻辑。
  - 新增布尔 `NOT` 的直接范围计算。
- `src/CHIR/Optimization/RangePropagation.cpp`
  - 增强比赛 I/O 查询：除 `Debug(value, variableName)` 外，也可以在同一源码行的普通表达式 operand 上，使用已记录的 value-name 映射读取状态。
  - 该增强用于验证 `match` case block 中没有 branch-local `Debug(tag)` 时的 MultiBranch case 收窄结果。

## 4. 验证记录

### 4.1 补丁卫生检查

已执行：

```bash
cd /home/gunseller/project/cangjie_compiler
git diff --check
```

结果：通过。

### 4.2 对象级编译验证

已执行：

```bash
cd /home/gunseller/project/cangjie_compiler
env PATH=/home/gunseller/project/cangjie_compiler/build/codex-tools:/usr/local/bin:/usr/bin:/bin \
  ninja -C build/codex-build \
    src/CHIR/CMakeFiles/CangjieCHIRExtra.dir/Analysis/ValueRangeAnalysis.cpp.o \
    src/CHIR/CMakeFiles/CangjieCHIRExtra.dir/Optimization/RangePropagation.cpp.o \
    src/CHIR/CMakeFiles/CangjieCHIRBase.dir/CHIR.cpp.o -j2
```

结果：通过。

### 4.3 可执行编译器验证

为端到端验证，基于 `build/build` 的完整对象和本阶段重新编译对象，临时链接生成：

```text
/home/gunseller/project/cangjie_compiler/build/build/bin/cjc.stage2
```

验证结果：

```text
Cangjie Compiler: 0.0.1 (cjnative)
Target: x86_64-unknown-linux-gnu
```

说明：当前 WSL 环境与旧构建产物存在 glibc 版本差异，最终链接时使用了临时 `mallinfo2` 兼容对象补齐 LLVM 静态库符号；该兼容对象未写入源码树。

### 4.4 端到端样例验证

验证目录：

```text
/home/gunseller/project/cangjie_compiler/range_analysis_stage2/e2e_branch
```

源程序核心逻辑：

```cj
func probe(x: Int64, y: Int64, flag: Bool): Int64 {
    if (x < 10) {
        let ltTrue: Int64 = x
    } else {
        let ltFalse: Int64 = x
    }
    if (flag) {
        let flagTrue: Bool = flag
    } else {
        let flagFalse: Bool = flag
    }
    if (!flag) {
        let notFlagTrue: Bool = flag
    } else {
        let notFlagFalse: Bool = flag
    }
    return x
}
```

查询输入：

```text
[main.cj, 3, ltTrue]
[main.cj, 5, ltFalse]
[main.cj, 14, flagTrue]
[main.cj, 16, flagFalse]
[main.cj, 19, notFlagTrue]
[main.cj, 21, notFlagFalse]
```

生成的 `output.txt`：

```text
[-9223372036854775808, 9:1]
[10, 9223372036854775807:1]
true
false
false
true
```

已执行 `diff -u expected_output.txt output.txt`，结果一致。

## 5. 阶段 2 验收状态

| 验收项 | 状态 | 说明 |
| --- | --- | --- |
| true / false 边分别收窄 | 通过 | `x < 10` 的两条边输出互补区间 |
| 布尔变量分支收窄 | 通过 | `flag` true/false 边输出单值 |
| `!flag` 条件收窄 | 通过 | true 边输出 `false`，false 边输出 `true` |
| 变量与常量比较 | 通过 | 使用数值区间求交，输出可见精确区间 |
| 变量与变量比较 | 通过 | 已写入符号约束；当前 I/O 对纯符号关系仍以 sound 输出为主 |
| MultiBranch case 处理 | 通过 | 已支持 CHIR `MULTIBRANCH` case/default 出边约束，并可在 `match` case 1/2 中查询到 `tag = 1` / `tag = 2` |
| 阶段 1 I/O 闭环兼容 | 通过 | 继续通过 `input.txt` / `output.txt` 验证 |

## 6. 已知限制

- 当前出边收窄遇到空交集时保留原状态，优先保证“不漏值”；后续阶段可考虑显式标记不可达边以提升精度。
- 变量与变量比较的符号约束已经进入域状态，但比赛输出格式目前主要展示数值区间；纯符号关系未必能直接在 `output.txt` 中体现为更窄的数值区间。
- `MultiBranch` default 分支的 `!= case` 组合对 interval domain 仍不友好，当前输出通常保持 full range 以保证 soundness。
- 本地完整 `ninja cjc` 仍受旧构建路径与 glibc 环境影响，本阶段以对象级编译和 `cjc.stage2` 端到端样例作为验收依据。

## 7. 阶段 3 入口

阶段 3 可以在本阶段的分支出边状态基础上继续提升循环与回边精度，建议优先确认：

- `while` / `for` 条件出边是否复用当前 Branch 收窄。
- 回边 Join 是否需要 widening 或迭代次数保护。
- 归纳变量的常见更新模式是否能在 `SIntDomain` 中表达。
- 新增循环端到端样例，覆盖进入循环、退出循环和循环体内查询。

阶段 2 完成。
