# 仓颉高性能 Range Analysis 分析项目计划

## 1. 项目背景与目标

本项目面向“仓颉高性能 Range Analysis 分析”赛题，目标是在仓颉编译前端生成的 CHIR 层实现高性能值域分析，分析整数类型和布尔类型变量在指定程序点的可能取值。

赛题输入是一组可由 `cjpm` 编译构建的仓颉包。每个测试包包含一个 `input.txt`，每行指定一个查询：

```text
[fileName, line, variableName]
```

项目需要在编译或分析流程中读取查询，对指定 CHIR 文件、指定行号、指定变量输出值域结果，并生成 `output.txt`。输出规则如下：

- 整数类型：支持枚举、区间或混合形式，例如 `1, 2, [5, 20:1]`。
- 布尔类型：使用枚举形式，例如 `true`、`false` 或 `false, true`。
- 正确性原则：输出必须等于标准答案，或是标准答案的 sound 超集；不能漏掉任何可能值。

总体目标按优先级排序：

1. 建立稳定的比赛 I/O 和查询闭环，保证所有查询都有 sound 输出。
2. 提升分支、循环、算术表达式和类型转换场景的分析精度。
3. 控制大程序分析时间和内存，在功能达标后争取性能排名。

## 2. 赛题评分与项目成功标准

初赛客观分由功能测试和性能测试组成：

- 功能测试占 40%。每个查询变量单独计分，输出必须与标准答案完全一致，或覆盖标准答案的所有可能值。
- 性能测试占 60%。只有当一个测试用例中所有查询变量功能测试均非 0 分时，该用例才有资格获得性能得分。
- 因此实现策略必须先保证 soundness，再逐步收窄范围；不能为了精度或速度产生漏值。

初赛成功标准：

- 仓颉编译器源码可在竞赛平台指定环境中构建运行。
- 对每个测试包自动读取 `input.txt` 并生成 `output.txt`。
- 直线代码、常量、基本算术、布尔值、简单分支和简单循环具备可验证结果。
- 未能精确分析的查询使用类型全域等 sound fallback，避免漏值。
- 分析流程具备基础性能统计，能够定位热点。

决赛成功标准：

- 能处理更大规模测试包和更多仓颉语言特性。
- 分支、循环、类型转换、数组下标等核心场景具备稳定精度。
- 形成设计文档、测试结果、性能对比、瓶颈问题和解决方案。
- 准备 5 分钟作品介绍视频、答辩材料和引用/合规说明。

## 3. 当前实现基线

已分析仓库：`/home/gunseller/project/cangjie_compiler`。

当前 Range Analysis 已接入 CHIR 优化流程，核心模块如下：

- `ConstantRange`：固定宽度整数区间抽象域，支持半开区间、wrapped range、交并差、算术、扩展和截断。
- `SIntDomain`：整数值域，包含 numeric bound 和 symbolic bounds，可表达部分变量间相对约束。
- `BoolDomain`：布尔四点域，包含 bottom、false、true、top。
- `RangeAnalysis`：基于通用 `ValueAnalysis` 的 CHIR transfer functions。
- `RangePropagation`：消费分析结果做常量改写、确定分支改写、DCE 触发和 varray 下标诊断。
- `Engine` / `Results`：通用 worklist fixpoint 引擎和结果访问框架。

已有能力：

- 支持整数和布尔常量。
- 支持整数 `add/sub/mul/div/mod` 的区间传播。
- 支持 signed/unsigned 语义和部分 overflow strategy。
- 支持整数比较、布尔 equality/inequality。
- 支持类型转换的宽度和符号处理。
- 支持 readonly global、简单 load/store、field/tuple 传播。
- 支持当条件已经确定时改写分支。

主要缺口：

- 缺少比赛所需的 `input.txt` 到 `output.txt` 查询系统。
- 当前 debug 字符串格式不符合赛题输出格式。
- 条件分支只在条件已确定时选择 successor，尚未系统地把 `if (x < c)` 约束传播到 true/false 边。
- 循环收敛依赖 `MAX_INQUEUE_TIMES = 4` 后清空状态，简单但容易丢失归纳变量精度。
- `blockLimit = 80` 可能跳过隐藏测试中的大函数。
- 一元运算、位运算、移位、多分支和常见 intrinsic 覆盖不足。
- 当前测试以抽象域单测为主，缺少比赛端到端查询用例。

