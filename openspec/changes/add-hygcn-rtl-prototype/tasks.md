# Tasks

## 1. 规格与模块

- [x] 1.1 定义 RTL 原型边界、接口契约和非目标
- [x] 1.2 实现参数化 SIMD SUM/MAX 聚合引擎
- [x] 1.3 实现多模块、多阵列组合引擎集群
- [x] 1.4 实现 FIFO/batch-class 请求仲裁器
- [x] 1.5 实现双区 Aggregation Buffer 控制器

## 2. 验证与综合

- [x] 2.1 增加聚合、组合、仲裁和缓冲 SystemVerilog testbench
- [x] 2.2 增加 Verilator lint/仿真脚本并纳入 CTest
- [x] 2.3 增加 Yosys smoke 综合脚本和可审计统计
- [x] 2.4 执行原有 CTest，确认 C++ 路径不回退

## 3. 文档与交付

- [x] 3.1 记录模块映射、运行命令、已验证行为和 ASIC 签核边界
- [x] 3.2 提交并推送当前 `report` 分支
