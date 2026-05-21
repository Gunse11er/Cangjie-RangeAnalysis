# 阶段 0 基线验证与样例准备报告

## 1. 阶段目标

本阶段对应 `plan.md` 的“阶段 0：基线验证与样例准备”，目标是确认当前仓颉编译器仓库的可运行状态，准备一个最小 Range Analysis 查询样例，并保存 CHIR dump 对照，为阶段 1 的比赛 I/O 闭环提供可复现基准。

本阶段不修改编译器源码。

## 2. 仓库与工具基线

- 仓库路径：`/home/gunseller/project/cangjie_compiler`
- 当前可用编译器：
  - `output/bin/cjc`
  - `build/build/bin/cjc`
  - `build/build/bin/chir-dis`
- `cjc --version`：
  - `Cangjie Compiler: 0.0.1 (cjnative)`
  - `Target: x86_64-unknown-linux-gnu`
- `cjpm`：当前 WSL 环境的 `PATH` 与仓库 `build/`、`output/` 目录下未发现 `cjpm` 可执行文件。
- 测试基线：
  - `python3 build.py test` 可运行。
  - `ctest -N` 显示 `Total Tests: 0`，当前构建目录没有注册 CTest 用例。

## 3. 最小样例

样例目录：`range_analysis_stage0/sample/`

源码：`range_analysis_stage0/sample/main.cj`

```cj
main(): Int64 {
    let base: Int64 = 3
    let step: Int64 = 2
    let sum: Int64 = base + step
    let flag: Bool = sum > 4
    if (flag) {
        return sum
    }
    return 0
}
```

比赛查询样例：`range_analysis_stage0/sample/input.txt`

```text
[main.cj, 2, base]
[main.cj, 3, step]
[main.cj, 4, sum]
[main.cj, 5, flag]
```

预期输出基准：`range_analysis_stage0/sample/expected_output.txt`

```text
3
2
5
true
```

## 4. 编译与 CHIR dump 对照

静态库方式：

```bash
cd /home/gunseller/project/cangjie_compiler/range_analysis_stage0/sample
../../output/bin/cjc main.cj --dump-chir -O2 --output-type=staticlib -o stage0_sample
```

结论：该方式会产生 `unused function:'main'` 警告，后续优化阶段中 `main` 容易被当作未使用函数删除，不适合作为阶段 1 的主验收样例。

可执行方式：

```bash
cd /home/gunseller/project/cangjie_compiler/range_analysis_stage0/sample
../../output/bin/cjc main.cj --dump-chir -O2 -o stage0_exe
```

结论：该方式能在 `stage0_exe_CHIR/0_AST_CHIR.chirtxt` 和 `stage0_exe_CHIR/20_RangePropagation.chirtxt` 中稳定保留 `main.cj`、`srcCodeIdentifier: main`、以及 `base`、`step`、`sum`、`flag` 对应的 `Debug` 节点。

关键对照文件：

- `range_analysis_stage0/stage0_exe_0_AST_CHIR.main_excerpt.txt`
- `range_analysis_stage0/stage0_exe_20_RangePropagation.main_excerpt.txt`

关键观察：

- `0_AST_CHIR` 中 `sum` 是 `Add(%1, %3)`，`flag` 是 `GT(%5, %7)`。
- `20_RangePropagation` 后，`sum` 对应值已变为 `Constant(5i)`，`flag` 对应值已变为 `Constant(true)`。
- `Debug(value, variableName)` 节点携带源码位置和变量名，可作为阶段 1 查询定位的第一条实现路径。

## 5. 阶段 1 输入条件

阶段 1 可以基于以下事实开始实现比赛 I/O 闭环：

- 查询文件可以先按 `[fileName, line, variableName]` 解析并保留顺序。
- 当前最小样例中的查询可通过 `Debug` 节点的 `loc: "main.cj"-line-column` 与变量名定位。
- 直线代码和常量折叠结果已经能在 `RangePropagation` 后的 CHIR dump 中看到。
- 对无法定位的查询，阶段 1 必须先提供 sound fallback，保证生成 `output.txt` 的行数与 `input.txt` 一致。

## 6. 已知限制与风险

- 当前环境未发现 `cjpm`，阶段 1 需要先确认比赛平台实际调用入口；本地开发可先用 `cjc` 直编样例闭环。
- 当前仓库没有注册 CTest 用例，后续需要补充专用端到端脚本或测试入口。
- `--dump-chir -O2` 会生成较大的 CHIR 目录，后续测试应保留关键摘录和可复现命令，避免把大 dump 当作唯一证据。
- 静态库编译会把样例 `main` 视为未使用函数；阶段 1 的样例执行路径优先采用可执行编译方式。

## 7. 阶段 0 验收结果

- 可运行：`cjc --version` 可用，样例可编译并生成 CHIR dump。
- 可验证：已保存源码、`input.txt`、`expected_output.txt`、CHIR 对照摘录。
- 可回归：阶段 1 可直接复用 `range_analysis_stage0/sample/` 作为最小端到端用例。

阶段 0 完成。
