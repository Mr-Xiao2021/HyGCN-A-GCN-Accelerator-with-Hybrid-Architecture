# HyGCN 论文机制复刻说明

本文档描述 `run/npu` 分支对 *HyGCN: A GCN Accelerator with Hybrid Architecture*
核心机制的模块级复刻与验收口径。

## 已实现机制

- 论文架构档：32 个 SIMD16 聚合核心、8 个组合模块、每模块 4 条 1x128 阵列，以及论文容量的片上缓冲和 256 GB/s HBM。
- Interval/Shard：按 Edge、Input 和 Aggregation Buffer 的可驻留容量分区，记录目标顶点、实际 edge 编码字节、源区间和唯一邻居。论文档物理容量不变；请求模型把 Input/Edge 的 ping-pong 半区和 4 MiB Aggregation 单 shard 驻留上限作为派生值写入 JSON。
- Window Sliding & Shrinking：稀疏开启时先滑到首个有效行，再收缩到末个有效行之后，发出一个连续 `[window_start, window_end)` 请求；窗口内部空洞仍被读取。关闭时请求完整 interval，流量和地址使用同一语义。
- Vertex-Disperse：高维特征分散到多个 SIMD，低维特征并行处理多个顶点；SUM 与 MAX 分开计数。
- Combination Module：显式计算矩阵 tile、活动模块、batch wave、权重装载/级联、输入推进、流水填充、MAC 和输出写回周期；单 batch 内的顶点组可并行占用 8 个模块，跨 batch 的 producer/RAW 依赖仍由时间线约束。
- Independent/Cooperative：两种模式都只从 HBM 装载一次权重并由 Weight Buffer 复用；前者把 ready batch 分配给可用模块，后者共享输入并按输出列协作。
- Aggregation Buffer：按 ping-pong 半区形成合法分区，在 batch 时间线上执行 ready、consume、reclaim；容量不足会阻塞 AE，CE 完成后才释放空间。
- AE/CE 策略：sequential 在 AE producer-ready 后把中间结果写到 HBM，读请求访问相同地址并等待写完成，随后 CE 启动；latency-aware 在合法 batch ready 后启动；energy-aware 累积到目标顶点数或容量边界。Output 仅在对应 CE group 完成后入队。
- Memory Access Coordinator：先把请求展平为统一 block 事务流，再跟踪 channel 发射、bank 可用周期和 row-buffer hit/miss，因此同一地址/事务序列的完成时间不依赖上层请求切分。开启时执行最早 batch 仲裁与低位 channel/bank 交织，关闭时使用 FIFO、可审计的二路 bank 交织和 row-first 映射。
- AE-only：`--scope aggregation --layer 0` 固定同一图、同一层和同一 AE 工作量，只切换稀疏优化，不混入 Weight 预取、CE、Output、ping-pong 排程或中间流量。

## 配置档

- `configs/HYGCN_PAPER.ini`：论文结构参数，以及带单位的 HBM channel/bank/row 时序参数。
- `configs/HYGCN_LEGACY.ini`：原始实现对应的兼容配置说明。
- `configs/HYGCN_SMOKE.ini`：默认测试使用的缩小配置，不可作为论文性能结论。
- `configs/paper_metrics.json`：版本化论文参考值、来源、单位、聚合规则和 20% 容差。

## 验收方法

基准对每个机制执行成对实验，除目标开关外，模型、数据集、配置、种子和其他策略保持一致：

- 稀疏：第一层 AE-only 的 `sparsity=on` 对比 `off`。
- 流水：`latency-aware` 对比 `sequential`。
- 协调器：`coordination=on` 对比 `off`。

参考清单按论文证据类型分别处理：

- Fig. 15/16 从 arXiv HTML 所引用 SVG 的柱高坐标数字化，清单保存源 URL、SHA256、坐标和逐数据集参考值。
- Fig. 15(a) 验收 AE-only 周期加速，Fig. 15(b) 验收 AE 的 Edge+Input 总 DRAM 比率；Input-only 比率仅作诊断。
- Fig. 16(a)/(b) 分别验收完整层周期加速和完整层 DRAM 比率。
- Fig. 17 的协调器平均 `3.70x` 加速和 `4.00x` 带宽提升按三数据集算术平均验收。

标量参考的相对误差为：

```text
abs(measured - reference) / abs(reference)
```

任一强制指标缺失、非有限或相对误差超过 20% 时，验收命令返回非零。

## 2026-09-24 自检结果

| 指标 | 论文参考 | 模拟结果 | 验收 |
|---|---:|---:|---|
| Fig. 15 AE-only 加速 Cora | 1.082102x | 1.138446x | 5.21%，PASS |
| Fig. 15 AE-only 加速 Citeseer | 2.883223x | 3.334554x | 15.65%，PASS |
| Fig. 15 AE-only 加速 PubMed | 1.115278x | 1.156091x | 3.66%，PASS |
| Fig. 15 AE DRAM 比 Cora | 0.879502 | 0.869152 | 1.18%，PASS |
| Fig. 15 AE DRAM 比 Citeseer | 0.339962 | 0.292966 | 13.82%，PASS |
| Fig. 15 AE DRAM 比 PubMed | 0.896637 | 0.859107 | 4.19%，PASS |
| Fig. 16 流水加速 Cora/Citeseer/PubMed | 2.125024/1.842621/1.366562x | 2.062418/2.186294/1.561088x | 2.95%/18.65%/14.23%，PASS |
| Fig. 16 DRAM 比 Cora/Citeseer/PubMed | 0.501466/0.535832/0.731763 | 0.580879/0.621853/0.781589 | 15.84%/16.05%/6.81%，PASS |
| Fig. 17 访存协调平均加速 | 3.70x | 3.293600x | 10.98%，PASS |
| Fig. 17 平均带宽利用率提升 | 4.00x | 3.293600x | 17.66%，PASS |

本轮未修改 `configs/HYGCN_PAPER.ini`，其 SHA256 为
`6e3b29fc9f93c578c8c850fde98bc7a4d426b0666624ef52f2a6ed596cdf821d`；
报告中的 `parameter_recalibration` 为 `false`。结果来自整改后的开发复跑，正式可审计产物由最终提交目录记录。

## 声明边界

当前结果表示 GCN 三数据集上的核心优化已达到请求级相对性能复现门槛，不表示 Ramulator/RTL 周期精度或端到端绝对性能完全复现。
本仓库没有同口径 CPU/GPU 软件基线、DiffPool、论文全部数据集、RTL、综合、面积或完整芯片能耗模型，
因此不宣称复现论文的 1509x CPU 加速、6.5x GPU 加速或完整能效结论。
