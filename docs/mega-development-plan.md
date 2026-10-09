# MEGA 开发技术方案

本分支以论文 [MEGA: A Memory-Efficient GNN Accelerator Exploiting Degree-Aware Mixed-Precision Quantization](https://arxiv.org/html/2311.09775v1) 为来源，在现有 HyGCN 请求级模拟器上增加独立 `mega` engine。最终目标是用完整请求和时钟因果证据证明 DRAM access 减少与总时钟数下降，而不是通过调整基线限速贴合论文柱值。

完整行为契约和实现决策位于：

- `openspec/changes/develop-mega-memory-efficiency/proposal.md`
- `openspec/changes/develop-mega-memory-efficiency/specs/`
- `openspec/changes/develop-mega-memory-efficiency/design.md`
- `openspec/changes/develop-mega-memory-efficiency/tasks.md`

## 核心机制

1. **Degree-Aware mixed precision**：按节点入度选择 1-8 bit 特征位宽和 scale，权重使用逐输出列 scale 的 4 bit 表示。训练产物通过版本化 manifest 输入；启发式映射只能做诊断。
2. **Adaptive-Package**：实现 64/128/192 bit 三档 package、2 bit Mode、3 bit Bitwidth、独立 bitmap 和可逆节点边界流，精确统计 header、padding 和 DRAM 对齐开销。
3. **Condense-Edge**：基于冻结 partition 清单，对每个目标子图的跨子图源节点去重并连续重排；显式建模 16 个 8-entry eID FIFO、32 KB Sparse Buffer、spill/refill 和匹配周期。
4. **MEGA cycle engine**：组合阶段使用 4×8×32 BSE row-product bit-serial 模型，聚合阶段使用 256 AU outer-product 模型，复用现有 HBM command timing 和完整 producer trace。

## 公平对比

固定消融顺序如下：

| 配置 | 含义 |
|---|---|
| M0 | 32 bit `A(XW)`，无压缩、无 Condense |
| M1 | Degree-Aware + Bitmap |
| M2 | M1 + Adaptive-Package |
| M3 | M2 + Condense-Edge，完整 MEGA |

M0-M3 共用图、层 shape、partition、HBM、buffer 总容量和请求时序。现有 HyGCN 作为跨架构 H0 参考单列，不允许增加 baseline-only throttle。

## 指标与验收

每个 workload 必须输出并可独立重算：

- logical bytes、DRAM bytes 和 transactions，按 Input、Weight、Edge、Bitmap、Package、Sparse spill/refill、Output 分类；
- Combination、Aggregation、Encoder、Decoder、Condense、memory stall 和 total cycles；
- M1/M0、M2/M1、M3/M2 的相邻增量，以及 M3/M0 的最终 speedup 和 DRAM reduction；
- 量化、partition、package 和硬件配置的来源、哈希及参数 diff。

第一阶段要求 Cora、CiteSeer、PubMed 上 M3 相对 M0 同时降低 DRAM bytes 和 total cycles。论文的平均 `38.3x` HyGCN speedup 与 `108.1x` DRAM reduction 先作为 reference；在缺少完整五数据集 workload 和合格量化来源时，不作为“完整论文复现”结论。

## 开发阶段

1. 量化 manifest、reference quantizer 和本地训练导出。
2. Adaptive-Package 位精确 codec 与流量 oracle。
3. Condense-Edge planner、在线调度和访问守恒。
4. MEGA 计算/buffer/DRAM 周期引擎。
5. M0-M3 强制 benchmark、敏感性与独立 validator。
6. clean build、全量回归、最终报告和远端交付。

## 当前边界

当前提交完成技术方案、OpenSpec 契约和论文 reference manifest，尚未生成 MEGA 性能结果。28 nm 面积/功耗、1 GHz STA 和论文训练精度不属于第一阶段交付，后续没有 PDK、标准单元库和作者训练产物时不会作无依据声明。

## 方案自检

```bash
python3 tools/validate_mega_reference.py
openspec validate develop-mega-memory-efficiency --strict --no-interactive
```
