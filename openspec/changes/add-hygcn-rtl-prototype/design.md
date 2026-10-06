# Design

## 架构边界

RTL 原型以流接口复现论文最关键的硬件组织，不尝试把 C++ 请求级模型逐行翻译成门级结构。模块之间统一使用 ready/valid；当下游阻塞时，数据、元数据和状态 MUST 保持稳定。

## 聚合引擎

聚合引擎每周期接收一个 SIMD feature beat。`first` 初始化目标顶点累加器，`last` 提交结果；SUM 使用扩展精度累加，MAX 使用有符号比较。输入顶点和 batch 在一个 reduction 内不得变化。输出寄存器允许同周期消费旧结果并接收新 beat。

## 组合引擎

组合集群由多个 module 组成，每个 module 包含多个并行 array。每个 beat 提供共享 activation vector 与每个 array 独立 weight vector；`first` 清零累加器，`last` 生成各 array 的输出。该结构表达并行输出通道计算和跨 inner tile 累加，不建模 SRAM macro 或物理 systolic placement。

## 调度与缓冲

请求仲裁器在 FIFO 模式按 sequence 选择，在 batch-class 模式先选最小 batch，再按 Edge、Input、Weight、Output 排序。Aggregation Buffer 控制器使用两个 bank；bank 仅能按 FREE→AE_ACTIVE→READY→CE_ACTIVE→FREE 转移，防止 CE 读取未完成 AE 数据或 AE 覆盖未完成消费者。

## 验证策略

- 聚合：SUM、MAX、背压和非法元数据。
- 组合：多 beat 点积、并行 array 和输出背压。
- 仲裁：FIFO、batch-class、跨 batch 和同 batch class 顺序。
- 缓冲：双 bank 并行、满载阻塞、ready 后启动和 CE 完成回收。
- 综合：对 smoke wrapper 执行 Yosys `synth`，报告 wire/cell/memory 数；结果只作为开源逻辑综合代理。

## Non-goals

- 不声明论文 12 nm 面积、频率或功耗复现。
- 不实现 HBM PHY/控制器、AXI 完整协议、SRAM compiler macro、CDC、DFT 或布局布线。
- 不使用综合代理结果替代 PrimeTime/Design Compiler/物理签核。

