# Proposal

## Why

当前仓库只有 C++ 请求级模拟器，无法证明 HyGCN 数据通路能够被综合为硬件，也无法生成独立的逻辑资源和时序代理结果。本变更增加一套与现有架构参数对应的可综合 SystemVerilog 原型，并用确定性仿真和开源综合流程验证核心机制。

## What Changes

- 增加参数化的 SIMD 聚合引擎，支持 SUM/MAX、顶点边界、ready/valid 背压和协议错误检测。
- 增加参数化的组合引擎集群，以多模块、多阵列并行点积表示论文的 8 模块 × 4 阵列组织。
- 增加 batch-class 内存请求仲裁器，支持 FIFO 与“最早 batch、batch 内 Edge→Input→Weight→Output”两种策略。
- 增加双区 Aggregation Buffer 控制器，显式建模 allocate、AE-ready、CE-launch、reclaim 和容量阻塞。
- 增加 SystemVerilog testbench、Verilator 仿真入口和 Yosys 综合入口，输出可复跑日志和统计。
- 明确原型不包含 HBM PHY、SRAM macro、CDC、DFT、物理实现和 12 nm PDK 签核。

## Capabilities

### New Capabilities

- `hygcn-rtl-prototype`: 定义 HyGCN 核心数据通路与调度机制的可综合 RTL、接口契约、验证和开源综合要求。

### Modified Capabilities

无。

## Impact

- 新增 `rtl/`、`docs/rtl-prototype.md` 和 CMake/CTest RTL 入口。
- 不改变现有 legacy/paper C++ 模拟器结果。
- 默认 paper 参数用于结构配置；测试和开源综合使用缩小参数，以控制运行时间。

