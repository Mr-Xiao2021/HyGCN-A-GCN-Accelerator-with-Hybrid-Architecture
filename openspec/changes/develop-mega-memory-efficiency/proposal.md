# Proposal

## Why

当前请求级模拟器仍以定长、高精度特征和传统分区访问为主，无法表达 MEGA 论文中按节点度数分配精度、按实际位宽打包稀疏特征以及重排跨分区稀疏边的核心机制，因此不能可靠评估这些机制对 DRAM access 和总时钟数的贡献。需要建立一条与现有 HyGCN 路径隔离、来源和假设可审计、能够做逐机制消融的 MEGA 执行路径。

## What Changes

- 新增版本化 Degree-Aware 量化配置，描述逐数据集、模型、层和度数对应的特征位宽与 scale，并固定权重 4 bit 规则。
- 新增 Adaptive-Package 编码/解码与流量模型，支持 64/128/192 bit 三种 package、独立 bitmap、跨连续节点装包及精确 header/padding 计费。
- 新增 Condense-Edge 离线预处理和请求级调度模型，对跨子图稀疏连接所需的源节点去重、连续重排并显式计入 Sparse Buffer 与 DRAM 流量。
- 新增 MEGA 组合、聚合和流水时钟模型，按论文 28 nm、1 GHz、256 GB/s、392 KB buffer 和处理单元配置建立默认 profile。
- 新增基线、逐机制消融、敏感性和强制验收流程，分别报告 DRAM bytes/transactions、总时钟数及其分解，不把启发式输入或校准结果表述为独立论文复现。
- 保持现有 `legacy`、`paper` 和 RTL 路径行为不变；MEGA 通过独立 engine/profile 和结果目录接入。

## Capabilities

### New Capabilities

- `mega-degree-aware-quantization`: 定义按节点度数选择混合精度、量化配置来源及计算/存储位宽计费契约。
- `mega-adaptive-package`: 定义混合精度稀疏特征的 Adaptive-Package 编解码、边界和 DRAM 流量计算契约。
- `mega-condense-edge`: 定义跨子图稀疏连接的去重重排、Sparse Buffer 行为和访问守恒契约。
- `mega-evaluation`: 定义 MEGA 周期模型、基线与消融、指标输出、敏感性和交付验收契约。

### Modified Capabilities

无。

## Impact

- 新增 `mega/` 请求级模型、`configs/mega_*` 配置、`tools/mega_*` 预处理与验收工具、MEGA 专用测试和结果目录。
- 扩展 CLI/CMake，使 MEGA benchmark 可独立构建和复跑；既有 HyGCN 默认命令和结果格式不发生破坏性变化。
- 可选依赖包括 PyTorch/PyG 量化训练环境与 METIS 预处理工具；核心 C++ 模拟与已冻结 manifest 的复跑不依赖训练环境。
- 论文作者未公开逐节点位宽、scale 和完整 cycle simulator 时，相关输入 MUST 标注为本地训练、论文推导或诊断假设，禁止隐式目标拟合。