## 4. 功能点划分

### F1 比赛 I/O 与查询系统

任务：

- 解析测试包根目录下的 `input.txt`。
- 支持 `[fileName, line, variableName]` 行格式，保留输入顺序。
- 建立 CHIR 文件、行号、变量名到 `RangeDomain` 状态的查询索引。
- 生成与 `input.txt` 一一对应的 `output.txt`。
- 对无法精确定位的查询输出 sound fallback，并在 debug 日志中记录原因。

交付物：

- 查询解析器。
- 查询执行入口。
- `output.txt` 生成逻辑。
- 最小端到端样例包。

验收标准：

- 给定直线代码样例，能够自动生成正确格式的 `output.txt`。
- 查询顺序与输入顺序一致。
- 查询失败不会导致编译器崩溃或漏输出。

### F2 输出格式化

任务：

- 新增独立于现有 `ToString()` 的比赛格式化器。
- 整数支持单值、枚举、连续区间、步长区间和混合输出。
- 布尔支持 `true`、`false`、`false, true`。
- wrapped range 或 disjoint 信息在输出前拆成赛题允许的非 wrapped 表达。
- top 输出为对应类型全域，bottom/unreachable 按查询策略处理。

交付物：

- 整数格式化函数。
- 布尔格式化函数。
- 格式化单元测试。

验收标准：

- 输出不出现内部格式如 `|>=0,<=4|`、`|any|`。
- 相同输入得到稳定、排序确定的输出。
- 全域、单点、小集合、区间和布尔 top 都有测试覆盖。

### F3 条件分支收窄

任务：

- 为 `Branch` 引入 true/false edge-specific state。
- 支持 `x < c`、`x <= c`、`x > c`、`x >= c`、`x == c`、`x != c` 的双边收窄。
- 支持变量间比较的 symbolic bound 传播。
- 支持布尔变量、`!b`、布尔 equality/inequality 的条件收窄。
- 为 `MultiBranch` case edge 增加单点约束，default edge 尽量表达 case 排除。

交付物：

- 条件约束提取器。
- 边状态传播机制。
- 分支相关 CHIR 集成测试。

验收标准：

- `if (x < 10)` true 分支中 `x` 能收窄到 `<= 9`，false 分支能收窄到 `>= 10`。
- 布尔条件分支能分别得到 `true` / `false`。
- 不确定分支保持 sound，不产生漏值。

### F4 循环分析增强

任务：

- 识别 CFG back edge 和 SCC。
- 将 widening 限定在 loop header 或 SCC header。
- 对稳定后的循环执行有限 narrowing，恢复 guard 提供的边界。
- 识别常见归纳变量：`i = init`、`i = i + step`、`i = i - step`。
- 结合循环 guard 推断归纳变量上下界和可选 stride。
- 复杂循环超过阈值时优先丢弃 symbolic bounds，再退化 numeric bound。

交付物：

- loop/SCC 分析辅助逻辑。
- widening/narrowing 策略。
- 简单循环和嵌套循环测试。

验收标准：

- 简单 `for` / `while` 计数器不再无条件退化为类型全域。
- 嵌套循环能在有限时间内收敛。
- 复杂循环 fallback 仍 sound。

### F5 表达式覆盖

任务：

- 支持一元布尔取反、整数取负、位取反。
- 支持移位和位运算的保守区间传播。
- 补充类型转换、overflow strategy、signed/unsigned 边界测试。
- 审计常见整数、布尔、数组和 range 相关 intrinsic。
- 对可证明纯函数或 intrinsic 增加摘要；对未知副作用保持保守 top 和 ref invalidation。

交付物：

- expression kind 覆盖表。
- 表达式 transfer function 补充。
- 对应单测和 CHIR 集成测试。

验收标准：

- 常见整数/布尔 CHIR 表达式不因缺少 transfer function 而过早 top。
- 未支持表达式有明确保守处理，不产生未定义行为。

### F6 性能优化

