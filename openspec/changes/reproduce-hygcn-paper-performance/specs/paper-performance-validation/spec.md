# Spec Delta

## Purpose

定义 HyGCN 论文性能复现的实验入口、参考指标、误差算法和自动验收规则，使结果可重复、可追踪，并能判断关键优化是否落在论文报告值的 ±20% 区间。

## ADDED Requirements

### Requirement: 参数化实验入口
系统 SHALL 提供命令行接口选择模型、数据集、层、架构配置档、流水策略、组合策略、随机种子、输出目录和消融开关。缺失的数据集、采样文件、配置或输出目录错误 MUST 产生明确错误和非零退出码，不得静默生成空结果。

#### Scenario: 单组合运行
- **WHEN** 用户指定 GCN、Cora、论文配置档和全部优化
- **THEN** 系统只运行该组合并生成带唯一实验标识的结果文件

#### Scenario: 输入缺失
- **WHEN** 用户选择不存在的数据集或采样文件
- **THEN** 系统在模拟开始前失败，并指出缺失路径

### Requirement: 可重复运行
相同代码版本、输入文件、配置档、随机种子和开关组合 SHALL 产生相同的分区、请求计数、操作计数与总周期。运行清单 MUST 记录代码版本、输入摘要和随机种子。

#### Scenario: 重复基准
- **WHEN** 同一实验连续运行两次
- **THEN** 两次结构化结果中的确定性指标完全一致

### Requirement: 论文参考指标清单
系统 SHALL 维护版本化的论文参考指标清单，记录指标名称、论文章节或图号、参考值、单位、聚合方式、实验 scope、是否强制和容差。Fig. 15/16 MUST 保存 arXiv SVG 来源 URL、SHA256、坐标提取方法和 Cora/Citeseer/PubMed 的逐数据集柱值，并按逐数据集标量验收；协调器平均 73% 时间下降（约 3.70×）和平均 4.00× 带宽提升 MUST 作为聚合指标。Fig. 15(b) MUST 验收 AE 的 Edge+Input 总 DRAM 比率；Input-only 比率 MUST 标记为诊断项，不得代替论文指标参与门禁。

#### Scenario: 加载参考指标
- **WHEN** 论文验收工具启动
- **THEN** 每个指标均能解析出来源、实验 scope、验证类型、参考值、容差、强制标记和参与聚合的数据集集合，数字化柱值还能追踪到 SVG 哈希和坐标

### Requirement: ±20% 误差验收
验收工具 SHALL 对标量参考使用 `abs(measured - reference) / abs(reference)`。每个强制论文指标 MUST 不高于 20%；任何强制指标缺失、非有限、基线为零或误差超限 MUST 使验收失败并返回非零状态。诊断项 MUST 输出但不得改变退出码。

#### Scenario: 指标通过
- **WHEN** 测量值相对论文参考值的误差不超过 20%
- **THEN** 报告该指标通过，并显示参考值、测量值和误差百分比

#### Scenario: 指标失败
- **WHEN** 任一必需指标相对误差超过 20%
- **THEN** 整体验收失败，并列出超限指标及其差值

### Requirement: 受支持论文基准矩阵
论文性能验收 SHALL 至少覆盖仓库现有且与论文重合的 Cora、Citeseer、PubMed 数据集，并通过版本化 workload manifest 将 Fig. 15-17 绑定到 Table 5 的 GCN layer 0（dataset feature width → 128）。未映射到论文的 `128 → num_class` 分类层 MUST NOT 隐式进入强制验收。GraphSAGE 和 GIN MAY 作为扩展报告，但不得计入强制 ±20% 验收。

#### Scenario: 核心矩阵完整
- **WHEN** 执行论文验收套件
- **THEN** Cora、Citeseer、PubMed 的 GCN 优化版与所需消融基线均被运行或从有效缓存加载

### Requirement: 消融实验隔离
每项论文优化的对照实验 SHALL 只改变目标机制，其他模型、数据集、硬件参数、种子和运行策略 MUST 保持一致。结果 SHALL 同时保留原始周期/字节值和计算后的比率。

