# Spec Delta

## Purpose

定义一个可配置且可观测的 HyGCN 周期级模拟能力，使聚合引擎、组合引擎、片上缓冲、跨引擎流水和访存协调能够按论文配置运行，并支持受控消融而不改变实验输入。

## ADDED Requirements

### Requirement: 论文架构配置档
系统 SHALL 提供论文配置档，包含 32 个 SIMD16 聚合核心、8 个组合模块、每个组合模块 4 个 1×128 计算阵列、1 GHz 名义频率、128 KB Input Buffer、2 MB Edge Buffer、2 MB Weight Buffer、4 MB Output Buffer 和 16 MB Aggregation Buffer。系统 MUST 同时提供较小的 smoke 配置，但结果中 MUST 标明所使用的配置档。

#### Scenario: 加载论文配置
- **WHEN** 用户选择论文配置档启动模拟
- **THEN** 运行清单记录全部计算、缓冲、频率和 HBM 参数，并拒绝缺失或非正值参数

#### Scenario: 加载快速配置
- **WHEN** 用户选择 smoke 配置执行快速自检
- **THEN** 模拟器使用缩小参数运行，并将结果标记为不可用于论文性能验收

### Requirement: 聚合引擎语义
聚合引擎 SHALL 使用 Vertex-Disperse 策略把一个顶点的特征向量分块分配给可用 SIMD 核心，并 MUST 区分 SUM 类聚合与 MAX 类聚合的操作计数和周期模型。聚合引擎 SHALL 报告有效操作、空闲周期、输入特征字节、边字节和聚合缓冲读写字节。

#### Scenario: 高维顶点分散执行
- **WHEN** 单个顶点的特征维度超过一个 SIMD16 的宽度
- **THEN** 特征块被分派给多个 SIMD 核心，且汇总操作数等于有效边与有效特征元素所要求的操作数

#### Scenario: GraphSAGE MAX 聚合
- **WHEN** 模型声明 MAX 聚合
- **THEN** 模拟器记录比较操作而非 MAC 操作，并使用 MAX 聚合对应的周期模型

### Requirement: 组合引擎多模块执行
组合引擎 SHALL 建模 8 个组合模块以及每模块 4 个 1×128 阵列，并 SHALL 支持 independent 与 cooperative 两种执行策略。权重装载、输入推进、流水线填充、有效 MAC、输出写回和模块利用率 MUST 分别计数。

#### Scenario: Independent 模式
- **WHEN** 一个权重分块能够独立分配到组合模块
- **THEN** 不同模块处理不同输入批次，并独立记录权重读取和完成周期

#### Scenario: Cooperative 模式
- **WHEN** 用户选择 cooperative 策略
- **THEN** 多个模块共享同一批输入并按列拆分输出，权重级联流量与并行完成周期被显式统计

### Requirement: Interval/Shard 与动态稀疏消除
系统 SHALL 按 Edge Buffer、Input Buffer 和 Aggregation Buffer 容量生成可审计的 Interval/Shard 划分。动态稀疏消除启用时，系统 MUST 仅请求 shard 中实际引用的唯一源顶点特征；禁用时 MUST 请求完整 interval，从而形成可比较基线。

#### Scenario: 启用稀疏消除
- **WHEN** shard 只引用 interval 中部分源顶点
- **THEN** 输入特征请求集合等于实际唯一邻居集合，且报告被跳过的无效顶点数量和字节数

#### Scenario: 禁用稀疏消除
- **WHEN** 同一实验关闭稀疏消除
- **THEN** 模拟器请求完整 interval 的输入特征，同时保持图、层、硬件配置和其他开关不变

### Requirement: 跨引擎流水策略
系统 SHALL 提供 sequential、latency-aware 和 energy-aware 三种 AE/CE 调度策略。latency-aware SHALL 在可形成合法组合批次时尽快启动 CE；energy-aware SHALL 优先形成可复用权重的较大批次。任一策略 MUST 保证 CE 只读取已完成聚合的顶点且 Aggregation Buffer 不被未完成消费者覆盖。

#### Scenario: Latency-aware 流水
- **WHEN** Aggregation Buffer 中出现可消费顶点且 CE 可接收工作
- **THEN** CE 在无需等待完整 shard 的情况下启动，并保持数据依赖正确

#### Scenario: Energy-aware 流水
- **WHEN** Aggregation Buffer 容量允许累计目标批次
- **THEN** CE 等待至目标批次或容量边界后启动，以减少每个顶点分摊的权重读取

#### Scenario: Sequential 基线
- **WHEN** 流水被禁用
- **THEN** CE 仅在当前聚合阶段全部完成后启动，作为流水加速比的基线

### Requirement: Batch-aware 访存协调
Memory Access Coordinator SHALL 支持 Edge、Input、Weight、Output 四类 HBM 请求及 Aggregation Buffer 请求。对于同一 batch，优先级 MUST 为 Edge、Input、Weight、Output；当前 batch 的低优先级请求 MUST 先于后续 batch 的高优先级请求完成发射。系统 SHALL 报告各类队列等待、HBM 接收阻塞、有效带宽和地址映射分布。

#### Scenario: 同批次优先级
- **WHEN** 同一 batch 同时存在 Edge、Input、Weight 和 Output 请求
- **THEN** 协调器按 Edge、Input、Weight、Output 顺序选择可发射请求

#### Scenario: 跨批次公平性
- **WHEN** 当前 batch 存在 Output 请求且后续 batch 到达 Edge 请求
- **THEN** 当前 batch 的 Output 请求不会被后续 batch 的 Edge 请求无限推迟

### Requirement: 地址与流量统计正确性
所有内存请求 SHALL 使用对应数据对象的实际字节数，顶点输出地址 MUST 按顶点步长与输出分块计算。系统 MUST 检测区域越界、重叠、负事件计数和未完成事务，并在错误时以非零状态终止。

#### Scenario: Edge chunk 流量
- **WHEN** Edge Buffer 载入一个不足容量上限的 edge chunk
- **THEN** 发射字节数等于该 chunk 的编码后实际大小，而不是 Weight Buffer 或其他对象大小

#### Scenario: 输出顶点地址
- **WHEN** 连续两个顶点写回同一输出分块
- **THEN** 两个首地址之差等于对齐后的单顶点输出步长

### Requirement: 统一运行统计
每次模拟 SHALL 输出机器可读的运行清单与层级统计，至少包括总周期、AE/CE 结束周期、各引擎利用率、各数据类型 DRAM/eDRAM/SRAM 字节、有效操作数、请求等待周期、HBM 带宽利用率、配置档和全部消融开关。

#### Scenario: 完整统计输出
- **WHEN** 一层模拟正常结束
- **THEN** CSV 或 JSON 结果包含规范要求的字段，且各字节与操作计数非负并通过内部守恒检查
