# HyGCN 论文机制复刻说明

本文档描述 `run/npu` 分支对 *HyGCN: A GCN Accelerator with Hybrid Architecture*
核心机制的模块级复刻与验收口径。

## 已实现机制

- 论文架构档：32 个 SIMD16 聚合核心、8 个组合模块、每模块 4 条 1x128 阵列，以及论文容量的片上缓冲和 256 GB/s HBM。
- Interval/Shard：按 Edge、Input 和 Aggregation Buffer 容量分区，记录目标顶点、实际 edge 编码字节、源区间和唯一邻居。
- Window Sliding & Shrinking：稀疏开启时只请求唯一邻居，关闭时请求完整 interval。
- Vertex-Disperse：高维特征分散到多个 SIMD，低维特征并行处理多个顶点；SUM 与 MAX 分开计数。
- Combination Module：显式计算矩阵 tile、活动模块、batch wave、权重装载/级联、输入推进、流水填充、MAC 和输出写回周期。
- Independent/Cooperative：前者让模块处理不同 batch，后者共享输入并按输出列拆分，保留相同有效 MAC 和输出字节。
- Aggregation Buffer：有界环形阶段模型，执行 allocate、ready、consume、reclaim，并检查环绕、容量和顺序。
- AE/CE 策略：sequential、latency-aware、energy-aware 共享数据依赖检查，并记录 AE 完成、CE 启动和 CE 完成周期。
- Memory Access Coordinator：最早 batch 优先，batch 内按 Edge、Input、Weight、Output 仲裁，并统计等待、阻塞、带宽和地址分布。

## 配置档

- `configs/HYGCN_PAPER.ini`：论文结构参数和集中记录的实现校准参数。
- `configs/HYGCN_LEGACY.ini`：原始实现对应的兼容配置说明。
- `configs/HYGCN_SMOKE.ini`：默认测试使用的缩小配置，不可作为论文性能结论。
- `configs/paper_metrics.json`：版本化论文参考值、来源、单位、聚合规则和 20% 容差。

## 验收方法

基准对每个机制执行成对实验，除目标开关外，模型、数据集、配置、种子和其他策略保持一致：

- 稀疏：`sparsity=on` 对比 `off`。
- 流水：`latency-aware` 对比 `sequential`。
- 协调器：`coordination=on` 对比 `off`。

引用值是论文给出的跨数据集平均结果，因此强制门禁使用 Cora、Citeseer、PubMed 的算术平均；
逐数据集结果作为拓扑敏感性诊断保留在报告中。相对误差为：

```text
abs(measured - reference) / abs(reference)
```

任一聚合指标缺失、非有限或误差超过 20% 时，验收命令返回非零。

## 2026-09-24 自检结果

| 指标 | 论文参考 | 模拟结果 | 相对误差 |
|---|---:|---:|---:|
| 稀疏消除加速 | 1.10x | 1.2596x | 14.51% |
| 稀疏输入 DRAM 比率 | 0.60x | 0.6319x | 5.32% |
| 跨引擎流水加速 | 1.10x | 1.1931x | 8.46% |
| 流水 DRAM 比率 | 0.50x | 0.5607x | 12.14% |
| 访存协调加速 | 1.10x | 1.0217x | 7.12% |
| 带宽利用率提升 | 1.10x | 1.0217x | 7.12% |

## 声明边界

当前结果表示论文相对微架构效应已在模块级模拟器中校准，不表示端到端绝对性能完全复现。
本仓库没有同口径 CPU/GPU 软件基线、DiffPool、论文全部数据集、RTL、综合、面积或完整芯片能耗模型，
因此不宣称复现论文的 1509x CPU 加速、6.5x GPU 加速或完整能效结论。
