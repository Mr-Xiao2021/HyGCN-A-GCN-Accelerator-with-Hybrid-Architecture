# Spec Delta

## Purpose

定义 MEGA Degree-Aware 混合精度输入的可复跑契约，使逐节点特征位宽、scale、权重精度和来源能够被模拟器、验证器及性能报告一致解释，并避免用未披露的启发式位宽生成论文级结论。

## ADDED Requirements

### Requirement: 版本化量化配置

系统 SHALL 使用版本化 manifest 描述每个数据集、模型和层的图哈希、度数定义、特征位宽映射、特征 scale、权重位宽、权重 scale 及生成来源。特征位宽 MUST 位于 1 至 8 bit；论文默认权重位宽 MUST 为 4 bit。

#### Scenario: 加载完整配置
- **WHEN** 用户运行具备完整字段且图哈希匹配的量化 manifest
- **THEN** 系统接受该配置，并在结果中记录 manifest 版本、哈希和来源类别

#### Scenario: 拒绝不匹配配置
- **WHEN** manifest 缺少层、存在非法位宽或图哈希与输入图不一致
- **THEN** 系统终止 required benchmark，并明确报告不匹配字段

### Requirement: Degree-Aware 量化语义

系统 SHALL 根据节点入度选择对应位宽和 scale，使用符号保持、最近整数舍入和有符号范围饱和生成整数特征。组合阶段权重 SHALL 按输出列使用独立 scale；聚合阶段输入 SHALL 使用版本化配置中声明的量化位宽。

#### Scenario: 不同度数选择不同参数
- **WHEN** 两个节点的入度映射到不同的量化项
- **THEN** 系统使用各自的位宽和 scale，并在 trace 中保留可核验的参数索引

#### Scenario: 饱和边界
- **WHEN** 特征绝对值超过当前位宽和 scale 可表示的最大值
- **THEN** 系统将整数值饱和到对应有符号边界，且不会发生静默溢出

### Requirement: 实际位宽计费

系统 MUST 使用每个节点的实际位宽计算特征存储、传输和 bit-serial 计算周期；bitmap、scale、package header、padding 和节点边界元数据 MUST 单独计费，不得按理想压缩大小替代实际 DRAM 流量。

#### Scenario: 混合位宽流量
- **WHEN** 同一层包含 2 bit、3 bit 和 4 bit 节点
- **THEN** 报告分别给出有效值位、格式开销和对齐后 DRAM bytes，并能重算总量

#### Scenario: bit-serial 周期
- **WHEN** 其余条件相同而节点位宽从 2 bit 增加到 4 bit
- **THEN** 组合计算发射工作量按实际有效 bit 数增加，且变化可从周期分解中观察

### Requirement: 证据来源分级

系统 SHALL 将量化输入标记为 `paper-derived`、`locally-trained` 或 `diagnostic-heuristic`。只有前两类且具备完整 manifest 的结果可进入 required 结果表；启发式配置 MUST 仅作为敏感性或开发诊断。

#### Scenario: 启发式配置运行
- **WHEN** 用户使用根据平均位宽反推的启发式度数映射
- **THEN** 系统允许生成诊断结果，但 required 验收 SHALL 标记为不可判定而非通过