任务：

- 记录分析总时间、查询时间、join 次数、最大状态大小、widening 次数。
- 复用一次包级分析结果服务全部查询。
- 查询按 CHIR 文件和行号预索引，避免重复全量遍历。
- 对明显无查询相关性的函数做安全跳过或延迟分析。
- 限制 symbolic bounds 数量和状态规模。
- 调整 `blockLimit`、SCC 迭代阈值、narrowing 次数和小集合展开阈值。

交付物：

- 性能统计日志。
- 阈值配置说明。
- 性能对比表。

验收标准：

- 大测试包分析时间可解释、可复现。
- 热点定位清晰。
- 性能优化不破坏功能 soundness。

### F7 测试与答辩材料

任务：

- 建立单元测试、CHIR 集成测试、端到端比赛样例和性能测试。
- 整理设计文档，说明功能、性能、创新性和风险处理。
- 准备 5 分钟作品介绍视频提纲。
- 准备答辩 Q&A：soundness、widening、symbolic bounds、性能权衡、合规说明。

交付物：

- 测试用例集。
- 测试结果记录。
- 设计文档素材。
- 视频提纲和答辩提纲。

验收标准：

- 每个核心功能点至少有一个可回归测试。
- 答辩材料能解释“为什么 sound、为什么快、创新点在哪里”。

## 5. 阶段性计划

| 阶段 | 目标 | 主要任务 | 交付物 | 验收标准 |
| --- | --- | --- | --- | --- |
| 阶段 0：基线验证与样例准备 | 明确当前能力和调试入口 | 构建当前编译器；运行 CHIR 单测；准备最小 `cjpm` 样例；保存 CHIR dump | 构建/测试记录、最小 fixture、CHIR dump 对照 | 能复现当前 Range Analysis 行为，并能定位样例变量 |
| 阶段 1：比赛 I/O 闭环 | 打通输入、分析、输出 | 实现 `input.txt` 解析；查询状态收集；比赛格式化；生成 `output.txt` | 查询系统、格式化器、直线代码端到端测试 | 常量/赋值/布尔直线代码输出正确 |
| 阶段 2：分支精度提升 | 提升 if/else 和多分支精度 | 条件约束提取；true/false 边状态；变量间比较；布尔分支；MultiBranch case | 分支收窄实现、分支测试集 | 分支内部查询能得到收窄结果，且无漏值 |
| 阶段 3：循环精度提升 | 改善归纳变量和循环 guard | SCC/back edge；widening；narrowing；归纳变量识别；复杂循环 fallback | 循环分析增强、循环测试集 | 简单循环不退化为全类型 top，嵌套循环可收敛 |
| 阶段 4：表达式与语言特性补全 | 覆盖隐藏测试常见表达式 | 一元运算；位运算；移位；cast；intrinsic 审计和摘要 | expression kind 覆盖表、表达式测试 | 常见整数/布尔/类型转换/位运算均有 sound transfer |
| 阶段 5：性能冲刺 | 在功能达标后压缩运行时间 | profiling；查询相关函数过滤；状态规模限制；阈值调优；多线程验证 | 性能指标表、热点优化记录、阈值说明 | 大样例运行时间可控，优化前后有数据对比 |
| 阶段 6：提交与答辩准备 | 形成参赛交付材料 | 设计文档；测试结果；视频提纲；答辩 Q&A；引用和开源合规说明 | 设计文档素材、5 分钟视频提纲、答辩材料 | 可完整说明功能、性能、创新性和合规性 |

阶段推进原则：

- 每个阶段结束必须有可运行、可验证、可回归的交付物。
- 后一阶段不得破坏前一阶段的端到端查询闭环。
- 性能优化必须以功能非零和 soundness 为前提。

## 6. 验收标准

总体功能验收：

- 能在 CHIR 层分析整数和布尔类型变量。
- 能读取 `input.txt` 并生成 `output.txt`。
- 输出格式符合赛题允许的整数/布尔表达。
- 所有无法精确处理的路径都有 sound fallback。
- 不出现漏值；宁可输出超集，也不输出不完整集合。

阶段验收：

