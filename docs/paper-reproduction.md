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
- Memory Access Coordinator：priority 和 address mapping 是两个独立开关。映射前的四个 buffer port 并发提供 64B block；`fifo` 每个 admission 周期选择一个端口并连续放行最多四个 block，`batch-class` 按 batch-by-batch 约束执行 `Edge > Input > Weight > Output` 连续组装。required 路径不设置 active-window cap，因为论文 Figure 9 只给出四类 request source，并未给出未完成地址窗口容量；有限的 1/2/4/8/queue-capacity window 仅作为诊断反事实。两条路径共享全局 admission 时钟和完全相同的 transaction/command queue 容量，每周期合计最多接收 `floor(256B/cycle / 64B)=4` 个 block。`unified_queue=False` 时，每 channel 分别维护 32-entry read queue 和 32-entry write buffer，再进入每 bank 8-entry command queue；容量直接对应 bundled DRAMSim3 的 `trans_queue_size=32` 和 `cmd_queue_size=8`。控制器读优先并在写 buffer 满、读队列空或满足 draining 阈值时切换写方向。bank 请求通过 PRE、ACT、READ/WRITE 分阶段状态机，同一 channel 的所有 bank 共享排他的 command lane；READ/WRITE completion 与下一 data issue 解耦。实际 data command 使用 `tRCDRD/tRCDWR` ACT 间隔、`tCCD_L` 同向间隔和读写切换约束；换行显式调度 PRE→ACT，并受 `tRTP/tWR/tRAS/tRP/tRC` recovery 约束。统一 simulation clock 保证 admission、controller dispatch 与 command issue trace 全局时间单调。每个请求保存 PRE/ACT 数量及首末周期。priority bank 仲裁只在同 batch/class 内延续 open row，不跨 RAW 或优先级组绕行。`low-bits` 按论文 §4.5.2 把低位映射到 channel/bank；`row-first` 按 DRAMSim3 的 `rorabgbachco` 保持一个完整 row span 位于同一 channel/bank，不再使用无来源的二路 bank striping。
- Queue evidence：每次 admission 保存全部 channel 的 read/write occupancy before/after，并在末尾保存零占用快照；独立 validator 从零推导 admission 与中间 dispatch 总量，重算逐 channel 峰值、容量违规、histogram、weighted totals、edge samples 和 checksum。
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
- required 带宽利用率使用从周期 0 到最后一个 HBM 请求 completion 的统一 memory-service 区间，包含等待 AE/CE producer 的空闲时间。逐 block in-flight 区间并集另存为 `active_bandwidth_utilization`，只作诊断，因为 producer 延迟反例可使端到端吞吐下降而 active 利用率保持不变或上升。
- 同一 low-bits mapping 上，optimized/mapping-only 的逐数据集周期和完整区间带宽必须至少改善 `0.5%`（`>=1.005x`）；row-hit 逐数据集和三数据集平均都必须至少改善 `3%`（`>=1.03x`）。论文没有给出 isolated priority 的 row-hit 数值，因此 schema v6 将 aggregate 门槛与逐数据集门槛统一，删除无外部依据且重复的 `1.05x` 更高门槛。这些仍是 12 项内部因果检查，不是论文外部指标。

标量参考的相对误差为：

```text
abs(measured - reference) / abs(reference)
```

任一强制指标缺失、非有限或相对误差超过 20% 时，验收命令返回非零。

## 参数与证据

