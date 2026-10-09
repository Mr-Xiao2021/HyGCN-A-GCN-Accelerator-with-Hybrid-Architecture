# Spec Delta

## Purpose

定义 MEGA Adaptive-Package 的可逆编码、实际存储开销和 DRAM 访问计量方式，使混合精度稀疏特征的压缩收益能够在没有免费元数据或理想化对齐假设的情况下被独立复算。

## ADDED Requirements

### Requirement: Adaptive-Package 格式

系统 SHALL 支持总长度为 64、128 和 192 bit 的 short、medium 和 long package。每个 package MUST 包含 2 bit Mode、3 bit Bitwidth 和自适应 Val Array；Mode `00`、`01`、`10` 分别对应三种长度，`11` MUST 被拒绝。

#### Scenario: 三种合法模式
- **WHEN** 编码器分别选择 short、medium 和 long 模式
- **THEN** 输出 package 的总位数、Mode 和 Bitwidth 与配置一致，且不超过对应容量

#### Scenario: 非法模式
- **WHEN** 解码器读取 Mode 为 `11` 的 package
- **THEN** 解码失败并报告 package 位置，不得继续产生特征值

### Requirement: 同位宽连续节点装包

系统 MUST 仅将相同位宽的非零值写入同一个 package，并按节点顺序连续装入，直到 package 容量不足或下一节点位宽变化。位宽变化 MUST 关闭当前 package，剩余位计为 padding。

#### Scenario: 位宽切换
- **WHEN** 连续两个节点分别使用 2 bit 和 3 bit
- **THEN** 第二个节点从新 package 开始，前一 package 的剩余空间计入 padding

#### Scenario: 相同位宽跨节点复用
- **WHEN** 多个连续节点位宽相同且非零值能够放入当前 package
- **THEN** 编码器继续使用当前 package，直至达到容量边界

### Requirement: 稀疏位置与节点边界可逆

系统 SHALL 为每个节点保存非零 bitmap，并保存足以恢复节点边界和 package 消费位置的元数据。解码结果 MUST 与量化后的原始整数张量逐元素一致。

#### Scenario: 编解码往返
- **WHEN** 输入包含全零节点、稠密节点、混合稀疏度和混合位宽
- **THEN** 解码后的整数值、节点边界、位宽和 bitmap 与编码输入完全一致

#### Scenario: 截断数据
- **WHEN** package、bitmap 或边界元数据被截断
- **THEN** 解码器拒绝该输入，并指出未满足的长度或计数约束

### Requirement: 实际 DRAM 访问计量

系统 MUST 分别报告有效值、header、padding、bitmap、边界元数据和 scale 的逻辑字节，并按版本化 DRAM transaction granularity 计算实际传输 bytes 与 transactions。required profile 的粒度来源和敏感性 MUST 出现在报告中。

#### Scenario: 跨 transaction 边界
- **WHEN** 编码流长度超过一个 DRAM transaction 且未对齐
- **THEN** 实际传输量向上对齐，同时保留未对齐逻辑字节和浪费字节

#### Scenario: 包长度敏感性
- **WHEN** 用户扫描多组三档 package 长度
- **THEN** 系统对每组配置重算 padding、transactions 和 DRAM bytes，且不会复用默认配置的统计结果

