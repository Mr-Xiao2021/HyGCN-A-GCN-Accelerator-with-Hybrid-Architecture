# HyGCN Simulator

本仓库包含两条可并行使用的执行路径：

- `legacy`：保留原始事件驱动模拟器，用于 Cora/Citeseer 周期与流量快照回归。
- `paper`：面向 HyGCN 论文核心机制的可配置模块级模型，用于架构消融和相对性能验收。

## 构建与快速自检

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

默认测试只使用小型 smoke 配置，并检查配置解析、分区、地址、缓冲区、事务边界、
组合模块调度、六种策略组合、CLI 错误路径、确定性输出和论文指标校验器。

## 单次实验

```bash
./build/hygcntest \
  --engine paper \
  --profile paper \
  --model gcn \
  --dataset cora \
  --scope full \
  --layer 0 \
  --pipeline latency-aware \
  --combination independent \
  --sparsity on \
  --priority batch-class \
  --mapping low-bits \
  --seed 1 \
  --output-dir res/manual
```

每次运行生成 JSON 清单和 CSV 层级统计。JSON 记录代码版本、输入与配置摘要、全部开关、
AE/CE 时间点、操作数、各级流量、请求等待、producer-ready/入队/发射/完成周期、
连续输入窗口及 HBM channel/bank 分布。`--scope aggregation --layer 0` 用于 Fig. 15
同口径的第一层 AE-only 实验，不生成 Weight、CE、Output 或中间流量。

## 完整验收

```bash
cmake --build build --target legacy_regression
cmake --build build --target paper_benchmark
```

`legacy_regression` 复跑 GCN+Cora/Citeseer 并与版本化快照逐项比较。
`paper_benchmark` 运行 Cora、Citeseer、PubMed 的优化版和稀疏、流水、priority-only、
mapping-only、combined 消融。Fig. 15/16
使用版本化 SVG 坐标数字化的逐数据集柱值，Fig. 17 使用论文正文平均值，统一执行
`±20%` 相对误差校验。报告写入 `res/paper/benchmark_report.json` 和
`res/paper/validation_report.md`。`configs/paper_workloads.json` 固定 Fig. 15-17 的
Table 5 layer-0 shape；参数报告根据 `configs/paper_parameter_baseline.json` 自动生成差异，
不使用硬编码重标定声明。物理 16 MiB Aggregation Buffer 与 5 MiB 图分区调度上限分别记录，
并在正式证据中保留 4/5/6 MiB 敏感性对照。

实现范围、指标定义和声明边界见 [docs/paper-reproduction.md](docs/paper-reproduction.md)。
