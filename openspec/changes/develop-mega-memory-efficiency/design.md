# Design

## Context

当前仓库有三类可复用基础：`paper` 请求级模拟器已经具备可追踪的 HBM 队列/命令时序、producer 因果和逐类流量；`legacy` 路径可提供原始 HyGCN 对照；`report` 分支已有 RTL 与 PyG-CPU 测量入口。MEGA 需要改变特征表示、组合计算工作量、跨子图访问顺序和 buffer 组织，但不应复制一套未经验证的 DRAM 时序模型。

论文没有公开逐节点位宽、scale、完整 simulator 或训练产物。正文只给出算法公式、平均位宽、硬件配置和平均性能，因此设计必须把“论文明确值”“本地训练值”“实现假设”和“诊断校准”分开。论文源固定为 `https://arxiv.org/html/2311.09775v1`。

## Goals / Non-Goals

**Goals:**

- 在同一图、同一层 shape 和同一 HBM 时序下，对比 32 bit 基线与 MEGA 的真实编码流量和请求完成时间。
- 实现 Degree-Aware、Adaptive-Package、Condense-Edge 三项机制，并能逐项关闭、单独消融和检查流量守恒。
- 以请求级 trace 证明 DRAM access 减少，以完整 producer-to-completion 时间线证明总时钟数减少。
- 将论文参数和目标值固化为机器可读 manifest，所有影响指标的覆盖项都进入结果和 revision audit。
- 为后续 RTL 扩展保留稳定的数据格式和模块边界，但先在 C++ 请求级模型中验证机制和收益。

**Non-Goals:**

- 第一阶段不宣称复现论文的训练精度、28 nm 面积、功耗或 1 GHz 时序签核。
- 不根据论文最终柱值反推逐节点位宽、DRAM 延迟、buffer 限速或 baseline-only throttle。
- 缺少 NELL、Reddit 或论文量化 manifest 时，不宣称复现正文的全 workload 平均值。
- 不在 MEGA 变更中修改既有 HyGCN 论文目标、legacy 快照或 RTL 功能语义。

## Decisions

### 1. 建立独立 `mega` engine，复用已验证的 HBM 服务层

新增 `mega/` 领域模块，CLI 通过 `--engine mega` 进入。图加载、层 shape、请求 trace schema、DRAM queue/command timing 和 trace validator 复用现有基础；特征格式、组合/聚合工作量、buffer 和调度由 MEGA engine 负责。

选择该方案是为了让 baseline 与 optimized 共用同一内存因果模型，并保持已有 command-level regression。备选方案是在 `paper_sim.cpp` 中继续堆叠开关，但这会让 HyGCN 校准参数与 MEGA 机制相互污染，难以形成公平消融。

### 2. 将量化训练产物定义为外部、版本化输入

每个 workload 使用 `mega_quantization_manifest.json`，至少包含：

- 数据集、模型、层、邻接和特征文件 SHA256；
- 入度定义以及 `degree -> bitwidth/scale` 映射；
- 每层权重 4 bit 及逐输出列 scale；
- 平均位宽、非零率、饱和计数和准确率摘要；
- `paper-derived`、`locally-trained` 或 `diagnostic-heuristic` 来源标签。

请求级模型只消费整数特征和 manifest，不在主仿真循环内训练。后续 PyG 训练工具按论文公式实现 degree-indexed 可学习 scale/bitwidth 和 memory penalty，再导出相同 schema。这样可以先完成硬件机制，同时防止启发式位宽被误当成论文训练结果。

### 3. 使用位精确 Adaptive-Package reference codec

codec 输入为节点顺序、每节点位宽、整数特征和 bitmap，输出为 package stream、bitmap stream、boundary metadata 和统计。默认 package 总长采用论文的 `(64, 128, 192)` bit，header 为 `Mode[1:0] + Bitwidth[2:0]`。

