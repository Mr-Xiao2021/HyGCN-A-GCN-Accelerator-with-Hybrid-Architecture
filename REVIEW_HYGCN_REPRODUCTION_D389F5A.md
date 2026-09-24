# HyGCN 论文复现独立复审报告（d389f5a）

## 1. 复审结论

**复审对象**：`run/npu` 分支，提交
`d389f5a481abd8158f28c37a745a3a34ac7a06d4`（`refine to review`）。

**对比基线**：上一轮审查提交 `03ad0ac6bffc15cb3ac14c6cfcb023e4e7e57e9d`，以及其审查报告
`REVIEW_HYGCN_REPRODUCTION_03ad0ac.md`。

**基准论文**：Mingyu Yan et al., *HyGCN: A GCN Accelerator with Hybrid
Architecture*, HPCA 2020。判定依据为作者公开的
[原论文 PDF](https://mingyuyan-ict.github.io/MingyuYan-ICT/files/HyGCN-HPCA2020.pdf)
和 [arXiv 版本](https://arxiv.org/abs/2001.02514)。仓库内 `ResearchReport.pdf` 是 2023 年中文
复现报告，不是原论文。

**最终判定：有实质进步，但仍未达到我对“论文关键性能指标上下 20% 的可信请求级复现”的预期。**

当前提交已经从“由目标收益常数直接驱动的解析模型”推进为“带请求地址、Channel/Bank/Row 状态和
AE/CE batch 时间线的请求级原型”。干净构建、默认测试、legacy 回归和当前门禁均可重复通过；上一轮
最明显的错误论文参考值、固定 overlap、固定 spill 系数、协调/非协调效率常数和构建失败已经修正。

但当前 `PASS` 仍不能证明论文结果被复现，原因是三条核心因果链仍不成立：

1. 稀疏路径实现的是“精确唯一邻居 gather”，不是论文 Algorithm 4 的连续 Window Sliding & Shrinking；
2. HBM 模型对同一地址流仅因请求切分不同就产生 4.73 倍差异，而稀疏开关恰好同时改变请求切分；
3. 输出和 sequential 中间读写在计算结果产生前已经进入内存模型，同一中间流量又以解析 spill 周期
   加到关键路径，流水基线存在非因果调度和重复计时。

因此，当前结果适合描述为：

> HyGCN GCN 三数据集核心机制的、可重复运行的请求级研究原型；当前输出与论文正文范围相容。

不适合描述为：

> 已经以逐数据集上下 20% 误差复现 HyGCN Fig. 15-17，或复现了论文的 cycle-accurate 性能。

| 维度 | 本次判定 | 相比 03ad0ac |
|---|---|---|
| 干净构建与回归 | 通过 | 已修复 |
| 论文参考值 | 正文标量已纠正 | 明显改善 |
| 请求地址与交通守恒 | 基本建立 | 明显改善 |
| 访存协调器 | 已进入完成时间，但存在请求粒度伪加速 | 部分达到 |
| AE/CE 流水 | 有 batch 时间线，但内存请求不满足因果 | 部分达到 |
| Window Sliding & Shrinking | 地址真实，但算法语义过度理想化 | 仍未达到 |
| ±20% 门禁 | 仅验证宽泛范围相容，不是逐数据集误差 | 仍无效 |
| 论文总体复现 | 不完整 | 边界声明更准确 |

---

## 2. 主要 Findings

以下 findings 按严重度排序。`P0` 表示会使当前论文门禁的结论失效，`P1` 表示重要模型偏差，`P2`
表示工程或覆盖风险。

### F-01 [P0] 稀疏路径实现了论文没有提出的精确 gather，并改变了请求粒度

位置：

- `hygcn/paper_sim.cpp:618-695`
- `hygcn/paper_sim.cpp:980-1003`
- `docs/paper-reproduction.md:9-10`
- 原论文 Algorithm 4 与 Fig. 5(c)(d)，PDF 第 21 页

论文 Algorithm 4 的语义是：窗口先滑到首个非空行，再从底部收缩到最后一个非空行，最终返回连续的
`X[winstart:winend]`。窗口内部的空洞仍属于该连续区间；论文并未实现按唯一邻居索引进行任意 gather。

当前实现先构造 `unique_sources`，然后把 `input_bytes` 记为
`unique_neighbors.size() * feature_stride`。生成请求时，它进一步把不连续邻居拆成多个连续 run，仅读取
实际出现的顶点。例如邻居 `{0, 2, 4, 6}` 会产生 4 个精确请求，而论文窗口会读取从首个到末个非空行
之间的连续范围。

这不是对上一轮地址问题的等价修复，而是比论文更强的稀疏硬件能力。它同时造成两个偏差：

- 少算 Window 内部空洞的 DRAM 流量；
- 把一个连续 Window 请求拆成多个请求，触发 F-02 的额外并行发射收益。

单元测试 `hygcn_test/unit.cpp:110-129` 还把“4 个离散邻居必须产生 4 个请求”固化为 golden 行为，因此
测试保护的是当前理想 gather 语义，而不是论文的 Sliding & Shrinking 语义。

**影响**：Fig. 15 的稀疏 traffic 和 speedup 不能与论文同口径比较。

### F-02 [P0] HBM 完成时间依赖请求封装方式，同一地址流可产生 4.73 倍差异

位置：

- `hygcn/paper_sim.cpp:438-515`
- `hygcn/paper_sim.cpp:449-498`
- `hygcn/paper_sim.cpp:980-1003`

`MemoryCoordinatorModel::Simulate()` 为每个 `MemoryRequest` 新建一个
`issue_cursor = request.enqueue_cycle`。单个大请求内的所有 block 被该 cursor 串行约束；请求结束后，
下一个请求的 cursor 又从其 enqueue 周期重新开始。Channel 和 Bank 状态虽然保留，但不存在全局控制器
发射宽度或统一事务队列。

本次用相同配置、地址、字节数、enqueue 周期和访问顺序做了独立反例，仅改变请求封装：

| 128 个连续 64B block | coordinated 周期 | row hit/miss |
|---|---:|---:|
| 单个 8192B 请求 | 284 | 0/128 |
| 128 个 64B 请求 | 60 | 0/128 |

两者物理访问集合和 Row 行为完全相同，后者却快 **4.7333x**。关闭协调映射时，两者均为 1820 周期，
说明差异正来自 coordinated 映射下“每个 request 隐式获得独立 issue cursor”。

稀疏消融正好把 `off` 的大 interval 请求改成 `on` 的多个 run 请求，因此当前
`sparsity_speedup` 同时测量了：

- 减少的 DRAM 字节；
- 更细请求粒度带来的额外隐式发射端口；
- F-01 中超出论文能力的精确 gather。

现有协调器测试 `hygcn_test/unit.cpp:236-277` 只验证一个手工构造序列满足
`coordinated < fifo`，没有验证“同一事务流在不同 coalescing/fragmentation 下时序等价”。

**影响**：稀疏加速、带宽利用率和协调收益对请求切分敏感，不能作为稳健的微架构结果。

### F-03 [P0] 输出和中间流量在结果产生前执行，sequential spill 还被重复计时

位置：

- `hygcn/paper_sim.cpp:968-1038`
- `hygcn/paper_sim.cpp:1110-1237`
- `hygcn/paper_sim.cpp:1200-1210`

所有 Edge、Input、Weight、Output 请求先一次性构造，再统一调用一次内存模拟。Edge/Input 和 Output 的
`enqueue_cycle` 都只是 `batch_id`（通常为 0、1、2...），并不是对应 AE/CE 生产或消费事件的周期。
内存模拟完成后，代码才计算 AE batch ready、CE start 和 CE finish。

这意味着：

- 最终 Output 可在 CE 计算产生数据前参与仲裁和占用 HBM；
- sequential 中间写回和读回可在 AE 完成前执行；
- 内存协调器会依据尚未真实产生的低优先级请求改变 Row/Bank 状态；
- `metrics.cycles = max(ce_finish, memory_service_cycles)` 无法恢复丢失的生产者依赖。

本次 Cora layer 0 的强制复跑结果提供了直接症状：全部 Output 请求在 cycle 950444 前完成，而 CE 到
cycle 1579449 才完成。流式输出可以与 CE 局部重叠，但不可能在缺少任何 tile/data-ready 事件的情况下，
让所有写回在全部生产者完成前合法结束。

sequential 路径还有两个额外问题：

1. 中间写和读使用两个不相交地址区间（read base 被放在全部 write 数据之后），而物理读回应访问刚写入
   的同一数据地址；这会改变 Row hit 和 Bank 映射。
2. 同一组中间读写已经进入 `MemoryCoordinatorModel::Simulate()` 并与 AE 请求竞争，随后
   `ce_start = ae_finish + ServiceCycles(intermediate_dram_bytes, peak_bw)` 又添加一次理想 spill 延迟。
   于是相同流量既提前影响 AE 内存完成时间，又在 AE 后再次计入关键路径。

这会系统性抬高 `sequential` 基线，当前 1.47-1.69x 流水加速虽然落入论文范围，但其因果来源不可靠。

**影响**：Fig. 16 的 pipeline speedup 和 coordination/pipeline 交互不能被当前门禁接受。

### F-04 [P0] 门禁验证的是“落入论文全局范围”，不是逐数据集 ±20% 复现

位置：

- `configs/paper_metrics.json:7-55`
- `tools/validate_paper_metrics.py:61-87`
- `tools/paper_benchmark.py:199-238`
- 原论文 §5.3 与 Fig. 15-17，PDF 第 26-27 页

论文正文提供的 `1.1-3x`、`27%-53%` 和 `50%-73%` 是跨数据集观察范围，不是每个数据集各自的
参考区间。当前 manifest 把同一个全局范围应用到 Cora、Citeseer 和 PubMed，再允许相对边界 20% 的
误差。其实际可接受区间被放宽为：

| 指标 | 声明范围 | 实际 PASS 范围 |
|---|---:|---:|
| sparsity speedup | 1.10-3.00x | 0.88-3.60x |
| pipeline speedup | 1.3699-2.1277x | 1.0959-2.5532x |
| pipeline DRAM ratio | 0.50-0.73 | 0.40-0.876 |

不同数据集的结果即使互换、趋势相反或偏离各自柱值，只要落在上述宽区间仍会 PASS。当前 Cora
sparsity speedup 为 1.0555x，低于论文报告下界，但仍被标记为 PASS。

此外，论文明确说明 Fig. 15 的 sparsity 实验“只运行 Aggregation Engine 以避免其他模块干扰”；当前
`paper_benchmark.py` 却用整个两层 GCN 的 `summary.total_cycles` 计算 sparsity speedup。参考值和被测量
对象不是同一实验口径。

论文正文明确给出的协调器平均值 3.70x/4.00x 已被正确修复，这是本轮的重要进步；但 Fig. 15-16
仍需保存可追踪的逐柱数字化值，或者把现有结论降级为“正文范围一致性检查”。

**影响**：11 项 PASS 是必要的范围 sanity check，不是逐数据集 ±20% 复现证据。

### F-05 [P1] benchmark 工作负载与论文 Table V 的 GCN 层配置不一致

位置：

- `hygcn/paper_sim.cpp:573-580`
- `tools/paper_benchmark.py:85-102`
- `tools/paper_benchmark.py:206-228`
- 原论文 Table V，PDF 第 23 页

论文 Table V 将 GCN 卷积层列为 `Add & |a_v^k|-128`。当前 paper simulator 对所有模型硬编码两个
shape：`input_features -> 128` 和 `128 -> num_class`，benchmark 又固定 `selected_layer=all` 并将两层
周期相加。第二层输出宽度因此随数据集变成 7/6/3，而不是论文配置中的 128。

该 shape 也没有从仓库 `gcn/gcn.ini` 读取；后者的两个 legacy 层均写成 128x128。当前结果既不严格
对应论文 Table V，也不对应仓库 legacy 模型配置。

**影响**：即便时序模型修复，当前总周期仍缺少与论文实验 workload 的一一映射。

### F-06 [P1] 请求级模型的精度边界已写清，但 HBM/计算时序参数仍无可追踪来源

位置：

- `configs/HYGCN_PAPER.ini:18-32`
- `hygcn/paper_sim.cpp:45-53`
- `hygcn/paper_sim.cpp:759-777`
- `docs/paper-reproduction.md:58-62`

原论文使用 cycle-accurate、execution-driven simulator，并集成 Ramulator；RTL、CACTI 和综合工具提供
模块延迟、功耗和面积。当前 paper 路径是自建请求级模型，不调用仓库内 DRAMSim3。这一点现在已在
文档中明确声明，属于合理的范围收缩。

但 `row_hit_cycles=12`、`row_miss_cycles=28`、`simd_efficiency=0.78`、
`independent_array_efficiency=0.72` 和 `cooperative_array_efficiency=0.82` 没有来源、校准集、敏感性区间
或 hold-out 结果。它们仍直接控制 Fig. 15-17 的关键周期。尤其当前 HBM 状态机不含 rank、读写切换、
刷新、队列容量和 backpressure，不能用“cycles”字段名推断论文级周期精度。

**影响**：可以接受为请求级研究模型，不能提升为 cycle-accurate 复现。

### F-07 [P1] Ping-pong 和事件测试没有覆盖真实的两半所有权及生产者依赖

位置：

- `hygcn/paper_sim.cpp:289-393`
- `hygcn/paper_sim.cpp:591-592`
- `hygcn/paper_sim.cpp:1089-1197`
- `hygcn_test/unit.cpp:280-300`
- `hygcn_test/unit.cpp:390-497`

`BuildShards()` 会把单个 batch 限制在 Aggregation Buffer 的半区内，这是正确改善。但实际 RunLayer
没有使用 `AggregationBufferModel` 的 segment/state，而是只维护全局 `buffer_used` 和 completion
release queue；没有 half 0/half 1 的归属、AE/CE 同半区互斥或读写端口冲突。

现有测试分别验证了一个脱离 RunLayer 的环形 allocator，以及 `ce_start <= ae_finish` 等方向性条件。
它们没有验证：

- Output enqueue 不早于相应 CE 数据 ready；
- intermediate read 不早于 write completion；
- 第三个 batch 必须等待某个 ping-pong half 被释放；
- 同一 block 流在不同请求切分下完成时间保持一致。

**影响**：默认 8/8 PASS 证明了守恒和回归稳定性，但不会发现 F-01 至 F-03。

### F-08 [P2] 完整论文覆盖仍缺失，但文档边界已经诚实

位置：

- `tools/paper_benchmark.py:10-35`
- `docs/paper-reproduction.md:58-62`

强制验收仍只有 GCN + Cora/Citeseer/PubMed。论文中的 IMDB-BIN、COLLAB、Reddit、GraphSAGE、
GINConv、DiffPool、CPU/GPU 基线、RTL、面积和完整能耗均未复现。GIN 与 GCN 仍共用同样的 shape，
GraphSAGE 主要只切换 MAX 并使用离线 sample。

本轮文档已明确不宣称这些结果，这是正确做法。该 finding 不单独否定“GCN 三数据集机制原型”，但会
否定“完整论文复现”。

---

## 3. 对上一轮问题的关闭情况

| 上一轮 finding | 当前状态 | 复审意见 |
|---|---|---|
| 错误论文参考值 | **部分关闭** | 3.70x/4.00x 等正文值已修正；逐数据集柱值和实验口径仍缺 |
| 协调收益由效率常数决定 | **部分关闭** | 常数已删除，请求时序已进入周期；请求粒度伪加速仍使结果失真 |
| 固定 overlap/spill 因子 | **部分关闭** | 固定因子已删除；output/intermediate 请求不满足因果，spill 重复计时 |
| 稀疏流量/地址不一致 | **部分关闭** | 流量和地址现在一致，但变成超出论文能力的精确 gather |
| paper 路径非 cycle-accurate | **未关闭，已降级声明** | 请求级模型可用，但不能与论文 Ramulator 精度等同 |
| 干净构建失败 | **关闭** | 标准 Release 命令成功 |
| 理想计算模型 | **部分关闭** | 增加模块和 batch 时间线；核心吞吐仍由固定 efficiency 决定 |
| 覆盖不足 | **未关闭，已明确边界** | 仍仅 GCN 三数据集 |
| 测试只验证内部自洽 | **部分关闭** | 测试更丰富，但缺因果和 fragmentation invariance |
| HBM 配置链路不一致 | **部分关闭** | paper path 自建 HBM 参数清晰；参数来源和真实后端仍缺 |

---

## 4. 独立复跑结果

### 4.1 仓库状态

- 本地与远端 HEAD：`d389f5a481abd8158f28c37a745a3a34ac7a06d4`；
- 分支：`run/npu`，跟踪 `origin/run/npu`；
- 拉取后源代码工作区干净；
- `git diff 03ad0ac..d389f5a --check`：通过；
- 本次 review 未修改实现、配置、测试或验收脚本。

### 4.2 构建与测试

| 检查 | 结果 |
|---|---|
| 干净 Release 配置与构建 | PASS |
| `ctest --output-on-failure` | 8/8 PASS |
| Legacy Cora snapshot | PASS |
| Legacy Citeseer snapshot | PASS |
| 论文参考 manifest | PASS |
| 强制重跑 3 数据集 x 4 variants | 完成 |
| 当前 metric validator | 11/11 required rows PASS |
| OpenSpec strict CLI | 未运行：环境未安装 `openspec` 命令 |

构建期间仅出现 DRAMSim3 原有编译 warning，没有构建错误。

### 4.3 强制 benchmark 结果

| 数据集 | 稀疏加速 | 稀疏输入比 | 流水加速 | 流水 DRAM 比 | 协调加速 | 带宽提升 |
|---|---:|---:|---:|---:|---:|---:|
| Cora | 1.055462 | 0.913377 | 1.474686 | 0.487340 | 3.576640 | 5.335001 |
| Citeseer | 2.083155 | 0.439998 | 1.654481 | 0.569497 | 3.200484 | 3.562207 |
| PubMed | 1.741607 | 0.559734 | 1.694886 | 0.577204 | 3.009394 | 3.372899 |
| 算术平均 | 1.626741 | 0.637703 | 1.608018 | 0.544680 | 3.262173 | 4.090036 |

validator 报告的协调加速误差为 11.83%，带宽提升误差为 2.25%。这些数值可确定性复现，也与提交文档
一致；本报告否定的是它们与论文结果之间的因果和验收充分性，不是否定运行可重复性。

原始产物保存在：

- `res/rereview-d389f5a/benchmark_report.json`
- `res/rereview-d389f5a/validation_report.md`
- `res/rereview-d389f5a/paper_*.json`

`res/` 被仓库 `.gitignore` 排除，避免 benchmark 输出污染源码提交。

---

## 5. 当前做得合理的部分

1. `<cstdint>` include 问题已修复，README 标准构建命令可以从新目录完成。
2. 论文正文明确给出的协调器 73% 时间节省和 4x 带宽提升已正确转换为 3.70x/4.00x。
3. 已删除 `coordinated_efficiency`、`uncoordinated_efficiency`、固定 overlap fraction 和经验 spill factor。
4. 协调器现在显式建模 Channel、Bank、Row hit/miss，排序和映射能够改变完成时间。
5. 稀疏请求现在使用实际顶点地址，traffic 和 request bytes 通过守恒检查，不再使用上一版的假连续地址。
6. 两种 Combination 模式都只从 HBM 读取一次权重，级联流量与 HBM 流量分开记录。
7. AE/CE 已有 batch ready、module ready、buffer release 和容量阻塞时间线，不再是固定重叠百分比。
8. paired ablation 会校验 manifest 只改变目标开关，并验证 binary/config/graph digest，缓存污染防护合理。
9. 文档明确限定为 GCN 三数据集的请求级机制检查，不再冒充 Ramulator/RTL/完整论文复现。

这些改进具有明确工程价值，建议保留；下一轮应修正请求和实验语义，而不是退回旧的标量模型。

---

## 6. 建议的下一轮接受标准

在重新宣布“达到上下 20% 的论文复现”前，至少应满足：

1. **还原论文稀疏算法**：按 Algorithm 4 生成连续 `[winstart, winend]` 请求，保留 Window 内部空洞；
   稀疏 ablation 只统计 Aggregation Engine，与 Fig. 15 同口径。
2. **统一事务发射模型**：将请求展开为 block/transaction 后进入一个有明确宽度和 backpressure 的全局
   队列；同一地址事务流不应因上层 `MemoryRequest` 切分而改变完成时间。
3. **建立生产者依赖**：Edge/Input 可预取，但 Output 必须在相应 CE tile ready 后 enqueue；中间 write
   必须在 AE ready 后执行，read 必须依赖 write completion，并访问同一物理地址。
4. **消除 sequential 重复计时**：中间读写只能由统一内存时间线计时一次，不再同时作为提前竞争流量和
   额外 `ServiceCycles` 延迟。
5. **对齐 benchmark workload**：明确 Fig. 15-17 使用的层数、输入/输出维度和模型阶段；从一个版本化
   workload manifest 生成 shape，不再硬编码 `128 -> num_class`。
6. **重建逐数据集参考**：数字化 Fig. 15-16 柱值，保存原图、坐标、提取值和误差；逐数据集分别过
   ±20%。若不数字化，只能把现有 gate 命名为“paper-reported range sanity check”。
7. **增加反例测试**：至少加入 fragmentation invariance、output data-ready、intermediate RAW dependency、
   ping-pong half ownership 和 AE-only sparsity 五类测试。
8. **提供参数证据和敏感性**：为 Row 时序和 compute efficiency 给出来源；至少报告参数 ±10% 时 Fig.
   15-17 指标的变化，并使用一个未参与校准的数据集作为 hold-out。

当 F-01 至 F-04 关闭，且 workload/reference 口径对齐后，可以接受为“HyGCN GCN 核心优化的可信
请求级性能复现”。在补齐 Ramulator/RTL、模型与数据集、面积和能耗前，仍不能称为完整论文复现。

---

## 7. 审查范围

本次只进行 review、独立构建、测试、强制 benchmark 和小型反例实验，没有修改工程实现。没有尝试
重建 CPU/GPU 软件基线、12nm RTL、CACTI、PrimeTime PX 或推理数值正确性。临时构建目录仅用于本次
验证，不属于交付源码。
