# 阶段 1 比赛 I/O 闭环报告

## 1. 阶段目标

本阶段对应 `plan.md` 的“阶段 1：比赛 I/O 闭环”，目标是在不重构现有 Range Analysis 框架的前提下，打通比赛要求的最小输入输出链路：

- 当编译工作目录存在 `input.txt` 时，自动读取查询。
- 按 `[fileName, line, variableName]` 定位 CHIR 中的 `Debug(value, variableName)` 节点。
- 在 `RangePropagation` 已完成并具备缓存分析结果后查询变量值域。
- 生成与 `input.txt` 行顺序一致的 `output.txt`。
- 对无法解析或无法定位的查询输出 sound fallback，保证不漏值。

## 2. 本阶段完成内容

### 2.1 比赛输入解析

新增 `input.txt` 解析逻辑，支持赛题格式：

```text
[fileName, line, variableName]
```

实现要点：

- 去除路径前缀，仅用文件 basename 与 `DebugLocation` 匹配。
- 解析行号并检查尾随字符，避免 `12abc` 被误解析为 `12`。
- 对空行、格式错误行、非法行号保留一条无效查询记录，最终输出保守 fallback，避免输出行数错位。

### 2.2 查询定位与 Range Analysis 结果读取

新增查询定位逻辑：

- 使用 `Debug` 表达式的 `srcCodeIdentifier` 匹配变量名。
- 使用 `DebugLocation` 的文件名与起始行号匹配查询位置。
- 通过 `RangeDomain::CheckAbstractValue(debug.GetValue())` 读取该点的抽象值。

该方案复用现有 CHIR debug 信息，不引入新的前端标记，也不改变通用 worklist engine。

### 2.3 比赛输出生成

新增 `output.txt` 输出逻辑：

- `Bool` 单值输出 `true` 或 `false`。
- `Bool` top/fallback 输出 `false, true`。
- 整数单值输出十进制精确值。
- 非单值整数区间在阶段 1 暂时输出对应类型全范围 `[min, max:1]`，优先保证 soundness。
- 未知类型 fallback 输出 `[-9223372036854775808, 9223372036854775807:1]`。

## 3. 源码改动

- `include/cangjie/CHIR/Optimization/RangePropagation.h`
  - 新增 `RangePropagation::EmitContestOutput(...)` 声明。
- `src/CHIR/Optimization/RangePropagation.cpp`
  - 新增比赛 I/O 解析、查询解析、结果格式化、fallback、`output.txt` 写入逻辑。
  - 新增 `RangePropagation::EmitContestOutput(...)` 实现。
- `src/CHIR/CHIR.cpp`
  - 在 `ToCHIR::RunRangePropagation()` 中，于 `DumpCHIRToFile("RangePropagation")` 前调用 `EmitContestOutput(chirPkg, vra)`。

## 4. 验证记录

### 4.1 补丁卫生与编译验证

已完成以下验证：

- `git diff --check`：通过。
- `RangePropagation.cpp` 语法检查：通过。
- `CHIR.cpp` 语法检查：通过。
- 独立 CMake/Ninja 构建目录对象级编译：通过。

对象级编译命令：

```bash
cd /home/gunseller/project/cangjie_compiler
env PATH=/home/gunseller/project/cangjie_compiler/build/codex-tools:/usr/local/bin:/usr/bin:/bin \
  ninja -C build/codex-build \
  src/CHIR/CMakeFiles/CangjieCHIRExtra.dir/Optimization/RangePropagation.cpp.o \
  src/CHIR/CMakeFiles/CangjieCHIRBase.dir/CHIR.cpp.o -j2
```

### 4.2 可执行编译器验证

使用现有 `build/build` 的链接命令和仓库中已有的私有 glibc 兼容环境，生成临时验证二进制：

```text
/home/gunseller/project/cangjie_compiler/build/build/bin/cjc.stage1
```

验证结果：

```text
Cangjie Compiler: 0.0.1 (cjnative)
Target: x86_64-unknown-linux-gnu
```

说明：`cjc.stage1` 是本阶段为了端到端验证生成的临时二进制，正式提交仍以源码改动为准。

### 4.3 端到端样例验证

验证目录：

```text
/home/gunseller/project/cangjie_compiler/range_analysis_stage1/e2e
```

输入文件：

```text
[main.cj, 2, base]
[main.cj, 3, step]
[main.cj, 4, sum]
[main.cj, 5, flag]
```

生成的 `output.txt`：

```text
3
2
5
true
```

已与 `expected_output.txt` 做 `diff -u` 对比，结果一致。

## 5. 阶段 1 验收状态

| 验收项 | 状态 | 说明 |
| --- | --- | --- |
| 可自动读取 `input.txt` | 通过 | 仅在当前工作目录存在 `input.txt` 时启用，不影响普通编译 |
| 可自动生成 `output.txt` | 通过 | 输出行数与输入查询行保持一致 |
| 支持直线代码查询 | 通过 | 阶段 0 样例中 `base/step/sum/flag` 均输出精确值 |
| 保守 fallback | 通过 | 未解析、未定位、非单值整数区间输出 sound fallback |
| 不侵入通用分析框架 | 通过 | 复用 `RangeAnalysisWrapper` 缓存结果和 `VisitWith` 遍历 |
| 端到端最小样例 | 通过 | `range_analysis_stage1/e2e/output.txt` 与期望一致 |

## 6. 已知限制

- 查询定位目前依赖 `Debug` 节点，且文件名、行号、变量名需要匹配；复杂语法展开后的位置偏移将在后续阶段继续增强。
- 整数非单值区间在阶段 1 暂时输出类型全范围，保证 soundness 但精度不足。
- 条件分支 true/false 边的状态收窄尚未增强，这是阶段 2 的主要任务。
- 循环、归纳变量、位运算、一元运算等精度提升不在阶段 1 范围内。

## 7. 阶段 2 入口

阶段 2 可以在当前 I/O 闭环基础上开始做分支精度提升，建议优先完成：

- 梳理 `RangeAnalysis` 当前对 `Branch` / 条件表达式的状态传播路径。
- 为 `if` 的 true/false 出边加入整数比较约束。
- 为布尔变量条件加入 `true` / `false` 收窄。
- 增加端到端样例：`if/else`、嵌套 `if`、变量间比较、多分支布尔条件。

阶段 1 完成。
