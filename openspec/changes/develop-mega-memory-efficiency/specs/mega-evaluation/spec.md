# Spec Delta

## Purpose

定义 MEGA 请求级周期模型、论文配置、基线消融和最终验收口径，确保 DRAM access 减少与总时钟数提升能够由同一 workload、同一内存系统和完整因果 trace 独立复算。

## ADDED Requirements

### Requirement: 论文默认硬件配置

系统 SHALL 提供版本化 MEGA paper profile：1 GHz、256 GB/s HBM、392 KB 总 buffer，以及 64 KB Input、24 KB Edge、48 KB Weight、96 KB Combination、128 KB Aggregation、32 KB Sparse Buffer；处理单元 SHALL 配置为 4×8×32 BSE 和 256 个 Aggregation Unit。

#### Scenario: 加载 paper profile
- **WHEN** 用户选择 MEGA paper profile
- **THEN** 运行清单完整记录全部处理、buffer、DRAM 和 package 参数及其论文来源

#### Scenario: 参数覆盖
- **WHEN** 用户覆盖任一影响周期或流量的参数
- **THEN** 报告列出默认值、新值和指标变化，并将运行标记为敏感性或自定义配置

### Requirement: 因果一致的总时钟模型

系统 MUST 分别建模 Combination、Aggregation、Encoder、Decoder、Condense Unit、buffer 背压和 DRAM 服务时间，并根据 producer-ready、队列 admission、command issue 和 completion 因果关系计算总时钟数。计算和访存重叠只能在真实并行资源允许时发生。

#### Scenario: DRAM 延迟增加
- **WHEN** 同一 workload 仅增加 DRAM service latency
- **THEN** 任何消费者不得提前开始，总时钟数不得因该变化下降

#### Scenario: 位宽降低
- **WHEN** 同一量化整数结果可用更低位宽表示且其他配置不变
- **THEN** bit-serial 工作量和特征流量不增加，变化在周期与流量分解中可追踪

### Requirement: 公平基线与逐机制消融

系统 SHALL 在相同图、层 shape、partition、DRAM profile 和 buffer 总容量下运行至少五种配置：32 bit `A(XW)` 基线、Degree-Aware+Bitmap、Degree-Aware+Adaptive-Package、再加入 Condense-Edge 的完整 MEGA，以及现有 HyGCN 路径。baseline-only throttle 或只对某一配置生效的隐藏带宽限制 MUST 被禁止。

#### Scenario: 运行完整消融
- **WHEN** 用户执行强制 MEGA benchmark
- **THEN** 每个数据集生成五种配置的独立原始结果及相邻阶段增量，不从最终比值反推中间结果

#### Scenario: 检测不公平参数
- **WHEN** baseline 与 optimized 运行的共享硬件参数不同且该差异不属于被消融机制
- **THEN** validator 拒绝 required 对比并列出差异

### Requirement: DRAM 与周期指标

系统 MUST 按数据集、模型、层、请求类别和机制配置报告逻辑 bytes、实际 DRAM bytes、transactions、总时钟数、计算周期、内存阻塞周期、编码解码周期和 Condense 周期。speedup 与 DRAM reduction SHALL 从原始计数计算并附带分母定义。

#### Scenario: 独立重算
- **WHEN** validator 读取运行清单和请求/周期摘要
- **THEN** 能独立重算总 DRAM bytes、总时钟数、speedup 和 DRAM reduction，并与报告一致

#### Scenario: 部分 workload 缺失
- **WHEN** NELL、Reddit 或所需量化 manifest 不可用
- **THEN** 报告将结论限定为已运行数据集，不得使用论文全 workload 平均值宣称整体复现

### Requirement: 分级验收与论文目标

系统 SHALL 将验收分为机制正确性、局部收益和论文数值三层。机制层要求编解码、流量和因果测试通过；局部收益层要求完整 MEGA 在每个 required workload 上相对 32 bit `A(XW)` 基线同时减少 DRAM bytes 和总时钟数；论文数值层仅在论文 workload、量化配置和比较口径齐全时，才对论文报告的平均 speedup 与 DRAM reduction 执行误差判定。

#### Scenario: 只有局部收益通过
- **WHEN** 完整 MEGA 在现有数据集上减少 DRAM 和周期，但缺少论文逐节点量化参数或完整 workload
- **THEN** 结果标记为“机制通过/局部复现”，不得标记为完整论文数值复现

#### Scenario: 论文级判定
- **WHEN** workload、来源、配置和对比口径均满足论文级条件
- **THEN** validator 对论文报告的平均 speedup、DRAM reduction 及消融指标执行版本化容差检查并逐项报告

### Requirement: 既有路径不回退

系统 SHALL 保持现有 HyGCN `legacy`、`paper`、报告和 RTL 流程的默认输出与测试行为不变，MEGA 结果 MUST 写入独立目录。

#### Scenario: 全量回归
- **WHEN** MEGA 变更完成后执行默认 CTest 和既有强制回归
- **THEN** 既有测试通过，且 MEGA 产物不会覆盖 `res/paper`、`res/report` 或 `res/pyg-cpu`
