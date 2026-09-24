# Design

## Context

参见 `proposal.md`。系统保留 legacy 事件驱动路径，并为论文实验提供 C++17 请求级解析路径。论文路径显式推进 AE/CE batch 时间线，并使用 channel/bank/row-buffer 请求时序模型评估 HBM 排队、地址映射和行命中；它不冒充 Ramulator 或 RTL 级 cycle-accurate 模拟。

关键约束：

- 仓库只有 Cora、Citeseer、PubMed 与论文重合，故强校准矩阵使用这三个数据集。
- 论文公开的是大量相对加速/流量结果而非完整绝对周期，验收以可复现的相对指标为主。
- 完整论文配置计算量较大，开发自检需要独立的缩小配置。
- 当前分支已有使项目可构建的 HBM 配置与无效依赖修复，后续实现需保留这些变更。

## Goals / Non-Goals

**Goals:**

- 让模拟器的架构参数、关键优化和统计口径能够映射到论文 §IV 与 §V-A。
- 用同输入、同硬件参数的成对消融实验校准关键优化效应，并自动执行 ±20% 验收。
- 修复已知流量、地址和周期计数错误，建立单元与集成测试。
- 保留事件驱动模型的可读性和可扩展性，使后续增加 GraphSAGE/GIN 精确语义成为增量工作。

**Non-Goals:**

- 不实现 RTL、综合、布局布线或芯片面积复现。
- 不重建论文使用的 CPU/GPU 软件基线，不宣称绝对 1509×/6.5× 加速复现。
- 不在本变更中实现 DiffPool 或补齐全部论文数据集。
- 不通过不可解释的全局缩放因子强行贴合论文数字。

## Decisions

### 1. 保留现有事件模拟器，拆分架构配置、运行配置与机制开关

新增明确的配置层：架构参数描述计算与存储资源，运行参数描述模型/数据集/输出，机制开关描述稀疏消除、流水、组合策略和访存协调。论文配置和 smoke 配置使用不同文件，结果清单记录所有解析后的有效值。

选择该方案是因为现有模块边界已经对应 AE、CE、Coordinator 和 Aggregation Buffer，增量重构风险低。替代方案是重写模拟器，但会失去已经可运行的 DRAMSim3 集成，且难以在本次范围内完成校准。

### 2. 用聚合模型表示组合模块集群，而不是逐 PE 建模

组合侧引入模块级调度器，表示 8 个模块、每模块 4 条 128 宽阵列。每个任务根据矩阵维度计算 tile、流水填充、有效计算、权重传播和写回周期，再由调度器决定 independent 或 cooperative 的模块占用。

该方式能表达论文中的并行度、两种执行模式和权重复用，同时避免为 4096 个 PE 建立对象。替代方案是逐 PE 周期模拟，精度更高但复杂度和运行时间不适合当前目标。

### 3. 将图分区产物变为显式数据结构

每个 edge chunk 保存实际编码字节数、目标顶点范围、interval、shard、唯一邻居集合和所属 batch。Edge 请求使用真实编码字节数；稀疏消除关闭时使用完整 interval，开启时使用唯一邻居集合。现有未接入的边压缩代码只作为参考，不直接依赖其当前页面布局。

该设计同时解决 Edge 字节误用 Weight 大小的问题，并允许测试 Window Sliding & Shrinking 的输出。

### 4. 使用带阶段指针和时间线的有界 Aggregation Buffer

保留环形缓冲思想，将 allocated、ready、consuming、reclaim 四个阶段、字节范围和发生周期显式化。latency-aware 在一个分区 batch ready 后启动 CE；energy-aware 聚合多个分区直到目标顶点数或容量边界；sequential 等待当前层 AE 完成并按一次写出、一次读回计算中间流量。AE 在容量不足时等待最早可回收 batch，CE 完成后才释放对应空间。

相比硬编码双半区，这一设计可覆盖论文的 ping-pong 行为，同时兼容不同 batch 大小和后续扩展。

### 5. Coordinator 使用 batch 仲裁、低位交织映射和请求级 HBM 时序

每个 DRAM 请求携带 batch ID、请求类别、地址、字节数与入队周期。协调模式先选择最早 batch，再按 Edge、Input、Weight、Output 排序，并用 cache-line 低位交织到 channel/bank；对照模式保留 FIFO 与传统 row-first 映射。请求级模型跟踪每个 channel 的发射周期、每个 bank 的可用周期和 open row，行命中/未命中延迟、队列等待和完成周期进入 AE ready 时间与层总周期。

