# MEGA 请求级实现与复跑

## 实现范围

`mega/` 是与既有 HyGCN `paper`/`legacy` 路径隔离的请求级 MEGA 模型。当前实现包含：

- Degree-Aware 量化 manifest、来源分级和 reference quantizer；
- 64/128/192 bit Adaptive-Package 位精确编解码；
- Condense-Edge 分区计划、16 路 8-entry eID FIFO、连续 Sparse Buffer 地址和 spill；
- M0-M3 固定消融、HBM command model 接入以及逐层 DRAM/周期分解；
- 三数据集 benchmark 和独立指标校验器。

当前仓库没有原始节点特征、作者量化产物或训练 checkpoint，因此默认 benchmark 使用
`diagnostic-heuristic` manifest。结果只证明本地机制与请求模型的收益，不属于完整论文数值复现。

## 固定消融

| ID | 特征/权重 | 特征格式 | 跨分区访问 |
|---|---|---|---|
| M0 | 32 bit / 32 bit | dense FP32 | 原始离散访问 |
| M1 | degree-aware / 4 bit | bitmap + 8 bit stored values | 原始离散访问 |
| M2 | degree-aware / 4 bit | Adaptive-Package | 原始离散访问 |
| M3 | degree-aware / 4 bit | Adaptive-Package | Condense-Edge 连续访问 |

四种配置共享图、层 shape、量化非零分布、partition、buffer、HBM 配置和 transaction 粒度。
validator 对完整 `architecture` 和共享 manifest 字段做逐项相等检查，防止 baseline-only throttle。

## 周期公式

每层报告下列互不隐藏的组成：

- M0 Combination：dense activation 的 32 个 bit slice；32 bit 权重按 4 bit BSE 宽度分八次处理。
- M1-M3 Combination：每个非零 activation 按其实际 bitwidth 发射，4 bit 权重在 activation bit slice 间复用。
- Aggregation：`ceil(edges * output_features * precision_factor / aggregation_units)`；M0 的
  32 bit 值按 4 bit 数据路径分八次处理，M1-M3 的 factor 为 1。
- Decoder：非零值吞吐加 Adaptive-Package header 解析周期。
- Encoder：`ceil(vertices * output_features / encoder_qn_units)`。
- Condense：源节点扫描比较周期加 eID FIFO refill 周期。
- Memory：Input/Weight/Edge preload、跨分区流和 Output write 分别进入现有 HBM command model；
  当前版本保守地串行累加三个 memory stage 与计算 stage，不声称尚未建模的跨 stage 重叠收益。

降低位宽、降低非零率和增加处理单元都有单调性单测；提高 HBM 延迟不得缩短总周期。

## 流量口径

每层同时输出：

- `logical_bytes`：有效 payload、bitmap、header、boundary、scale 和 spill 的逻辑总量；
- `dram_bytes`：各独立流按 128 B paper transaction（smoke 为 64 B）对齐后的传输量；
- `dram_transactions`：实际传输量除以 transaction 大小；
- Input、Weight、Edge、cross-partition、Output 五类分解。

Condense-Edge 的基线按每个目标子图唯一外部源的离散 transaction 计费；M3 将同一目标子图的源特征
写成连续 stream，并计入 Sparse Buffer spill write。固定小图测试覆盖论文式的“两次 64 B 离散访问
合并为一次 128 B 连续访问”，同时验证源节点和边计数守恒。

## 一键复跑

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target mega_benchmark
```

等价的显式命令为：

```bash
python3 tools/mega_benchmark.py \
  --binary build/hygcntest \
  --output-dir res/mega \
  --force
python3 tools/validate_mega_metrics.py \
  --report res/mega/benchmark_report.json \
  --output res/mega/validation_report.md \
  --require-local-gate
```

最终报告分别列出 M1/M0、M2/M1、M3/M2 和 M3/M0。局部门禁要求 Cora、CiteSeer、PubMed
每个数据集都满足 `dram_bytes(M3) < dram_bytes(M0)` 且 `total_cycles(M3) < total_cycles(M0)`。
论文的 38.3x speedup 和 108.1x DRAM reduction 只保留为 reference。

## 未关闭边界

- 尚未实现 PyG 训练和 locally-trained 特征/权重导出；required 运行会拒绝 diagnostic 输入。
- 默认开发分区是确定性连续分区，不是 METIS；required 运行同样会拒绝。
- 当前没有 NELL/Reddit workload，不计算论文五数据集平均值。
- 当前模型不提供 28 nm 功耗、面积、STA 或 RTL 综合结论。
