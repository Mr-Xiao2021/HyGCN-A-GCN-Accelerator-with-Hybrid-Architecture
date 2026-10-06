# hygcn-rtl-prototype Specification

## Purpose

定义 HyGCN 核心机制的可综合 RTL 原型及其最低验证要求。

## ADDED Requirements

### Requirement: 可综合数据通路

系统 SHALL 提供无延时语句、无动态对象且可由开源综合器处理的 SystemVerilog 聚合和组合数据通路。

#### Scenario: SUM 聚合

- **WHEN** 同一顶点的多个 feature beat 以 `first`/`last` 标记输入
- **THEN** 输出 MUST 等于各 beat 对应 lane 的有符号求和

#### Scenario: MAX 聚合

- **WHEN** 聚合操作选择 MAX
- **THEN** 输出 MUST 等于各 beat 对应 lane 的有符号最大值

#### Scenario: 输出背压

- **WHEN** 下游拉低 ready
- **THEN** 上游 MUST 被阻塞且输出数据和元数据 MUST 保持稳定

### Requirement: 组合引擎并行

组合集群 SHALL 参数化 module 数、每 module array 数和 inner lane 数，并对所有 array 并行累加点积。

#### Scenario: 多 beat 点积

- **WHEN** 一个输出 tile 跨多个 inner beat
- **THEN** 每个 array 的结果 MUST 包含全部 beat 的 activation × weight 累加

### Requirement: 论文式请求优先级

仲裁器 SHALL 支持 FIFO 和 batch-class 两种可选择策略。

#### Scenario: batch-class 仲裁

- **WHEN** 多个请求同时有效
- **THEN** 仲裁器 MUST 优先最小 batch，并在同 batch 内按 Edge、Input、Weight、Output 排序

### Requirement: Aggregation Buffer 因果

双区缓冲控制器 MUST 阻止未完成 AE 的 batch 启动 CE，并阻止未完成 CE 的 bank 被重新分配。

#### Scenario: 双 bank 流水

- **WHEN** 一个 bank 处于 CE_ACTIVE 且另一 bank FREE
- **THEN** 新 batch MAY 分配到 FREE bank，但 MUST NOT 覆盖 CE_ACTIVE bank

### Requirement: 自动验证

仓库 SHALL 提供一条命令执行 RTL lint、确定性仿真和 smoke 综合，并在任何断言失败时返回非零状态。

#### Scenario: 完整 RTL 自检

- **WHEN** 开发者执行版本化 RTL 验证目标
- **THEN** lint、功能仿真和综合 MUST 全部执行，任一阶段失败 MUST 令目标返回非零状态