贪心规则为：相同位宽的连续节点可共享 package；下一节点位宽变化或剩余空间不足时关闭当前 package；所有剩余位计为 padding。编码与解码使用同一版本化 schema，但测试必须由独立 oracle 从 bitstream 恢复节点张量。

论文没有完整描述跨 package 节点边界的物理 metadata。实现将显式保存并计费最小边界流，而不是把边界信息当成免费 sideband。该成本单列，以便未来拿到作者格式后替换。

### 4. 同时保留 logical bytes 和 transferred bytes

每类数据均记录：有效 payload、格式 metadata、padding、对齐浪费、DRAM bytes 和 transaction 数。DRAM bytes 按 profile 的 transaction granularity 向上对齐，默认 required 配置依据论文 Condense-Edge 示例使用 128 B，并对 bundled HBM 配置对应的 64 B 做敏感性运行。

所有 speedup 和 reduction 只从完整请求流重算，不使用“理论压缩比 × 基线流量”替代实际访问。该选择会使早期结果低于论文理想值，但能防止漏算 bitmap、scale、refill、spill 和写回。

### 5. 将 Condense-Edge 拆为离线计划和在线执行

离线阶段消费冻结的 partition manifest，构建每个目标子图按源 ID 升序排列的跨子图唯一源节点列表，并生成列表哈希、边覆盖率和重复消除统计。required benchmark 只接受图哈希匹配的冻结清单；METIS 不存在时可使用确定性分区做开发，但只能标为 diagnostic。

在线阶段在组合结果产生时并行执行：

1. 必写 Combination Buffer；
2. 当前源 ID 与各活跃子图 8-entry eID FIFO 首项比较；
3. 命中时按目标子图连续指针写 Sparse Buffer；
4. 区域写满时生成真实 DRAM spill，后续聚合生成 refill/read；
5. 同一源节点在同一目标子图只存一次，在不同目标子图分别计费。

该拆分使图分区质量与硬件匹配成本可以独立消融，也能验证 Condense-Edge 没有通过漏边获得收益。

### 6. 按论文资源建立周期模型

默认 `MEGA_PAPER` profile 固化：

| 项目 | 默认值 |
|---|---:|
| Frequency | 1 GHz |
| HBM bandwidth | 256 GB/s |
| Input / Edge / Weight Buffer | 64 / 24 / 48 KB |
| Combination / Aggregation / Sparse Buffer | 96 / 128 / 32 KB |
| BSE | 4 tiles × 8 C-PE × 32 BSE |
| Aggregation Unit | 256 |
| Package | 64 / 128 / 192 bit |
| eID FIFO | 16 FIFOs，8 entries/FIFO |

组合阶段使用 row-product 和 bit-serial 工作量：每个非零值按实际位宽产生 bit slice，4 bit 权重在同一特征的各 bit 间复用。聚合阶段使用 outer-product，4 bit 组合结果在 256 个 AU 上传播并更新 16 bit partial sum。Encoder、Decoder、Condense、buffer port 和 DRAM 都是显式资源；ready 时间、占用和背压共同决定完成周期。

不会直接使用论文的平均 speedup 作为模型参数。组合和聚合吞吐先由确定性 microbenchmark 锁定，再进入端到端模拟。

### 7. 定义公平基线和固定消融矩阵

同一 workload 至少运行：

| ID | 配置 | 用途 |
|---|---|---|
| H0 | 现有 HyGCN | 论文跨架构参考 |
| M0 | 32 bit `A(XW)`、无压缩、无 Condense | 同执行顺序公平基线 |
| M1 | Degree-Aware + Bitmap | 量化贡献 |
| M2 | M1 + Adaptive-Package | 格式贡献 |
| M3 | M2 + Condense-Edge | 完整 MEGA |

M0-M3 的图、partition、HBM、buffer 总容量、request admission 和 trace 规则完全一致。只有被消融机制允许变化。现有 HyGCN 因执行顺序不同单独报告，不参与隐藏参数选择。

