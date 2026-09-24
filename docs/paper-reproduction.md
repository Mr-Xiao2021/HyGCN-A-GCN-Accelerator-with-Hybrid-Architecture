# HyGCN 论文机制复刻说明

本文档描述 `run/npu` 分支对 *HyGCN: A GCN Accelerator with Hybrid Architecture*
核心机制的模块级复刻与验收口径。

## 已实现机制

- 论文架构档：32 个 SIMD16 聚合核心、8 个组合模块、每模块 4 条 1x128 阵列，以及论文容量的片上缓冲和 256 GB/s HBM。
- Interval/Shard：按 Edge、Input 和 Aggregation Buffer 容量分区，记录目标顶点、实际 edge 编码字节、源区间和唯一邻居。
- Window Sliding & Shrinking：稀疏开启时按真实唯一邻居地址生成请求，仅合并连续顶点；关闭时请求完整 interval，流量和地址使用同一语义。
- Vertex-Disperse：高维特征分散到多个 SIMD，低维特征并行处理多个顶点；SUM 与 MAX 分开计数。
- Combination Module：显式计算矩阵 tile、活动模块、batch wave、权重装载/级联、输入推进、流水填充、MAC 和输出写回周期。
- Independent/Cooperative：两种模式都只从 HBM 装载一次权重并由 Weight Buffer 复用；前者把 ready batch 分配给可用模块，后者共享输入并按输出列协作。
- Aggregation Buffer：按 ping-pong 半区形成合法分区，在 batch 时间线上执行 ready、consume、reclaim；容量不足会阻塞 AE，CE 完成后才释放空间。
- AE/CE 策略：sequential 使用真实的一次中间写出和一次读回；latency-aware 在单个合法 batch ready 后启动；energy-aware 累积到目标顶点数或容量边界。
- Memory Access Coordinator：请求级跟踪 channel 发射、bank 可用周期和 row-buffer hit/miss。开启时执行最早 batch 仲裁与低位 channel/bank 交织，关闭时使用 FIFO 与 row-first 映射。

## 配置档

- `configs/HYGCN_PAPER.ini`：论文结构参数，以及带单位的 HBM channel/bank/row 时序参数。
- `configs/HYGCN_LEGACY.ini`：原始实现对应的兼容配置说明。
- `configs/HYGCN_SMOKE.ini`：默认测试使用的缩小配置，不可作为论文性能结论。
- `configs/paper_metrics.json`：版本化论文参考值、来源、单位、聚合规则和 20% 容差。

## 验收方法

基准对每个机制执行成对实验，除目标开关外，模型、数据集、配置、种子和其他策略保持一致：

- 稀疏：`sparsity=on` 对比 `off`。
- 流水：`latency-aware` 对比 `sequential`。
- 协调器：`coordination=on` 对比 `off`。

参考清单按论文证据类型分别处理：

- 稀疏加速 `1.1-3.0x`、流水加速 `1.369863-2.127660x` 和流水 DRAM 比率 `0.50-0.73` 按逐数据集范围验收。
- 协调器平均 `3.70x` 加速和 `4.00x` 带宽提升按 Cora、Citeseer、PubMed 算术平均验收。
- Fig. 15(b) 的稀疏输入 DRAM 比率尚无可追踪的逐柱数字化值，只输出诊断结果，不影响门禁。

标量参考的相对误差为：

```text
abs(measured - reference) / abs(reference)
```

范围指标在范围内记零误差，范围外相对最近边界计算误差。任一强制指标缺失、非有限或误差超过 20% 时，验收命令返回非零。

## 2026-09-24 自检结果

| 指标 | 论文参考 | 模拟结果 | 验收 |
|---|---:|---:|---|
| 稀疏消除加速（Cora/Citeseer/PubMed） | 1.10-3.00x | 1.0555/2.0832/1.7416x | PASS，最大边界误差 4.05% |
| 稀疏输入 DRAM 比率 | 诊断项 | 0.9134/0.4400/0.5597 | 不参与门禁 |
| 跨引擎流水加速（Cora/Citeseer/PubMed） | 1.3699-2.1277x | 1.4747/1.6545/1.6949x | PASS |
| 流水 DRAM 比率（Cora/Citeseer/PubMed） | 0.50-0.73 | 0.4873/0.5695/0.5772 | PASS，最大边界误差 2.53% |
| 访存协调平均加速 | 3.70x | 3.2622x | PASS，误差 11.83% |
| 平均带宽利用率提升 | 4.00x | 4.0900x | PASS，误差 2.25% |

## 声明边界

当前结果表示 GCN 三数据集上的核心优化已达到请求级相对性能复现门槛，不表示 Ramulator/RTL 周期精度或端到端绝对性能完全复现。
本仓库没有同口径 CPU/GPU 软件基线、DiffPool、论文全部数据集、RTL、综合、面积或完整芯片能耗模型，
因此不宣称复现论文的 1509x CPU 加速、6.5x GPU 加速或完整能效结论。
