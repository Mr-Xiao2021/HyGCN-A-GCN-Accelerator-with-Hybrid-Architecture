# Spec Delta

## Purpose

定义 Condense-Edge 对跨子图稀疏连接的识别、去重、连续重排和缓冲区流量守恒，使其带来的 DRAM transaction 减少来自真实空间局部性，而不是漏计源节点或隐藏搬运开销。

## ADDED Requirements

### Requirement: 版本化分区与稀疏连接清单

系统 SHALL 从版本化 partition manifest 读取每个节点所属子图、图哈希、分区工具和参数，并为每个目标子图生成按源节点 ID 升序排列的跨子图唯一源节点清单。

#### Scenario: 构建跨子图清单
- **WHEN** 一个源节点通过多条边连接到同一目标子图
- **THEN** 该源节点在目标子图清单中仅出现一次，同时保留其服务的全部边计数

#### Scenario: 图版本不匹配
- **WHEN** partition manifest 的图哈希与当前邻接矩阵不一致
- **THEN** required benchmark 终止，且不得自动重新分区后沿用旧基线

### Requirement: 连续重排与一次存储

系统 MUST 在源节点组合结果产生时，将其写入 Combination Buffer；若该节点出现在某目标子图的跨子图清单中，系统 SHALL 同时按该目标子图的连续地址顺序写入 Sparse Buffer，并在同一目标子图内仅存储一次。

#### Scenario: 同一源节点服务多个目标子图
- **WHEN** 一个源节点被两个目标子图的稀疏连接引用
- **THEN** 系统在两个目标子图各自区域中各存一份，并分别计入写流量

#### Scenario: 无跨子图引用
- **WHEN** 一个源节点不在任何跨子图清单中
- **THEN** 系统仅写 Combination Buffer，不产生 Sparse Buffer 写入

### Requirement: eID FIFO 匹配语义

系统 SHALL 为每个并行目标子图建模 8-entry eID FIFO，并仅比较 FIFO 首项与当前源节点 ID。FIFO refill、比较、失配和命中周期 MUST 进入 Condense Unit 周期统计。

#### Scenario: 首项命中
- **WHEN** 当前源节点 ID 等于某 eID FIFO 的有效首项
- **THEN** 系统弹出该 ID、发起对应重排写入并推进连续地址指针

#### Scenario: 清单超过八项
- **WHEN** 某目标子图仍有超过 8 个待匹配 ID
- **THEN** 系统通过可计时、可计流量的 refill 补充 FIFO，不得视为无限容量比较器

### Requirement: Sparse Buffer 容量与流量守恒

系统 MUST 显式建模 Sparse Buffer 容量、区域占用、溢出写回和后续读取。Condense-Edge 的总流量 SHALL 包含 Combination Buffer、Sparse Buffer 以及所有 DRAM refill/spill，不得只统计聚合读取侧收益。

#### Scenario: 区域写满
- **WHEN** 某目标子图区域无法容纳下一条重排特征
- **THEN** 系统将已占用数据写回 DRAM、记录 transaction 和阻塞周期，再复用该区域

#### Scenario: 访问守恒
- **WHEN** Condense-Edge 与无重排基线处理相同图和量化特征
- **THEN** 两者消费相同的逻辑边和源节点值，且报告能够从请求 trace 重算全部 DRAM bytes

