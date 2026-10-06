# HyGCN 可综合 RTL 原型

## 范围

`rtl/` 是一套独立于 C++ 模拟器的 SystemVerilog 原型，用于证明核心数据通路和调度规则可以表达为可综合硬件。默认参数与 `configs/HYGCN_PAPER.ini` 对齐：

| 结构 | RTL 默认值 | paper 配置 |
|---|---:|---:|
| AE cores | 32 | 32 |
| AE SIMD lanes/core | 16 | 16 |
| CE modules | 8 | 8 |
| CE arrays/module | 4 | 4 |
| CE inner lanes | 128 | 128 |
| Aggregation Buffer | 2 × 8 MiB | 16 MiB |
| memory request classes | 4 | Edge/Input/Weight/Output |

`tools/test_rtl_config.py` 自动检查这些结构参数，防止 RTL 与 paper 配置静默漂移。

## 模块

- `hygcn_aggregation_engine.sv`：单个 SIMD reduction core，支持有符号 SUM/MAX、`first/last` 顶点边界、ready/valid 背压和 sticky protocol error。
- `hygcn_aggregation_cluster.sv`：默认实例化 32 个独立 AE core，各 core 可在同一周期接收、累加和提交不同顶点。
- `hygcn_combination_cluster.sv`：默认 8 module × 4 array，每个 array 对 activation/weight inner tile 做并行点积并跨 beat 累加。
- `hygcn_request_arbiter.sv`：支持全局 FIFO 和 batch-class 优先级；batch-class 先选择最小 batch，再按 Edge、Input、Weight、Output 排序。
- `hygcn_aggregation_buffer_ctrl.sv`：双 bank 状态机，限制状态只能按 FREE→AE_ACTIVE→READY→CE_ACTIVE→FREE 转移。

所有数据通路均使用 ready/valid；输出阻塞时数据和元数据保持稳定。当前原型使用 packed vector 接口，尚未加入 AXI/HBM PHY 或真实 SRAM macro。

## 工具链

可复现环境定义在 `rtl/environment.yml`：

```bash
conda env create -p "$HOME/.cache/conda-envs/hygcn-rtl" \
  -f rtl/environment.yml
export HYGCN_RTL_TOOL_ROOT="$HOME/.cache/conda-envs/hygcn-rtl"
```

执行完整 RTL 验证：

```bash
make -C rtl all
```

或通过 CMake：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target rtl_verify
ctest --test-dir build --output-on-failure
```

`make -C rtl all` 依次执行：

1. Verilator lint；
2. Verilator deterministic testbench；
3. Yosys smoke synthesis，并生成 `build-rtl/hygcn_rtl_smoke_stat.json` 和结构化 netlist。

## 已验证行为

2026-10-06 的本机验证覆盖：

- 两 beat SIMD SUM：`[1,2,3,4] + [10,20,30,40] = [11,22,33,44]`；
- 有符号 MAX 和负数输入；
- 两个 AE core 同周期独立执行 SUM/MAX；
- 2 module × 2 array 的四路并行、多 beat 点积；
- 输出 ready 拉低时的稳定性和输入背压；
- FIFO oldest-sequence 仲裁；
- batch-class 的 earliest-batch 和 class 顺序；
- 双 bank 满载阻塞、AE-ready 后 CE launch、CE 完成后 bank 回收。

smoke 综合配置为 2 AE core、2 module × 2 array、4 inner lane，仅用于快速证明全部模块可综合。Yosys 0.69 展开后报告 47,943 个 generic primitive cell，其中 978 个 DFF/DFFE；该数字没有映射标准单元库，不能解释为论文面积或功耗。

## 尚未覆盖

- HBM PHY/完整控制器和 AXI 协议；
- SRAM compiler macro、clock gating、CDC、DFT 和 reset tree；
- CE 物理 systolic placement 与 wire delay；
- PDK、标准单元映射、STA、布局布线和 IR/EM；
- PrimeTime PX、SAIF/VCD 活动功耗和论文 12 nm 的 6.7 W/7.8 mm² 签核。

因此当前交付可以称为“可综合 RTL 功能原型”，不能称为“论文 12 nm RTL 实现已复现”。

