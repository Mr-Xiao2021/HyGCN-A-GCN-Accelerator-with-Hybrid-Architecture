# HyGCN 论文复现合理性独立审查报告

## 1. 审查结论

**审查对象**：`run/npu` 分支，提交
`03ad0ac6bffc15cb3ac14c6cfcb023e4e7e57e9d`（`enhance reproduce hygcn`）。

**基准论文**：Mingyu Yan et al., *HyGCN: A GCN Accelerator with Hybrid
Architecture*, HPCA 2020。本文采用作者公开的
[原论文 PDF](https://mingyuyan-ict.github.io/MingyuYan-ICT/files/HyGCN-HPCA2020.pdf)
和 [arXiv 版本](https://arxiv.org/abs/2001.02514) 作为判定依据。仓库内的
`ResearchReport.pdf` 是 2023 年中文复现报告，不是 HPCA 原论文。

**最终判定：不接受当前提交作为“论文性能指标上下 20% 的复现”。**

当前提交适合被描述为：

> 一个受 HyGCN 启发、具有论文参数外形、可确定性运行的模块级解析模型和回归框架。

它不适合被描述为：

> 一个已经复现论文关键性能效应、且误差在上下 20% 内的周期级或微架构级模拟器。

最关键的原因不是覆盖范围有限，而是验收依据和性能因果链不成立：参考值被改成了远低于论文
原文的数值，访存协调收益来自手工效率常数，流水收益来自固定重叠率和 spill 系数，而不是请求、
Bank、行缓冲和 AE/CE 批次执行共同产生的结果。因此，当前“六项全部 PASS”是模型对自定义门槛
的自洽，不是对论文结果的独立复现。

| 维度 | 判定 | 说明 |
|---|---|---|
| 论文结构参数 | 较好 | 32 SIMD16、8 个组合模块、4x128 阵列和主要 Buffer 容量已配置化 |
| 架构机制覆盖 | 部分 | 有对应数据结构和公式，但多处没有进入时序或性能因果链 |
| 性能模型可信度 | 不足 | paper 路径是解析公式，不使用 DRAMSim3 的请求级结果 |
| ±20% 验收有效性 | 无效 | 多个“论文平均值”不是论文原值，部分只是范围端点或人为值 |
| 数据集/模型覆盖 | 不足 | 仅强验收 GCN + Cora/Citeseer/PubMed；论文为 4 模型、6 数据集 |
| 工程可复现性 | 部分 | 带 workaround 的测试通过，但干净 Release 构建失败 |

---

## 2. Findings

以下 findings 按严重程度排序。`P0` 表示结论失效，`P1` 表示核心模型偏差，`P2` 表示重要但不单独
推翻结论的问题。

### F-01 [P0] 论文参考清单使用了错误或无出处的数值，导致 ±20% 门禁失真

位置：

- `configs/paper_metrics.json:7-41`
- `docs/paper-reproduction.md:33-40`
- `openspec/changes/reproduce-hygcn-paper-performance/design.md:62-72`
- `openspec/changes/reproduce-hygcn-paper-performance/specs/paper-performance-validation/spec.md:27-35`

原论文 §5.3 和 Fig. 15-17 给出的结论，与清单中的“平均值”不一致：

| 指标 | 原论文 | 清单值 | 本次复跑 | 审查判断 |
|---|---:|---:|---:|---|
| 稀疏消除加速 | **1.1-3x，逐数据集范围** | 1.10x“平均” | 1.2596x 平均 | 把范围下界误标为平均值 |
| 稀疏输入 DRAM 比率 | Fig. 15(b)，正文未给 0.60 平均值 | 0.60 | 0.6319 | 0.60 缺少可核验出处 |
| 流水加速 | 执行时间下降 **27%-53%**，即约 **1.37-2.13x** | 1.10x“平均” | 1.1931x | 参考值不在论文报告范围内 |
| 流水 DRAM 比率 | **0.50-0.73** | 0.50“平均” | 0.5607 | 把最佳端点误标为平均值 |
| 协调器加速 | 平均节省 **73%** 时间，即约 **3.70x** | 1.10x | 1.0217x | 严重错误 |
| 协调器带宽提升 | 平均 **4x** | 1.10x | 1.0217x | 严重错误 |

按论文正文的协调器结果重新计算，当前 1.0217x 相对 3.70x 加速的误差约为 **72.4%**，相对 4x
带宽提升的误差约为 **74.5%**，显然不在 ±20% 内。

原论文并未把 Fig. 15 和 Fig. 16 中每个柱状值汇总为清单所写的 1.10 平均值。若要做自动验收，
应对图表进行可追踪的数字化提取，保存逐数据集参考值和提取误差，而不是选取范围端点后声明为平均。

**影响**：当前 validation report 的 PASS 结论无效，不能支撑论文复现声明。

### F-02 [P0] 访存协调器的性能收益由两个配置常数直接决定，不是请求重排产生的

位置：

- `configs/HYGCN_PAPER.ini:18-24`
- `hygcn/paper_sim.cpp:44-52`
- `hygcn/paper_sim.cpp:860-891`
- `hygcn/paper_sim.cpp:977-1013`
- `hygcn/paper_sim.cpp:461-466`

paper 路径用 `coordinated_efficiency=0.90` 和 `uncoordinated_efficiency=0.82` 选择一个标量效率，随后
用 `bytes / (peak_bandwidth * efficiency)` 计算内存周期。AE、CE 和总周期在请求排序之前已经确定。
`MemoryCoordinatorModel::Order()` 只改变后续统计请求的遍历顺序，计算出的 queue wait 不会反馈到
`metrics.cycles`。

本次做了一个不修改源码的敏感性实验：仅把两个效率都设为 0.90，其他输入保持不变。

| Cora 实验 | coordination=off | coordination=on |
|---|---:|---:|
| 总周期 | 269,448 | 269,448 |
| 带宽利用率 | 0.557054 | 0.557054 |
| Channel/Bank 分布 | 相同 | 相同 |
| 累计 queue wait | 不同 | 不同 |

结果证明请求排序虽然改变了一个未进入关键路径的统计量，却不能改变完成时间。当前默认配置下的
1.0217x 收益全部来自 0.82/0.90 的先验效率差。

此外，`bandwidth_utilization = bytes / (cycles * peak)`；协调开关前后字节数相同，所以
`coordination_bandwidth_gain` 必然与 `coordination_speedup` 完全相等。本次三个数据集的聚合结果
均为 1.0216824219，两个所谓独立指标实际上是同一个公式的重述。

模型没有实现论文协调优化所依赖的：

- DRAM row-buffer hit/miss；
- 请求地址连续性对 ACT/PRE/RD/WR 的影响；
- Channel/Bank 可并行发射；
- DRAM 队列容量和 backpressure；
- batch 间低优先级请求先于下一 batch 高优先级请求所产生的真实完成时间。

**影响**：协调器和带宽两项验收没有微架构证据，是参数回放而非复现。

### F-03 [P1] AE/CE 流水不是批次级时序模型，Aggregation Buffer 状态机不影响执行

位置：

- `configs/HYGCN_PAPER.ini:26-35`
- `hygcn/paper_sim.cpp:792-826`
- `hygcn/paper_sim.cpp:855-908`

每个 batch 在同一个无时间推进的循环中依次执行 `Allocate -> MarkReady -> StartConsume -> Reclaim`。
因此 Buffer 峰值最多是一个 batch，无法出现真实的 AE/CE 并发、ping-pong 半区竞争、CE 滞后或
容量 backpressure。`aggregation_buffer_capacity_stalls` 对任何“合法 batch”实际上不可达。

最终时序由三个手工参数决定：

- `latency_overlap_fraction = 0.05`；
- `energy_overlap_fraction = 0.14`；
- `sequential_spill_factor = 2.2`。

流水模式的 CE 起点只是
`AE_finish - min(AE_cycles, CE_cycles) * overlap_fraction`。它不由第一个可消费顶点/小组的 ready
时间决定。sequential 模式额外写入 `2.2 * vertices * input_stride` 的中间流量；该系数直接影响
流水消融结果，但没有论文或硬件推导依据。

从结果上看，三个数据集的流水加速几乎固定在 1.19x；原论文报告的是随数据集变化的 27%-53%
执行时间下降。这种低方差也是标量校准模型而非工作负载驱动时序模型的表现。

**影响**：流水结果可用于公式回归，不足以证明论文的 inter-engine pipeline 被复现。

### F-04 [P1] Window Sliding & Shrinking 被替换成理想化唯一邻居计数，地址行为与流量行为不一致

位置：

- `hygcn/paper_sim.cpp:483-600`
- `hygcn/paper_sim.cpp:765-777`
- `hygcn/paper_sim.cpp:930-949`

稀疏开启时，代码从目标 chunk 汇总全体唯一源顶点，再按窗口容量分组。`input_bytes` 直接设为
`unique_neighbors.size() * feature_stride`。`shrunk_interval_end` 仅作为元数据记录，不参与传输范围、
请求拆分或周期计算。

随后，每个分组却只生成一个从 `interval_start * input_stride` 开始的连续请求，请求长度是“唯一邻居
数量”。若邻居编号是 `{10, 20, 30}`，统计上按三个向量计费，地址上却等价于请求 `{10, 11, 12}`。
因此：

- 稀疏流量使用理想 gather 计数；
- Channel/Bank 分布使用另一组连续地址；
- 没有模拟 Sparsity Eliminator 获取索引后发出的真实离散预取；
- Window 的滑动/收缩边界本身不产生任何性能效果。

目标分区宽度还使用完整 16 MB Aggregation Buffer，并由固定的
`partition_vertices = 1536` 再截断。原论文的 Aggregation Buffer 是 ping-pong 使用，合法宽度应与
当前 feature stride 和半区容量共同变化，不应由跨数据集固定顶点数主导。

本次结果与 Fig. 15 的拓扑响应也不一致：当前 Cora/Citeseer/PubMed 的稀疏加速分别为
1.015x、1.070x、1.694x；论文图中大致是 Cora 约 1.1x、Citeseer 约 3x、PubMed 约 1.1x。
尤其 Citeseer 的主要收益没有出现，PubMed 却被模型赋予最大收益。

**影响**：聚合侧最核心的动态稀疏优化尚未形成可审计的请求级复现。

### F-05 [P1] paper 路径不是 cycle-accurate/execution-driven 内存模拟器

位置：

- `hygcn/paper_sim.cpp:44-52`
- `hygcn/paper_sim.cpp:735-1015`
- `CMakeLists.txt:17-27`

原论文使用 cycle-accurate、execution-driven 模拟器并集成 Ramulator。仓库链接了 DRAMSim3，legacy
路径也会实例化它，但 `PaperSimulator` 中没有任何 DRAMSim3 MemorySystem、事务提交、时钟推进或
callback 调用。它是一次性解析计算：

- AE 周期 = 聚合操作数 / SIMD lane 数 / `simd_efficiency`；
- CE 周期 = MAC 数 / PE 数 / `array_efficiency`，再加若干闭式公式；
- HBM 周期 = 字节数 / 峰值带宽 / 手工效率；
- eDRAM 成本 = shard 数 * 固定 2 周期。

这类模型适合设计空间的早期估算，但不能声称已经复现论文的行缓冲、通道/Bank 并行、请求冲突、
双缓冲隐藏或逐周期 engine interaction。

**影响**：输出字段虽然命名为 cycles、queue wait、blocked cycles 和 bandwidth utilization，其精度等级
仍是解析估算，不应与论文的详细微架构模拟结果直接做 ±20% 对照。

### F-06 [P1] 干净 Release 构建失败，既有“独立目录构建通过”无法复现

位置：

- `hygcn/hardware/simd.h:8-9`
- `hygcn/hardware/simd.h:80-84`
- `hygcn/hardware/simd.cpp:5`

复跑命令：

```bash
cmake -S . -B build-review -DCMAKE_BUILD_TYPE=Release
cmake --build build-review -j2
```

GCC 13.3 在编译 `hygcn/hardware/simd.cpp` 时失败：`simd.h` 直接使用 `uint64_t`，但没有包含
`<cstdint>`。这是确定性的 include self-sufficiency 问题。

为了继续审查，本次仅在 CMake cache 中加入 `-include cstdint` 后完成后续测试；源码没有修改。

**影响**：按 README 提供的标准构建命令，指定提交不能从干净 checkout 构建。

### F-07 [P1] 计算引擎是理想吞吐公式，关键调度行为没有进入周期

位置：

- `hygcn/paper_sim.cpp:616-680`
- `hygcn/paper_sim.cpp:753-790`
- `hygcn/paper_sim.cpp:824-853`

AE 的实际图结构只影响总边数和流量。执行周期使用总操作数除以总 lane 数和固定 0.78 效率，没有
逐顶点 degree、SIMD task、eSched 分配、同步或负载不均衡。所谓 Vertex-Disperse 的
`simd_cores_per_vertex` 和 `simd_parallel_vertices` 只是输出统计，不参与周期。

CE 同样把全部 MAC 除以 4096 个 PE 和固定效率。tile、active module、batch wave 被记录，但没有
模块级资源时间线。Independent 模式还把同一权重矩阵的 HBM 读取乘以活动模块数；原论文明确说明
两种模式都在 Weight Buffer 中跨顶点复用权重，差异主要是模块间权重传播和片上访问能耗，因此这里
把模块副本直接记为 HBM 流量缺少依据。

**影响**：模型能保持操作量守恒，但不能验证论文所强调的 eSched、顶点内并行、模块协作和权重复用
的真实性能效果。

### F-08 [P1] 强制验收范围只覆盖论文矩阵的一小部分，且其余 CLI 模型语义不完整

位置：

- `tools/paper_benchmark.py:10-35`
- `configs/paper_metrics.json:5`
- `hygcn/paper_sim.cpp:474-481`
- `hygcn/paper_sim.cpp:1033-1043`
- `docs/paper-reproduction.md:53-57`

论文使用 GCN、GraphSAGE、GINConv、DiffPool 和六个数据集。当前强制矩阵只有 GCN + Cora、
Citeseer、PubMed：

- 缺 IMDB-BIN、COLLAB、Reddit；
- 缺 DiffPool；
- GIN 与 GCN 共用同样的两层 shape，没有 `(1+epsilon)` 和 `|a|-128-128` MLP；
- GraphSAGE 只切换成 MAX 操作和读取离线 sample 文件；
- 没有 Sampler、Activate Unit、数值 feature/weight、归一化权重或推理精度验证；
- 不产生可与框架输出比较的 GCN 数值结果。

文档已诚实声明部分边界，这是优点；但这些边界也意味着“论文复现”只能限定为少数机制的概念性
估算，不能外推到论文总体性能和能耗结论。

### F-09 [P2] 测试主要验证内部公式自洽，不能发现错误论文引用或缺失的性能因果链

位置：

- `tools/validate_reference_metrics.py:35-53`
- `tools/test_metric_validator.py:41-58`
- `hygcn_test/unit.cpp`
- `CMakeLists.txt:31-80`

`paper_reference_manifest` 只检查参考值为正数、字段存在、容差等于 0.20；它不核对论文原文、图号
或逐数据集值。因此，把协调器参考值从 4x 写成 1.1x 仍会 PASS。

单元测试覆盖了地址边界、守恒和公式输出，工程价值较高，但它们没有验证：

- 请求排序能改变真实完成周期；
- Bank/row-buffer 行为符合 DRAM 后端；
- AE/CE batch 的 ready/consume 时间线；
- Fig. 15-17 的逐数据集趋势；
- 配置校准参数的来源和敏感性。

默认 `ctest` 中的 `legacy_snapshot_fixture` 使用 `--fixture-only`，只检查快照文件格式，不复跑 legacy
模拟。完整 legacy 和 paper benchmark 是额外 custom target，不属于 8 个默认测试。

### F-10 [P2] HBM 配置、解析模型和论文带宽不是同一条可追踪链路

位置：

- `configs/HBM1_4Gb_x128.ini:1-9`
- `configs/HBM1_4Gb_x128.ini:51-59`
- `configs/HYGCN_PAPER.ini:18-24`

paper 路径直接使用 256 bytes/cycle 标量，不读取 `HBM1_4Gb_x128.ini`。legacy 路径读取该 DRAMSim3
配置，但配置是 8 channel x 128-bit、`tCK=2ns`、DDR burst，理论峰值约为 128 GB/s，而不是论文
Table 6 的 256 GB/s。两条路径因此都没有形成“论文 HBM1 参数 -> DRAM 时序配置 -> 请求执行 ->
周期结果”的一致证据链。

---

## 3. 论文机制映射

| 论文机制/结果 | 当前实现 | 完整度 | 审查意见 |
|---|---|---|---|
| 32 个 SIMD16 | `HYGCN_PAPER.ini` 声明 32x16 | 参数对齐 | 周期仍是总吞吐公式 |
| 8 个模块，每模块 4x128 array | 已配置并输出 schedule 元数据 | 部分 | 无模块占用时间线和冲突 |
| 128KB/2MB/2MB/4MB/16MB Buffer | 容量对齐 | 部分 | ping-pong 半区和 backpressure 未进入时序 |
| Interval/Shard | 有目标 chunk、源 interval、unique source | 部分 | 固定 1536 cap 改变论文容量驱动分区 |
| Window Sliding & Shrinking | 有近似分组和边界字段 | 较弱 | 性能使用理想 unique count，边界本身不生效 |
| Vertex-Disperse | 输出 cores-per-vertex | 较弱 | eSched 和逐顶点调度未进入周期 |
| SUM/MAX | 操作计数分开 | 基本 | 不计算数值结果 |
| Independent/Cooperative | 两种闭式 schedule | 部分 | 权重流量和能耗语义不完整 |
| Latency-/Energy-aware pipeline | 有模式和重叠参数 | 较弱 | 固定 overlap，不是 batch 事件流水 |
| Ping-pong Aggregation Buffer | 有状态类 | 较弱 | 状态在零时间内完成，不形成 ping-pong 并发 |
| Memory Access Coordinator | 能排序请求 | 名义实现 | 排序不改变周期；收益来自效率常数 |
| 地址低位映射到 Channel/Bank | modulo 统计 | 较弱 | 不驱动 DRAM 时序，无 row 映射/命中 |
| Ramulator 级详细内存模拟 | paper 路径没有 | 缺失 | DRAMSim3 仅与 legacy 路径相关 |
| Sampler / Activate Unit | 无性能模型 | 缺失 | GraphSAGE 依赖离线样本 |
| 4 模型 x 6 数据集 | 1 模型 x 3 数据集强验收 | 不完整 | 不能代表论文总体矩阵 |
| 12nm RTL、CACTI、功耗/面积 | 无 | 缺失 | 无法复现 6.7W、7.8mm2 和能耗结论 |
| CPU/GPU 基线 | 无 | 缺失 | 无法复现 1509x/6.5x 端到端加速 |

---

## 4. 独立复跑结果

### 4.1 仓库与输入确认

- HEAD：`03ad0ac6bffc15cb3ac14c6cfcb023e4e7e57e9d`；
- 分支：`run/npu`，跟踪 `origin/run/npu`；
- clone 后源代码工作区干净；
- Cora：2,708 顶点、10,556 边、1,433 特征；
- Citeseer：3,327 顶点、9,104 边、3,703 特征；
- PubMed：19,717 顶点、88,648 边、500 特征；
- 三个 edge 文件的行数与 metadata 一致，无越界顶点和重复边。

### 4.2 构建与测试

| 检查 | 结果 | 说明 |
|---|---|---|
| 干净 Release 构建 | **FAIL** | `simd.h` 缺 `<cstdint>` |
| 强制 `-include cstdint` 后构建 | PASS | 仅用于继续审查，不代表原提交可构建 |
| `ctest --output-on-failure` | 8/8 PASS | 总耗时约 0.44s |
| Legacy Cora snapshot | PASS | 显式复跑 |
| Legacy Citeseer snapshot | PASS | 显式复跑 |
| Paper benchmark | 现有 6/6 PASS | 但参考值错误，不能作为论文验收 |
| OpenSpec strict CLI | 未运行 | 当前环境未安装 `openspec` 命令；已静态审查规格产物 |

### 4.3 本次 paper benchmark 原始汇总

| 指标 | 复跑值 |
|---|---:|
| sparsity_speedup | 1.259615 |
| sparsity_input_dram_ratio | 0.631931 |
| pipeline_speedup | 1.193107 |
| pipeline_dram_ratio | 0.560681 |
| coordination_speedup | 1.021682 |
| coordination_bandwidth_gain | 1.021682 |

逐数据集结果：

| 数据集 | 稀疏加速 | 稀疏输入比 | 流水加速 | 流水 DRAM 比 | 协调加速 | 带宽提升 |
|---|---:|---:|---:|---:|---:|---:|
| Cora | 1.014934 | 0.904727 | 1.196632 | 0.506985 | 1.017443 | 1.017443 |
| Citeseer | 1.069949 | 0.701914 | 1.196000 | 0.526367 | 1.018725 | 1.018725 |
| PubMed | 1.693961 | 0.289153 | 1.186689 | 0.648691 | 1.028879 | 1.028879 |

这些数值与提交文档一致，说明结果可以重复；问题在于它们对应的是当前闭式模型和错误门槛，而不是
论文的真实验收目标。

---

## 5. 做得合理的部分

尽管最终复现结论不成立，提交仍有以下可保留的工程价值：

1. 论文 Table 6 的主要计算资源和 Buffer 容量被集中配置，结构比原 legacy 配置清楚。
2. 单次运行 CLI、JSON/CSV manifest、输入/config/binary digest 和成对消融脚本改善了可追踪性。
3. Cora、Citeseer、PubMed 的数据规模与论文 Table 4 一致，数据文件基本完整。
4. 输出地址 stride、边 chunk 实际字节、事务边界和统计守恒增加了单元测试。
5. legacy Cora/Citeseer 快照能够复跑，旧路径的行为被保存。
6. 文档明确否认已经复现 CPU/GPU 绝对加速、DiffPool、面积和完整能效，声明边界比原仓库清晰。
7. benchmark 的 paired ablation 会检查 manifest 中只有目标开关发生变化，实验编排本身是合理的。

这些成果适合作为下一轮实现的测试和实验基础，不应因为当前论文门禁无效而全部丢弃。

---

## 6. 建议的重新验收标准

若目标仍是“无需 100% 对齐，但应反映原论文上下 20% 的性能指标”，建议至少满足以下门槛后再宣布
复现完成：

1. **重建参考数据**：从 Fig. 15-17 数字化提取 Cora/Citeseer/PubMed 的逐数据集柱值，记录图号、
   单位、提取工具、像素误差和原始截图；正文明确值直接按正文使用。
2. **取消错误聚合门槛**：逐数据集指标先独立过 ±20%，再报告几何平均或论文同口径平均；不允许用
   错误的总平均掩盖 Cora/PubMed/Citeseer 方向相反的偏差。
3. **让协调排序真正进入时序**：paper 路径必须把请求送入 DRAMSim3/Ramulator，或实现经验证的
   row/channel/bank 时序模型。协调开关不得切换预设效率常数。
4. **实现 batch 事件流水**：AE 完成一个合法小组后产生 ready 时间，CE 按模块模式消费，Buffer 在
   消费结束后 reclaim；总周期取真实事件关键路径。
5. **重做分区和请求地址**：分区宽度由 ping-pong 半区、feature stride、Input/Edge Buffer 共同决定；
   unique 邻居必须生成真实离散请求或明确建模 coalescing，流量和地址不能采用两套语义。
6. **校准参数可识别**：每个 efficiency/overlap/spill 参数都要有来源、单位、可接受范围和敏感性报告；
   禁止使用直接控制目标指标的自由参数进行同数据集拟合。
7. **补齐最小覆盖**：至少增加论文中一个高边数数据集和一个非 GCN 模型作为 hold-out；否则只能声明
   “GCN 三数据集机制验证”。
8. **建立干净 CI**：标准 README 命令必须在干净 checkout 上通过；full paper benchmark 应成为可见的
   CI job，而不只是手动 custom target。
9. **区分精度等级**：解析估算、请求级模拟、cycle-accurate 模拟、RTL 综合结果使用不同标签，报告中
   不混用“cycle”字段制造同等精度的印象。

建议的阶段性接受口径是：

> 当 F-01、F-02、F-03、F-04 和 F-06 关闭，并且 Fig. 15-17 的逐数据集结果独立落入 ±20% 时，
> 可接受为“HyGCN GCN 核心优化的请求级性能复现”；在补齐模型、数据集、能耗和 RTL 之前，仍不能
> 接受为完整论文复现。

---

## 7. 审查范围与未实施事项

本次仅做 review 和取证，没有修改实现、配置、测试或论文门槛。为继续执行测试而使用的
`-include cstdint` 仅通过本地 review build 的 CMake cache 注入，不是源码修复；临时构建目录已在
审查结束后清理。

没有尝试复现论文的 CPU/GPU 软件环境、12nm 综合、CACTI、PrimeTime PX 或推理准确率；当前仓库也
不包含完成这些验证所需的 RTL、完整模型实现和全部数据集。
