# HyGCN 论文机制复刻说明

本文档描述 `run/npu` 分支对 *HyGCN: A GCN Accelerator with Hybrid Architecture*
核心机制的模块级复刻与验收口径。

## 已实现机制

- 论文架构档：32 个 SIMD16 聚合核心、8 个组合模块、每模块 4 条 1x128 阵列，以及论文容量的片上缓冲和 256 GB/s HBM。
- Interval/Shard：按 Edge、Input 和 Aggregation Buffer 的可驻留容量分区，记录目标顶点、实际 edge 编码字节、源区间和唯一邻居。物理 Aggregation Buffer 保持 16 MiB、分为两个 8 MiB ping-pong 半区；图分区使用独立、跨数据集固定的 5 MiB 调度上限，并在 workload manifest、运行报告和参数 diff 中显式记录，不再由隐藏 residency-bank 常数派生。
- Window Sliding & Shrinking：稀疏开启时先滑到首个有效行，再收缩到末个有效行之后，发出一个连续 `[window_start, window_end)` 请求；窗口内部空洞仍被读取。关闭时请求完整 interval，流量和地址使用同一语义。
- Vertex-Disperse：高维特征分散到多个 SIMD，低维特征并行处理多个顶点；SUM 与 MAX 分开计数。
- Combination Module：显式计算矩阵 tile、活动模块、batch wave、权重装载/级联、输入推进、流水填充、MAC 和输出写回周期；单 batch 内的顶点组可并行占用 8 个模块，跨 batch 的 producer/RAW 依赖仍由时间线约束。
- Independent/Cooperative：两种模式都只从 HBM 装载一次权重并由 Weight Buffer 复用；前者把 ready batch 分配给可用模块，后者共享输入并按输出列协作。
- Aggregation Buffer：按 ping-pong 半区形成合法分区，在 batch 时间线上执行 ready、consume、reclaim；容量不足会阻塞 AE，CE 完成后才释放空间。
- AE/CE 策略：sequential 等待 AE 阶段完成，再按每批真实 producer bytes 的 block 对齐值写回和读回；Read 与对应 Write 同地址并等待写完成。latency-aware 在合法 batch ready 后启动；energy-aware 累积到目标顶点数或容量边界。Output 仅在对应 CE group 完成后入队。
- 动态 Input 依赖：Input 请求由对应 Edge 请求完成和邻居索引 ready 延迟共同释放，不再静态预知未来请求；跨 batch 仲裁保留实际 producer-ready、enqueue、issue 和 completion trace。
- Memory Access Coordinator：priority 和 address mapping 是两个独立开关。`batch-class` 执行最早 batch、请求类别和同优先级 open-row 延续；`fifo` 是排序基线。`low-bits` 按论文 §4.5.2 把低位映射到 channel/bank；`row-first` 保留 review v3 的完整 row 优先对照布局。原先隐藏的二路 bank striping 已移入配置和报告：它以 DRAMSim3 HBM 每周期至多发出第二条异类命令的宽度作为请求级近似，但不宣称 DRAMSim3 会固定配对相邻 bank，也不把该基线当作论文公开参数。
- AE-only：`--scope aggregation --layer 0` 固定同一图、同一层和同一 AE 工作量，只切换稀疏优化，不混入 Weight 预取、CE、Output、ping-pong 排程或中间流量。

## 配置档

- `configs/HYGCN_PAPER.ini`：论文结构参数，以及带单位的 HBM channel/bank/row 时序参数。
- `configs/HYGCN_LEGACY.ini`：原始实现对应的兼容配置说明。
- `configs/HYGCN_SMOKE.ini`：默认测试使用的缩小配置，不可作为论文性能结论。
- `configs/paper_metrics.json`：版本化论文参考值、来源、单位、聚合规则和 20% 容差。
- `configs/paper_workloads.json`：Fig. 15-17 的 GCN layer-0 shape、scope、聚合规则和两套地址映射依据。
- `configs/paper_parameter_baseline.json`：review v3 行为参数基线；benchmark 自动输出 current/baseline diff 和 `parameter_recalibration`。

## 验收方法

基准对每个机制执行成对实验，除目标开关外，模型、数据集、配置、种子和其他策略保持一致：

- 稀疏：第一层 AE-only 的 `sparsity=on` 对比 `off`。
- 流水：Table 5 layer 0 上 `latency-aware` 对比严格阶段化 `sequential`。
- priority-only：row-first 固定，`batch-class` 对比 `fifo`。
- mapping-only：FIFO 固定，`low-bits` 对比 `row-first`。
- combined：`batch-class + low-bits` 对比 `fifo + row-first`。

参考清单按论文证据类型分别处理：

- Fig. 15/16 从 arXiv HTML 所引用 SVG 的柱高坐标数字化，清单保存源 URL、SHA256、坐标和逐数据集参考值。
- Fig. 15(a) 验收 AE-only 周期加速，Fig. 15(b) 验收 AE 的 Edge+Input 总 DRAM 比率；Input-only 比率仅作诊断。
- Fig. 16(a)/(b) 分别验收完整层周期加速和完整层 DRAM 比率。
- Fig. 17 的协调器平均 `3.70x` 加速和 `4.00x` 带宽提升按三数据集算术平均验收。
- 带宽利用率的分母是至少一个 HBM 请求已 first-issue 且尚未 completion 的时间区间并集；等待 AE/CE producer 而没有在途请求的空闲周期保留在总执行时间中，但不伪装成 HBM 服务低效。

标量参考的相对误差为：

```text
abs(measured - reference) / abs(reference)
```

任一强制指标缺失、非有限或相对误差超过 20% 时，验收命令返回非零。

## 参数与证据

benchmark 报告包含 workload manifest SHA256、逐数据集原始结果、sequential producer-byte
oracle、priority issue/completion 反例、mapping channel/bank 分布，以及相对 review v3 的参数差异。
`parameter_recalibration` 由差异列表是否为空自动计算；机制修正不会被伪装成“配置未变化”。
5 MiB 图分区上限相对 review v3 的隐式 4 MiB 值会被报告为重标定；正式证据同时保存 4/5/6 MiB
敏感性结果，避免只给出单点门禁数值。该上限是调度占用而非物理容量声明。

```bash
python3 tools/partition_sensitivity.py \
  --binary build/hygcntest \
  --output-dir res/partition-sensitivity
```

## 声明边界

当前验收仅表示 GCN 三数据集 Table 5 layer-0 上的请求级相对机制结果，不表示 Ramulator/RTL 周期精度或端到端绝对性能完全复现。
本仓库没有同口径 CPU/GPU 软件基线、DiffPool、论文全部数据集、RTL、综合、面积或完整芯片能耗模型，
因此不宣称复现论文的 1509x CPU 加速、6.5x GPU 加速或完整能效结论。
Aggregation Buffer 仍以总容量 release queue 近似 ping-pong，并未建模精确 half ownership；
参考值校验器也不会自动重新下载和数字化 SVG，这两项保持为明确边界。