#### Scenario: 稀疏消除消融
- **WHEN** 计算稀疏消除加速与 DRAM 比率
- **THEN** 两组实验固定同一图、第一层和 AE-only scope，仅在连续窗口稀疏开关上不同，且不包含 Weight、CE、Output 或中间流量

#### Scenario: 流水消融
- **WHEN** 计算引擎流水加速与 DRAM 比率
- **THEN** sequential 与目标流水策略使用相同架构参数、输入和组合策略

#### Scenario: 协调器分解消融
- **WHEN** 验收 Fig. 17
- **THEN** 报告 priority-only、mapping-only 和 combined 三组结果，分别保持非目标机制不变，并保存 priority 请求 timeline 与 mapping channel/bank 分布

#### Scenario: 带宽与总执行时间解耦
- **WHEN** 请求时间线包含等待 AE/CE producer 且没有 HBM 请求在途的空闲区间
- **THEN** 总执行周期保留该区间，带宽利用率只使用 in-flight request 区间并集，并在结果中同时记录 wall-clock memory cycles 和 active memory cycles

### Requirement: 参数差异可审计
系统 SHALL 从版本化参数基线和当前有效配置自动计算参数差异。`parameter_recalibration` MUST 由差异结果生成，不得写死；报告 MUST 包含 workload/config/source 行为参数的 baseline、current 和来源。

#### Scenario: 行为参数发生变化
- **WHEN** ping-pong 派生容量、spill alignment、依赖延迟、工作负载层或映射常数变化
- **THEN** benchmark 报告列出逐项差异并自动标记发生参数变化

#### Scenario: 图分区占用上限重标定
- **WHEN** scheduler shard cap 相对上一验收基线变化
- **THEN** 报告将其与物理 Aggregation Buffer 容量分开记录，并保存同一配置族在三个数据集上的邻近值敏感性结果

### Requirement: 因果与请求切分不变量
同一有序 block 流的内存完成时间、row hit/miss 和 channel/bank 事务计数 MUST 不受上层请求切分影响。Output 请求 MUST 在对应 CE producer-ready 后入队；Intermediate Read MUST 访问对应 Write 的同一地址和字节范围，并等待该 Write 完成。已经进入请求级时间线的 intermediate 流量 MUST NOT 再以解析延迟重复计时。

#### Scenario: 请求切分反例
- **WHEN** 同一 128 个连续 block 分别封装为一个请求和 128 个请求
- **THEN** 两次模拟的完成周期、row hit/miss、channel 和 bank 事务计数完全一致

#### Scenario: producer 和 RAW 依赖
- **WHEN** 执行流水 Output 与 sequential intermediate 流量
- **THEN** 每个 Output 的入队周期不早于 producer-ready，且每个 Intermediate Read 的地址、字节数和入队周期满足对应 Write 的 RAW 依赖

### Requirement: 自检与回归
构建系统 SHALL 注册单元测试、集成 smoke 测试和论文指标验收测试。默认快速测试 MUST 在合理时间内运行且不依赖缺失的大型数据集；完整论文验收 MUST 可单独触发并输出汇总报告。

#### Scenario: 快速自检
- **WHEN** 开发者运行默认测试命令
- **THEN** 地址、分区、调度、周期模型和小数据集集成测试全部执行并报告结果

#### Scenario: 完整论文验收
- **WHEN** 开发者显式运行论文基准目标
- **THEN** 系统生成逐数据集结果、聚合指标和 ±20% 通过/失败摘要

### Requirement: 结果声明边界
报告 MUST 区分“论文相对优化指标已校准”和“论文端到端绝对性能已复现”。在没有同口径 CPU/GPU 基线、DiffPool 实现或论文全部数据集时，系统 MUST NOT 宣称复现论文的 1509× CPU 加速、6.5× GPU 加速或完整能效结论。

#### Scenario: 生成验收报告
- **WHEN** 当前仅完成微架构相对指标验收
- **THEN** 报告明确列出已验证指标和未验证的论文结论