该规则对应论文“当前批次低优先级请求先于后续批次高优先级请求”的描述，避免现有全局严格优先级造成跨批次饥饿。

### 6. 以成对消融验证论文公开范围和平均值

基准工具为每个机制运行优化版与唯一开关关闭版，计算：

- `speedup = baseline_cycles / optimized_cycles`
- `dram_ratio = optimized_dram_bytes / baseline_dram_bytes`
- `bandwidth_gain = optimized_bandwidth_util / baseline_bandwidth_util`

参考清单区分三种证据：论文给出的逐数据集范围、论文正文给出的跨数据集平均值、以及只能从图中读取但尚未完成可追踪数字化的诊断项。稀疏加速按 1.1-3.0x 逐数据集检查；流水加速按 27%-53% 时间下降换算为 1.369863-2.127660x，流水 DRAM 比率按 0.50-0.73 逐数据集检查；协调器按正文平均 3.70x 加速和 4.00x 带宽提升检查。Fig. 15(b) 的稀疏输入 DRAM 比率在完成数字化前不参与强制门禁。

绝对周期仍被记录并用于回归，但没有可靠论文绝对值时不作为论文验收门槛。

### 7. 区分结构参数与时序参数

结构参数来自论文，不允许基准脚本修改。论文未给出的 HBM row hit/miss 延迟、row 大小、channel/bank 数和最小 energy-aware batch 集中在配置中，具有明确单位并写入运行清单。协调开关不得选择预设效率，流水开关不得选择固定重叠率，sequential 不得使用经验 spill 系数。

禁止在结果生成阶段乘全局“论文修正系数”或按数据集写特例。Cora、Citeseer、PubMed 必须使用同一结构和时序参数执行成对消融。

### 8. 参数化 CLI 与结构化输出

测试程序改造成单次实验入口，支持模型、数据集、层、配置档、策略、种子和输出路径。批量实验由工具脚本组合单次运行，输出 JSON/CSV 清单；校验脚本只读取结构化结果，不解析控制台文本。

无参数模式保留为兼容入口，但只遍历实际存在的数据集，并提示使用参数化模式。完整论文基准单独作为 CMake/CTest 目标，默认测试只运行 smoke 配置。

### 9. 测试分层

- 单元测试：edge chunk 字节、interval/shard、稀疏邻居集合、输出地址、systolic 周期、batch 仲裁、Aggregation Buffer 边界。
- 集成测试：小图在 sequential/latency-aware/energy-aware 与 independent/cooperative 组合下完成且统计守恒。
- 回归测试：固定 Cora smoke 周期与字节快照。
- 论文验收：Cora/Citeseer/PubMed 成对消融与 ±20% 报告。

## Risks / Trade-offs

- [论文图表缺少完整原始数据] → 优先采用正文明确给出的平均值；图中读取值只作为非强制诊断，并在参考清单标注来源和提取方式。
- [请求级模型与论文 Ramulator 存在精度差异] → 固定并记录 channel/bank/row 与延迟参数，报告明确标记为请求级复现；后续可用 DRAMSim3/Ramulator trace 对照校准时序参数。
- [模块级组合模型低估细粒度冲突] → 将模块占用、tile、填充和写回分别计数，并用单元测试覆盖边界矩阵尺寸。
- [为满足指标而过拟合三个数据集] → 限制可校准参数、保留 PubMed 验证集、同时报告逐数据集和平均结果。
- [完整配置运行时间较长] → 默认测试使用 smoke 配置，完整论文套件显式触发并支持按实验缓存。
- [GraphSAGE/GIN 当前语义不完整] → 不纳入第一阶段强制 ±20% 验收，先通过操作类型和输入完整性测试再升级其声明。

## Migration Plan

1. 固化当前可运行结果为 legacy 回归快照，并新增 `legacy` 配置档。
2. 引入配置/CLI/结构化输出，不改变核心周期模型，确保现有 GCN Cora 可继续运行。
3. 修复流量、地址和统计错误，增加对应单元测试。
4. 分别升级 AE 分区与稀疏、CE 模块集群、Aggregation Buffer 流水和 Coordinator 仲裁，每阶段运行 smoke 回归。
5. 加入论文配置和成对消融工具，先在 Cora/Citeseer 校准，再用 PubMed 验证。
6. 当全部强制指标通过后，生成最终验收报告；若出现回归，可切回 legacy 配置和旧调度策略定位差异。
