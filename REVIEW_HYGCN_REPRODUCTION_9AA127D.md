# HyGCN 论文复现独立复审报告（9aa127d）

## 1. 复审对象与结论

**复审对象**：`run/npu` 分支，远端提交
`9aa127d248849e68160c7aafd70b79cbe468094a`。其中实现提交为
`44ff038da0fb563e06141b880e135a7a0581cbf3`，证据提交为 `9aa127d`。

**对比基线**：上一轮独立复审提交 `231b74a3b45bc8eb18255183c9ec9243fda110ff`
及 `REVIEW_HYGCN_REPRODUCTION_D389F5A.md`。

**论文依据**：Mingyu Yan et al., *HyGCN: A GCN Accelerator with Hybrid
Architecture*, HPCA 2020。本文重新核对了
[arXiv HTML](https://arxiv.org/html/2001.02514v1) 及其 Fig. 15/16 原始 SVG。

**最终判定：F-01 至 F-04 的定向整改真实有效，但当前版本仍未达到“可信地复现论文
Fig. 15-17 请求级相对性能”的验收预期。**

本轮确实关闭了上一版最直接的四类错误：连续稀疏窗口、请求碎片不变性、Output/intermediate
生产者与 RAW 因果、以及 Fig. 15 AE-only 和逐数据集柱值门禁。干净构建、9/9 CTest、legacy
回归、强制 benchmark 和提交的 artifact hashes 都可以独立复现。

不过，新门禁仍有三项会改变论文结论的基础问题：

1. Fig. 17 的全部 `3.2936x` 收益来自协调开关同时切换的一套无论文来源的基线地址映射；保留
   排序策略、仅统一物理映射后，协调加速和带宽提升都变为 `1.0x`。
2. Fig. 16 sequential 基线把每个 batch 未使用的 Aggregation Buffer 容量也当作 HBM 中间流量。
   仅改为实际产出字节后，Cora DRAM 比的误差从 15.84% 变为 25.87%，强制门禁失败。
3. Fig. 16/17 仍把 `128 -> num_class` 分类层计入完整运行，而论文 Table 5 声明的 GCN
   卷积层是 `|a_v^k| -> 128`。只跑论文对应的 layer 0 时，Cora Fig. 16 DRAM 比误差约 25.1%。

因此当前产物可以描述为：

> 可重复运行、具备较好因果检查的 HyGCN GCN 三数据集请求级研究原型；Fig. 15 的
> Window Sliding & Shrinking 相对实验已获得可信的机制级支持。

不宜描述为：

> Fig. 15-17 已全部在逐柱上下 20% 内完成可信论文复现，或当前 PASS 证明了论文协调器和
> 流水机制的性能因果。

| 维度 | 本次判定 |
|---|---|
| F-01 连续窗口 | 关闭 |
| F-02 fragmentation invariance | 关闭 |
| F-03 Output/intermediate 因果 | 对所测范围关闭 |
| F-04 AE-only 与逐柱参考 | 关闭 |
| Fig. 15 稀疏机制 | 基本达到请求级复现预期 |
| Fig. 16 流水机制 | 门禁依赖错误的中间流量计费和工作负载 |
| Fig. 17 协调机制 | 门禁依赖未论证的地址映射基线 |
| 总体论文复现 | 仍不接受 |

---

## 2. 已确认关闭的问题

### C-01 连续 Window Sliding & Shrinking 语义正确

`BuildShards()` 现在发出从第一个到最后一个有效邻居的单个连续窗口，内部空洞保留在请求中。
黄金用例 `{0,2,4,6}` 正确得到 `[0,7)`、`448B`、3 个内部空洞和 7 个 64B 事务。

这与论文 Algorithm 4 的滑动首边界、收缩尾边界语义一致，不再是上一版的精确离散 gather。
`window_requests.csv` 也保留了论文运行的完整窗口证据。

### C-02 同一 block 流不再因请求切分获得伪加速

内存模型先将请求展平为统一 block 事务流，再执行 channel/bank 时序。独立复跑确认，同一 128-block
地址流的单请求与 128 个单-block 请求都在 271 cycles 完成，row hit/miss 都是 64/64，channel/bank
分布相同。

这关闭了上一版中相同访问集合因 fragmentation 产生 4.73x 差异的问题。

### C-03 Output 与 intermediate RAW 因果已建立

Output 请求在 CE 生产者完成后入队。Sequential intermediate read 与 write 使用相同地址和长度，
read 的 producer-ready 等于 write completion；旧版的解析 spill 二次计时也已经删除。

单元反例和强制论文运行都能验证 `enqueue >= producer_ready`、`first_issue >= enqueue`，以及
intermediate 读写的地址、字节和时序一致性。

### C-04 Fig. 15 范围和论文柱值已修正

六个 Fig. 15 运行固定为 layer 0、AE-only，只包含 Edge/Input 流量。Weight、CE、Output 和
intermediate 均为零，优化与基线只切换 sparsity。

本次独立下载并校验了 arXiv HTML 引用的 Fig. 15/16 SVG：

- Fig. 15 SHA256：`807e32b119ff28b157c8d61e9c54ee5f61554d2246194a82b6a925d26a6b5f1b`
- Fig. 16 SHA256：`3e231d91f2d25dacc983178d916402ebfd572d9f532561f4fdfe98a3af06b5ed`

清单中的 baseline、full-scale 和柱顶坐标能够重新算出保存的逐数据集参考值。参考值本身正确。

---

## 3. 阻塞性 Findings

以下 findings 按严重度排序。`P0` 表示会使当前论文门禁结论失效，`P1` 表示重要机制或证据缺口，
`P2` 表示范围或覆盖风险。

### R3-01 [P0] Fig. 17 的 PASS 完全由另一套地址映射产生，优先级协调在完整 benchmark 中贡献为零

位置：

- `hygcn/paper_sim.cpp:30-31`
- `hygcn/paper_sim.cpp:348-352`
- `hygcn/paper_sim.cpp:462-485`
- `hygcn/paper_sim.cpp:540-560`
- `tools/paper_benchmark.py:43-50`

`memory_coordination` 不只改变请求排序，还切换了两套物理 block 映射：

- on：低位依次映射 channel、bank、row；
- off：使用代码内硬编码的二路 bank interleave 和 row-first 映射。

论文的协调器确实包含请求优先级和 channel/bank 数据布局，因此开关覆盖两者并非天然错误。问题在于，
当前实现没有为 off 路径的 `kUncoordinatedBankInterleave = 2` 提供论文依据或外部基线证据，而且当前
Fig. 17 收益全部由这项映射切换产生，而不是文档声称的最早 batch/请求类别仲裁。

本次在隔离 worktree 做了单变量反事实：保留 `Order(..., coordinated)` 和其全部排序逻辑，只让 on/off
使用相同低位映射。结果如下：

| 指标 | 原实现 | 同映射反事实 | 论文目标 |
|---|---:|---:|---:|
| 协调加速，三数据集平均 | 3.293600x | 1.000000x | 3.70x |
| 带宽提升，三数据集平均 | 3.293600x | 1.000000x | 4.00x |
| 相对误差 | 10.98% / 17.66% | 72.97% / 75.00% | <=20% |

除 Fig. 17 外，Fig. 15/16 所有结果完全不变。这证明完整 benchmark 中的协调排序开关没有可观察
贡献，当前 `3.2936x` 完全来自物理映射差异。

单个 8KiB 请求的反例进一步排除了请求排序：请求只有一个，无法重排；on/off 仍分别为 60 和 466
cycles，产生 `7.7667x` 表观收益。该收益只能来自地址映射。

同时，on 路径的 row-buffer hit rate 在三个数据集上都更低：

| 数据集 | coordination on hit/miss | hit rate | off hit/miss | hit rate |
|---|---:|---:|---:|---:|
| Cora | 874885 / 33408 | 96.32% | 878991 / 29302 | 96.77% |
| Citeseer | 2519065 / 283008 | 89.90% | 2707397 / 94676 | 96.62% |
| PubMed | 5876854 / 220728 | 96.38% | 5902208 / 195374 | 96.80% |

这与论文用协调器改善 row locality 和带宽利用率的因果解释不一致。当前模型更准确的描述是“用更高
channel 并行度交换 row hits”，但仓库没有证明这就是论文的 baseline/optimized 映射。

**影响**：Fig. 17 的两项 PASS 不能作为 Memory Access Coordinator 的复现证据。

**关闭条件**：从论文或作者实现追踪两套地址布局；分别报告 priority-only、mapping-only 和 combined
ablation；增加能证明跨 batch 优先级改变 issue/completion 的 trace 反例，而不是只看最终平均值。

### R3-02 [P0] Sequential spill 按 4 MiB 槽位而非实际产出计费，直接决定 Fig. 16 Cora 是否 PASS

位置：

- `hygcn/paper_sim.cpp:343-346`
- `hygcn/paper_sim.cpp:1006-1012`
- `hygcn/paper_sim.cpp:1078-1081`
- `hygcn/paper_sim.cpp:1283-1322`

每个 raw batch 的实际聚合结果是 `aggregation_bytes`。但 `spill_bytes` 被对齐到
`AggregationShardCapacityBytes()`，即 16 MiB Aggregation Buffer 除以两个 ping-pong region 和每区
两个 residency bank 后得到的固定 4 MiB。结果是每个 sequential batch 无论实际产生多少数据，都向
HBM 写 4 MiB、再读 4 MiB。

提交证据中的膨胀如下。`倍数 = intermediate_dram_bytes / (2 * actual aggregation bytes)`：

| 数据集 | layer | 实际单向产出 | 记录的读写流量 | 倍数 |
|---|---:|---:|---:|---:|
| Cora | 0 | 15,598,080B | 33,554,432B | 1.076x |
| Cora | 1 | 1,386,496B | 8,388,608B | 3.025x |
| Citeseer | 1 | 1,703,424B | 8,388,608B | 2.462x |
| PubMed | 1 | 10,095,104B | 25,165,824B | 1.246x |

缓冲槽位决定地址预留上限，但没有理由把未写入的空闲容量算作 DRAM transaction。现有测试只验证
`request bytes == reported bytes`，因此会接受“请求和统计同时多算”的内部自洽结果。

本次只改一行，将：

```cpp
batch.spill_bytes = Align(batch.aggregation_bytes, batch_capacity);
```

改为实际 block 对齐：

```cpp
batch.spill_bytes = Align(batch.aggregation_bytes, architecture_.block_size);
```

其余实现、配置、工作负载和验收脚本均不变。强制三数据集结果为：

| 指标 | Cora | Citeseer | PubMed |
|---|---:|---:|---:|
| 原 pipeline speedup | 2.062418 | 2.186294 | 1.561088 |
| 实际字节 speedup | 1.949324 | 2.151559 | 1.540504 |
| 原 pipeline DRAM ratio | 0.580879 | 0.621853 | 0.781589 |
| 实际字节 DRAM ratio | 0.631171 | 0.636975 | 0.794479 |
| 实际字节相对误差 | **25.87% FAIL** | 18.88% PASS | 8.57% PASS |

**影响**：Fig. 16 的 6/6 PASS 依赖把未使用 buffer capacity 当成 HBM 流量，Cora 的关键门禁在修正
后失败。

**关闭条件**：中间写回按真实 producer bytes 建模；新增测试断言每批 write/read 字节等于实际聚合
输出的 block 对齐值；重新生成 Fig. 16 证据。

### R3-03 [P1] Fig. 16/17 工作负载仍含论文 Table 5 未声明的分类层

位置：

- `hygcn/paper_sim.cpp:691-698`
- `tools/paper_benchmark.py:10-18`
- `tools/paper_benchmark.py:35-50`

`GetLayerShapes()` 对 GCN 固定返回两层：`input_features -> 128` 和 `128 -> num_class`。除 Fig. 15
外，benchmark 的 optimized、pipeline baseline 和 coordination baseline 都使用 `layer=all`。

论文 Table 5 对 GCN 的卷积层配置写为 `Add & |a_v^k|-128`，没有给出 `128 -> 7/6/3` 分类层作为
Fig. 16/17 的被测 workload。当前第二层输出宽度还随数据集类别数改变，缺少论文实验配置的对应关系。

本次将相同二进制的 full ablation 限定到 layer 0，结果为：

| 数据集 | pipeline speedup | pipeline DRAM ratio | coordination speedup |
|---|---:|---:|---:|
| Cora | 1.987063 | 0.627193 | 3.085715 |
| Citeseer | 2.164024 | 0.637855 | 3.305673 |
| PubMed | 1.540572 | 0.810196 | 3.536506 |

Cora Fig. 16 DRAM ratio 相对论文柱值 `0.501466` 的误差约 25.1%，会失败。换言之，当前 PASS 还
依赖纳入那一层未映射到论文配置的分类工作量。

**影响**：Fig. 16/17 的实验对象与论文 workload 尚不能一一对应。

**关闭条件**：明确论文每个 figure 的层形状、层数和汇总方法；使 benchmark shape 来自版本化 workload
manifest，并为每个 figure 输出 layer selection 证据。

### R3-04 [P1] Edge 到 Input 的生产者关系未建模，当前协调器不是动态 batch 仲裁

位置：

- `hygcn/paper_sim.cpp:1112-1155`
- `hygcn/paper_sim.cpp:1169-1201`
- `hygcn_test/unit.cpp:593-658`

论文描述的 prefetch 次序是先取 Edge，Sparsity Eliminator 得到邻居索引，再取对应 Input features。
当前实现为同一个 shard 的 Edge 和 Input 都直接使用 `enqueue_cycle = batch_id`，所有未来 batch 的请求
也在第一次 `simulate()` 前一次性生成。Input 没有等待 Edge completion 或 sparsity metadata ready。

因此当前请求排序只是在静态、预知未来的请求集合上 stable-sort。它没有模拟运行时出现的关键冲突：
当前 batch 的低优先级请求与下一 batch 后来才产生的高优先级请求同时竞争。R3-01 中，同映射后排序
贡献恰好为 `1.0x`，与这一结构性缺口一致。

F-03 测试很好地覆盖了 Output 和 intermediate，但没有覆盖 Edge -> Input producer dependency。

**影响**：即使修正地址布局，当前模型也不足以证明论文的动态 batch 优先级机制。

### R3-05 [P1] `parameter_recalibration=false` 是硬编码声明，不是可验证结论

位置：

- `hygcn/paper_sim.cpp:29-31`
- `tools/paper_benchmark.py:275-296`
- `res/review-v3/recalibration.txt`

`paper_benchmark.py` 无条件把 `parameter_recalibration` 写成 `False`。配置 SHA256 只证明
`HYGCN_PAPER.ini` 在 `d389f5a..9aa127d` 间未变，不能证明 source-level 参数未新增或未调节。

实现提交新增了 `kAggregationResidencyBanksPerRegion = 2` 和
`kUncoordinatedBankInterleave = 2`。前者派生 4 MiB batch capacity 并触发 R3-02，后者直接控制
R3-01 的 Fig. 17 收益。它们都是配置文件之外、实质改变验收指标的模型参数。

这不证明开发者主观进行了拟合，但证明当前自动证据无法支持“无参数重标定”的结论。

**关闭条件**：将所有行为控制参数纳入版本化配置和报告；记录相对上一验收基线的 config/source
parameter diff；对关键离散参数做敏感性分析或给出论文来源。

---

## 4. 其他重要风险

### R3-06 [P1] Ping-pong 仍是总容量 release queue，而非显式两半所有权

论文说明 Aggregation Buffer 分成两个 chunk 供 AE/CE ping-pong。当前 shard 容量派生考虑了
ping-pong，但实际 pipelined RunLayer 使用全局 `buffer_used` 和 completion release priority queue，
没有 half 0/half 1 归属、同半区冲突或第三批必须等待指定 half 的状态。

这比上一版更接近容量约束，也不影响本轮已建立的 Output/intermediate 因果，但仍不足以把模型称为
论文 ping-pong 的精确实现。

### R3-07 [P2] 参考数字化正确，但 validator 不会重新数字化或校验 SVG 内容

`validate_reference_metrics.py` 检查 SHA、坐标字段和正数参考值是否存在，却不下载/校验 SVG hash，
也不从坐标重新计算柱值。本次人工独立核验表明当前值正确，所以这是证据自动化缺口，不是当前参考值
错误。

### R3-08 [P2] 复现范围仍是 GCN 三数据集请求级模型

仓库仍未复现论文的全部模型/数据集、CPU/GPU 基线、Ramulator/execution-driven 周期精度、RTL、综合、
面积和完整芯片能耗。文档已经诚实声明这些边界，因此不将其视为本次回归，但总体结论必须继续限定为
请求级机制原型。

---

## 5. 对 review v2 findings 的关闭判定

| review v2 finding | 当前状态 | 独立复审意见 |
|---|---|---|
| F-01 连续窗口 | **关闭** | 黄金反例和论文运行 trace 都符合连续区间语义 |
| F-02 fragmentation 时序伪差 | **关闭** | 相同 block 流的周期、hit/miss 和分布保持一致 |
| F-03 Output/intermediate 因果 | **关闭所测范围** | 已建立 producer/RAW，未覆盖 Edge -> Input |
| F-04 Fig.15 scope 与逐柱门禁 | **关闭** | AE-only 和 SVG 柱值均独立验证正确 |
| F-05 workload 映射 | **未关闭** | full 仍包含 `128 -> num_class` 层 |
| F-06 参数来源 | **未关闭** | 新增两个关键 source-level 参数且没有论文依据 |
| F-07 ping-pong/因果覆盖 | **部分关闭** | Output/intermediate 已补齐，显式 half ownership 未补齐 |
| F-08 完整论文覆盖 | **未关闭，边界声明正确** | 保持 GCN 三数据集请求级范围 |

---

## 6. 独立复跑与证据核验

### 6.1 仓库与提交证据

- 本地/远端 HEAD：`9aa127d248849e68160c7aafd70b79cbe468094a`；
- 分支：`run/npu`，跟踪 `origin/run/npu`；
- `git diff --check 231b74a..9aa127d`：PASS；
- `res/review-v3/artifact_sha256.txt`：全部列出文件通过 SHA256 校验；
- `HYGCN_PAPER.ini` SHA256：
  `6e3b29fc9f93c578c8c850fde98bc7a4d426b0666624ef52f2a6ed596cdf821d`；
- 该配置相对 `d389f5a` 未变，但相对最初的 `03ad0ac` 有变化。

### 6.2 构建和测试

| 检查 | 独立结果 |
|---|---|
| 全新目录 Release configure/build | PASS |
| `ctest --output-on-failure` | 9/9 PASS |
| Legacy Cora snapshot | PASS |
| Legacy Citeseer snapshot | PASS |
| 3 数据集 x 5 variants 强制 benchmark | 完成 |
| 原 validator | 14/14 required rows PASS |
| artifact SHA256 | 全部 PASS |
| OpenSpec strict CLI | 本环境未安装 `openspec`，未独立复跑 |

提交的 `res/review-v3/openspec.log` 记录为 PASS；本报告只区分“提交证据存在”和“本机独立执行”，
不把无法执行的命令误记为独立 PASS。

### 6.3 原始强制 benchmark

本次强制复跑与提交证据一致：

| Metric | Cora | Citeseer | PubMed | 原门禁 |
|---|---:|---:|---:|---|
| Fig. 15 speedup | 1.138446 | 3.334554 | 1.156091 | 3/3 PASS |
| Fig. 15 AE DRAM ratio | 0.869152 | 0.292966 | 0.859107 | 3/3 PASS |
| Fig. 16 speedup | 2.062418 | 2.186294 | 1.561088 | 3/3 PASS |
| Fig. 16 DRAM ratio | 0.580879 | 0.621853 | 0.781589 | 3/3 PASS |
| Fig. 17 coordination speedup | 3.057030 | 3.292259 | 3.531512 | aggregate 3.293600 PASS |
| Fig. 17 bandwidth gain | 3.057030 | 3.292259 | 3.531512 | aggregate 3.293600 PASS |

本报告不质疑这些数值的确定性或证据文件完整性；质疑的是 R3-01 至 R3-03 证明的实验因果和工作负载
口径。

---

## 7. 建议的下一轮硬性验收条件

1. **Fig. 16 traffic oracle**：每个 sequential batch 的 intermediate write/read bytes 必须等于真实
   聚合输出的 block 对齐字节，禁止按 buffer slot capacity 计费。
2. **Fig. 17 分解消融**：同一 request trace 下分别运行 priority-only、mapping-only、combined；每项
   输出 channel/bank 分布、row hit rate、issue/completion timeline。
3. **地址映射来源**：on/off 的映射公式和所有 interleave 常数必须有论文、作者实现或明确基线规范来源。
4. **动态依赖**：Input enqueue 不早于对应 Edge/邻居索引 ready；测试构造“下一 batch Edge 到达时，
   当前 batch 低优先级请求仍在队列”的真实仲裁反例。
5. **Workload manifest**：Fig. 15-17 的模型、层数、输入/输出维度和聚合方法逐项绑定到论文证据，
   禁止由 `num_class` 隐式生成未声明层。
6. **参数可审计**：所有行为控制常数进入 manifest/config；`parameter_recalibration` 由 diff/allowlist
   自动计算，不能硬编码。
7. **保留现有回归**：F-01 至 F-04、9/9 CTest、legacy、artifact hashes 和逐柱 20% 门禁均不得退化。

满足以上条件后，才适合再次判定 Fig. 15-17 是否整体达到可信的请求级复现标准。