- 阶段 0：当前实现能力、缺口和调试流程已记录。
- 阶段 1：比赛 I/O 闭环完成，直线代码样例通过。
- 阶段 2：分支内查询可收窄，死分支处理 sound。
- 阶段 3：简单循环和嵌套循环可收敛，归纳变量精度优于当前硬清空策略。
- 阶段 4：常见表达式有明确 transfer 或 conservative fallback。
- 阶段 5：性能数据可采集，至少完成一轮热点优化。
- 阶段 6：设计文档、测试结果、视频和答辩材料准备完成。

质量验收：

- 新增代码遵循仓颉编译器现有风格。
- 单元测试和端到端测试可重复运行。
- 不引入未说明的第三方源码。
- 文档中明确列出参考文献和借鉴思想。

## 7. 测试计划

单元测试：

- `ConstantRange`：单点、空集、全域、wrapped range、交并差、signed/unsigned 边界。
- `SIntDomain`：numeric bound、symbolic bound、join/meet、比较关系、类型转换。
- `BoolDomain`：true/false/top/bottom、逻辑运算、equality/inequality。
- 格式化器：整数单点、枚举、区间、步长、全域、布尔 top。

CHIR 集成测试：

- 常量和变量赋值。
- 基本算术：加、减、乘、除、取模。
- 分支：常量条件、变量条件、嵌套 if、MultiBranch。
- 循环：递增计数器、递减计数器、嵌套循环、复杂循环 fallback。
- 类型转换：位宽变化、signed/unsigned 转换、overflow strategy。
- 位运算和移位。
- load/store、field、tuple、readonly global。

端到端比赛测试：

- 每个测试目录都是可 `cjpm` 构建的包。
- 每个包包含 `input.txt` 和预期输出。
- 自动运行编译/分析流程并比对 `output.txt`。
- 对刻意保守的场景验证输出是否为标准答案超集。

性能测试：

- 大量基本块的直线代码。
- 深层嵌套循环。
- 单包大量查询。
- 大 symbolic bounds 压力场景。
- 单线程和多线程模式对比。

## 8. 风险与应对

| 风险 | 影响 | 应对 |
| --- | --- | --- |
| 查询定位不准 | 输出错误变量或错误程序点 | 增加 debug resolver；结合 CHIR dump、`DebugLocation`、变量 `GetSrcCodeIdentifier()` 多源定位 |
| 条件边状态改造影响通用 engine | 可能破坏其他分析 | 保留旧 API 兼容包装，优先让 Range Analysis 使用扩展路径 |
| 循环 widening 过粗 | 查询结果退化为全域，影响功能分 | widening 只放在 loop/SCC header，稳定后执行有限 narrowing |
| 符号状态膨胀 | 内存和时间不可控 | 限制每个值的 symbolic bound 数量，超过阈值先丢弃 symbolic 信息 |
| 隐藏测试语言特性更多 | 大量 top，精度不足 | 建立 expression kind 审计表，优先补常见整数/布尔/数组相关表达式 |
| 性能优化导致不 sound | 功能分归零，性能无意义 | 每次性能优化都跑 soundness 回归，fallback 只允许变宽不允许变窄 |
| 第三方或文献借鉴说明不足 | 合规风险 | 文档和源码注释中明确参考文献和非复制实现方式 |

## 9. 参考文献与合规

参考文献：

1. Value Range Analysis: https://en.wikipedia.org/wiki/Value_range_analysis
2. W. H. Harrison, "Compiler Analysis of the Value Ranges for Variables," IEEE Transactions on Software Engineering, vol. SE-3, no. 3, pp. 243-250, May 1977, doi: 10.1109/TSE.1977.231133.
3. Campos V. H. S., Rodrigues R. E., de Assis Costa I. R., et al. "Speed and precision in range analysis," SBLP 2012.

合规要求：

- 本项目实现应基于仓颉编译器现有 CHIR 分析框架，不直接复制第三方源码。
- 若后续引入第三方代码或参考开源实现，必须在设计文档和源码头部说明来源、许可证和使用范围。
- 文献中的 widening、narrowing、SCC 和精度/速度权衡作为算法思想参考，具体实现需结合仓颉 CHIR 数据结构重新实现。

