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

## ResearchReport.pdf 最终对比图

`report` 分支增加了报告第 9 页 Fig. 7/8 的可审计复刻流程。它从版本化的 PDF
矢量坐标恢复 15 组 speedup/energy 柱值，并以正文给出的算术平均值 `275x` 和
`4112x` 做统一锚定。运行下面的目标会同时复跑仓库现有四个数据集上的
GCN、GIN 和 GraphSAGE legacy 模型：

```bash
cmake --build build --target report_figures
```

结果位于 `res/report/`，包括合并版 PDF/PNG/SVG、独立图、逐工作负载 CSV、JSON
清单以及本地 simulator 原始输出。GraphSAGE 在采样文件缺失时使用确定性的 25
邻居 fallback；Reddit 因仓库缺少图文件只保留报告数字化柱值。当前能耗证据仅覆盖
DRAMSim3 的 DRAM 能耗，CPU、RTL 综合和 CACTI 数据仍属于报告校准输入，而非本地
独立实测。完整边界见 [docs/report-figures.md](docs/report-figures.md)。

### 本地 PyG-CPU 实测 speedup

安装与当前环境匹配的 PyG 后，可复跑 12 个具备图数据的 CPU inference 基线：

```bash
python3 -m pip install -r requirements-pyg-cpu.txt
python3 tools/pyg_cpu_benchmark.py \
  --threads 20 \
  --output-dir res/pyg-cpu
```

该测试使用 PyG 的 `GCNConv`、`GINConv` 和 `SAGEConv`，固定两层、hidden=128，默认
绑定 20 个 CPU 核，warmup 后以 7 次推理的中位 wall-clock 延迟作为 CPU 基线。
speedup 为本机 PyG-CPU 延迟除以 0.5 GHz HyGCN legacy simulator 延迟。它是项目
真实执行结果，但会记录并披露当前 CPU 型号；若主机不是报告中的双路 Xeon 4210R，
不得把结果当成原报告平台的严格复现。

## 可综合 RTL 原型

`rtl/` 提供 SystemVerilog 功能原型，默认结构参数对应 32×SIMD16 聚合集群、
8 个组合模块 × 每模块 4 个 array × 128 inner lanes、四类请求仲裁和双区
Aggregation Buffer。安装 `rtl/environment.yml` 中的 Verilator/Yosys 工具链后运行：

```bash
export HYGCN_RTL_TOOL_ROOT="$HOME/.cache/conda-envs/hygcn-rtl"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target rtl_verify
```

该目标执行 lint、确定性功能仿真和 smoke 综合。它证明核心模块可综合，但不包含
HBM PHY、SRAM macro、PDK、STA 或布局布线，不能替代论文 12 nm 面积和功耗签核。
模块和验证边界见 [docs/rtl-prototype.md](docs/rtl-prototype.md)。

## MEGA 内存效率开发

`dev/mega` 分支基于当前 `report` 提交，引入 MEGA 论文的 Degree-Aware 混合精度、
Adaptive-Package 和 Condense-Edge 技术路线。开发目标是基于完整请求流同时降低
DRAM access 与总时钟数，并通过固定 M0-M3 消融区分量化、格式和调度贡献。

当前阶段已完成 OpenSpec 行为契约、技术设计、任务分解和机器可读论文 reference：

```bash
python3 tools/validate_mega_reference.py
openspec validate develop-mega-memory-efficiency --strict --no-interactive
```

技术方案见 [docs/mega-development-plan.md](docs/mega-development-plan.md)，完整规范位于
`openspec/changes/develop-mega-memory-efficiency/`。在逐节点量化来源和论文完整 workload
齐备前，论文平均 speedup/DRAM reduction 仅作为 reference，不作为完整复现声明。

Degree-Aware manifest 与 Adaptive-Package codec 的格式、来源分级和运行方式见
[docs/mega-quantization-and-package.md](docs/mega-quantization-and-package.md)。
请求级周期/流量口径、Condense-Edge 语义和 M0-M3 一键复跑入口见
[docs/mega-request-model.md](docs/mega-request-model.md)。