benchmark 报告包含 workload manifest SHA256、逐数据集原始结果、sequential producer-byte
oracle、priority issue/completion 反例、mapping channel/bank 分布，以及相对 review v3 的参数差异。
`parameter_recalibration` 由差异列表是否为空自动计算；机制修正不会被伪装成“配置未变化”。
5 MiB 图分区上限相对 review v3 的隐式 4 MiB 值会被报告为重标定。Cora、Citeseer 和 PubMed
在 review v4 前均已暴露，因此当前 14/14 论文数值只声明为三数据集 calibrated fit，不声明独立 hold-out。
partition sensitivity 继续报告 4/5/6 MiB，供读者判断拟合脆弱性；该上限是调度占用而非物理容量声明。
论文给出 256 GB/s HBM1；bundled `HBM1_4Gb_x128.ini` 是 8-channel、128 GB/s 单栈，因此 paper profile 显式复制为两栈共 16 个物理 channel。每个物理 channel 仍使用同一份 DRAMSim3 时序和独立 32-entry read/write queue，不通过缩短 tCCD 伪造带宽。HBM read `14/28/42` 周期由 bundled DRAMSim3 HBM 的 `CL/tRCDRD/tRP` 推导，write `4/18/32` 周期由 `CWL/tRCDWR/tRP` 推导，read-to-write/write-to-read 分别为 `18/16` 周期；row-command issue interval 为 `tCK=2` 个模型周期。未由论文公开的 neighbor-index 延迟另做固定邻域敏感性。row-first=1 是
`rorabgbachco` 的 required 基线，interleave=2 只作为诊断反事实，不参与目标拟合。

每层 admission 与 command 证据都保存完整、可逆的 delta-varint/base64 分块 trace。admission v3 除逐 channel 方向 occupancy 外，还保存每个入队 block 的 `sequence/block_offset`，因此可独立恢复精确 admission cycle。独立的 `hygcn_trace_validator` C++ 可执行程序不链接模拟器状态，会解码全部 admission 事件，重算读写 histogram、weighted totals、actual maxima 和 FNV-1a checksum；同时解码全部 PRE/ACT/READ/WRITE 事件，重算命令类型、FNV-1a checksum、每 channel command-lane、row state、recovery timing，以及每个 request 的 first issue、completion、PRE/ACT 数量和首尾周期。每个 command 的 `sequence/block_offset` 会同时解析回 `memory_requests` 和逐 block admission 时间线，独立核对 request 存在性、block 范围、读写方向、地址映射、producer-ready/enqueue/admission 因果、data-command 唯一性和 admission 方向总量。producer dependency oracle 读取 request class、base ready/enqueue、producer sequence 与 delay，从 producer 的重建 completion 推导 effective ready/enqueue，并拒绝 unknown producer、自依赖、依赖环以及 future/late producer。Python benchmark 只消费这些独立重放结果，避免把数千万事件降级成抽样。两类首尾样本都只用于人工浏览，不作为完整 trace 的替代；删除事件、修改中间周期、清空样本、伪造 checksum、伪造 request identity/direction/mapping、制造 duplicate/missing block、交换同 row 同方向但跨 producer-ready 边界的两个 block identity，或篡改 producer 为 unknown/self/future request，compiled-validator mutation test 都必须失败。

验收报告始终分开列出 `14` 项论文数值和 `12` 项内部因果检查，不以 `26/26` 表述扩大外部证据。

```bash
python3 tools/partition_sensitivity.py \
  --binary build/hygcntest \
  --output-dir res/partition-sensitivity

python3 tools/model_sensitivity.py \
  --binary build/hygcntest \
  --output-dir res/model-sensitivity
```

## 声明边界

当前验收仅表示 GCN 三数据集 Table 5 layer-0 上的请求级相对机制结果，不表示 Ramulator/RTL 周期精度或端到端绝对性能完全复现。cap-free required 结果若未进入论文值 ±20%，报告必须保留 FAIL，不能通过 baseline-only throttle 恢复门禁。
本仓库没有同口径 CPU/GPU 软件基线、DiffPool、论文全部数据集、RTL、综合、面积或完整芯片能耗模型，
因此不宣称复现论文的 1509x CPU 加速、6.5x GPU 加速或完整能效结论。
Aggregation Buffer 仍以总容量 release queue 近似 ping-pong，并未建模精确 half ownership；
参考值校验器也不会自动重新下载和数字化 SVG，这两项保持为明确边界。