### 8. 分层定义成功标准

第一层为正确性：codec round-trip、边覆盖、流量守恒、producer 因果、完整 trace replay 和既有回归全部通过。

第二层为本地机制收益：在 Cora、CiteSeer、PubMed 的 available workload 上，M3 相对 M0 必须同时满足 `dram_bytes(M3) < dram_bytes(M0)` 与 `total_cycles(M3) < total_cycles(M0)`；同时报告 M1、M2、M3 的相邻增量，禁止只看最终平均值。

第三层为论文数值：只有完整论文 workload 和合格量化输入齐备时才启用。参考值包括：MEGA 相对 HyGCN 平均 speedup `38.3x`、DRAM access reduction `108.1x`；相对 HyGCN-C 的消融平均值为 Degree-Aware speedup `4.8x`、Adaptive-Package 增量 `4.7x`、Condense-Edge 增量 `1.1x`，对应 DRAM reduction `5.8x`、`2.5x`、`4.4x`。这些值在当前阶段是 reference，不是 required 门禁。

### 9. 产物与可复算性

每次运行输出一个 self-contained manifest、逐层 CSV 和压缩 trace。manifest 包含代码 SHA、输入哈希、配置 diff、量化/partition 来源、全部原始计数和结论级别。validator 从原始计数独立重算：

- 各数据类 logical/DRAM bytes 和 transactions；
- package utilization、padding 和 bitmap overhead；
- sparse-node 去重率、Sparse Buffer spill/refill；
- stage cycles、memory stall、total cycles；
- M0-M3 speedup 和 DRAM reduction。

最终用户交付只包含源代码、一键入口、最终报告、完整 SHA 和远端引用；开发期 smoke 和中间日志仅保留为内部门禁。

## Risks / Trade-offs

- [缺少论文逐节点量化参数] → required 结果必须使用本地训练 manifest 或作者产物；启发式配置只做 diagnostic，并在报告中降级结论。
- [论文未完整公开 package 边界 metadata] → 实现显式、保守计费的 boundary stream，并把开销单列和纳入敏感性。
- [现有数据集缺少 NELL/Reddit] → 先完成三数据集机制验证；论文平均值保持 reference，直到 workload 补齐。
- [METIS 依赖和版本导致分区漂移] → required benchmark 使用冻结 partition manifest 和图哈希，不在运行时隐式重分区。
- [请求级模型高估计算/访存重叠] → 所有 stage 使用 producer-ready、有限 queue/buffer 和端口占用，增加延迟单调性与容量反例。
- [目标导向调参] → revision audit 覆盖所有配置和影响调度的源码常量，报告 calibration workload、参数 diff 和 metric delta。
- [完整 trace 体积较大] → 使用可逆压缩和分块哈希，validator 必须能从完整流重算，不能只信任 summary/sample。

## Migration Plan

1. 在 `dev/mega` 提交 OpenSpec、论文 reference manifest 和技术方案，不改变现有可执行行为。
2. 实现量化 manifest/schema、Adaptive-Package codec 和独立单元测试。
3. 实现 partition manifest、Condense-Edge planner/executor 和流量守恒测试。
4. 实现 MEGA cycle engine、paper profile、CLI 和 microbenchmark。
5. 建立 M0-M3 benchmark、validator、敏感性和三数据集结果。
6. 全量执行 clean build、CTest、legacy/paper/RTL 回归和 OpenSpec strict validation，再提交最终实现与证据。

任何阶段都可通过删除 `mega` engine 和对应 CMake 入口回滚；现有 engine、配置和结果目录不被迁移或覆盖。

## Open Questions

- 后续若获得作者逐节点量化参数或 simulator，将作为新来源版本导入同一 manifest，不改变现有接口。
- NELL、Reddit 原始图和训练特征的获取方式在数据阶段确定；缺失时不阻塞三数据集机制开发。
