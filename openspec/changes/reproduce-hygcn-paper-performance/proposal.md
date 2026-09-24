# Proposal

## Why

当前项目只复刻了 HyGCN 的高层数据流骨架，硬件规模、组合引擎组织、流水模式、访存协调和统计口径与论文存在显著偏差，现有周期与能耗结果无法用于论文级性能判断。本变更将把模拟器提升为可配置、可消融、可校准的论文复现实验平台，使关键相对性能指标在受支持数据集上进入论文报告值的 ±20% 区间。

## What Changes

- 按论文配置建立 32×SIMD16 聚合引擎、8 个组合模块（每模块 4×128 计算阵列）及论文容量级缓冲配置，同时保留可缩放的快速测试配置。
- 修正 Edge 流量、输出地址、systolic 周期/MAC/字节统计和片上聚合缓冲建模中的已知偏差。
- 将 Interval/Shard 分区、动态稀疏消除、AE/CE 流水和 Memory Access Coordinator 变为显式可开关机制，支持基线与消融对照。
- 实现 latency-aware 与 energy-aware 流水策略，以及组合引擎 independent/cooperative 执行策略的周期级近似模型。
- 增加 batch-aware DRAM 调度、论文式请求优先级和可审计的地址映射统计。
- 提供可选择模型、数据集、层和配置的命令行运行器，避免只能执行硬编码全量实验。
- 增加论文指标基线文件、自动基准脚本和 ±20% 误差检查，覆盖稀疏优化、引擎流水、访存协调及端到端周期指标。
- 明确模型支持边界：GCN 作为强校准路径；GraphSAGE/GIN 至少提供一致的数据流与操作类型建模；不在本变更中宣称复现 DiffPool 或 CPU/GPU 软件基线绝对性能。

## Capabilities

### New Capabilities

- `hybrid-architecture-simulation`: 定义论文对齐的聚合/组合引擎、缓冲体系、流水策略、访存协调、操作统计和可切换消融机制。
- `paper-performance-validation`: 定义可复现的实验入口、参考指标、结果格式、误差计算和 ±20% 验收规则。

### Modified Capabilities

无。

## Impact

- 主要影响 `hygcn/dataflow/`、`hygcn/hardware/`、`hygcn/config.*`、`hygcn/graph.*`、`hygcn_test/` 和 `configs/`。
- 将新增测试、基准配置、论文参考指标和结果校验脚本。
- DRAMSim3 继续作为 HBM 时序后端，但其参数与带宽假设必须在结果中显式记录。
- 默认测试入口将从硬编码组合改为参数驱动；原有无参数批量行为可保留为兼容模式，但缺失数据集不得导致静默错误。
