# Tasks

## 1. 方案与可审计基线

- [x] 1.1 固化 MEGA 论文机制、实现边界、公平基线和分级验收口径，并通过 OpenSpec strict validation
- [x] 1.2 建立机器可读论文 reference manifest 及校验器，并通过 `mega_reference_manifest` 测试验证 392 KB buffer、处理单元和目标字段
- [x] 1.3 记录分阶段开发方案、运行边界和预期产物，并验证 README 中的入口能够定位到 OpenSpec 与技术文档

## 2. Degree-Aware 量化输入

- [x] 2.1 实现量化 manifest schema、加载器、图/层哈希校验和来源分级，并用合法、缺字段、非法位宽和哈希不匹配测试验证
- [x] 2.2 实现符号保持、最近整数舍入、饱和和逐度数参数选择的 reference quantizer，并用边界值和多度数向量测试验证
- [ ] 2.3 实现 PyG 本地训练/导出入口和 diagnostic heuristic 入口，并验证 required benchmark 会拒绝 heuristic 来源
- [ ] 2.4 记录量化训练、导出和复跑方法，并用 Cora 最小层生成可被加载器接受的 manifest

## 3. Adaptive-Package

- [x] 3.1 实现 64/128/192 bit package、Mode/Bitwidth header 和同位宽连续节点贪心编码，并用位精确 golden vectors 验证
- [x] 3.2 实现独立 decoder、bitmap 和节点边界流，并用全零、稠密、跨 package、位宽切换及截断 mutation 验证往返一致性
- [x] 3.3 实现 payload/header/padding/bitmap/boundary/scale 与 DRAM 对齐流量统计，并用 64 B/128 B transaction 敏感性测试独立重算
- [x] 3.4 记录格式 schema 和已知论文歧义，并验证文档示例可由 codec 工具复现

## 4. Condense-Edge

- [x] 4.1 实现冻结 partition manifest 和跨子图唯一源节点清单生成器，并用重复边、图哈希不匹配和边覆盖测试验证
- [x] 4.2 实现每活跃子图 8-entry eID FIFO、首项比较和 refill 周期模型，并用命中、失配、超过八项和多子图测试验证
- [x] 4.3 实现 Combination/Sparse Buffer 双写、连续地址、区域容量、spill/refill 和去重流量，并用访问守恒 oracle 验证无漏边/重复消费
- [x] 4.4 记录 Condense-Edge 预处理和 trace 语义，并用固定小图复现论文“两次离散访问合并为一次连续访问”的机制示例

## 5. MEGA 周期引擎

- [x] 5.1 增加 `--engine mega`、paper/smoke profile 和独立结果目录，并验证既有 CLI 默认行为不变
- [x] 5.2 实现 4×8×32 BSE row-product bit-serial 组合模型，并用不同稀疏度/位宽的 microbenchmark 验证工作量和权重复用
- [ ] 5.3 实现 256 AU outer-product 聚合、16 bit partial sum、Encoder/Decoder 周期，并用多节点并行和背压测试验证
- [ ] 5.4 接入 392 KB buffer、ping-pong、有限端口和现有 HBM command model，并用延迟单调性、容量和 producer 因果测试验证
- [x] 5.5 记录 cycle formula、资源占用和 trace 字段，并用 microbenchmark 结果对照文档算例

## 6. 公平消融与指标

- [ ] 6.1 实现 H0、M0、M1、M2、M3 固定消融矩阵和共享参数 diff validator，并验证 baseline-only throttle 会被拒绝
- [x] 6.2 输出逐请求类 DRAM bytes/transactions、stage cycles、memory stall 和 total cycles，并由独立 validator 重算 speedup/reduction
- [x] 6.3 在 Cora、CiteSeer、PubMed 上强制运行 M0-M3，并验证完整 MEGA 相对 M0 的 DRAM bytes 与 total cycles 是否逐数据集同时下降
- [ ] 6.4 执行 package 长度、transaction 粒度、Sparse Buffer、分区和量化位宽敏感性，并验证报告披露全部参数 diff
- [x] 6.5 记录逐机制结果、论文 reference 对照和结论等级，并明确区分局部机制复现与完整论文数值复现

## 7. 集成交付

- [ ] 7.1 执行 clean Release、全部 CTest、legacy、paper、report、RTL 和 OpenSpec strict 回归，并保存最终通过摘要
- [ ] 7.2 生成最终 MEGA 报告、原始 manifest/CSV/trace 哈希和一键复跑入口，并验证报告中全部结论可从原始计数重算
- [ ] 7.3 提交并推送 `dev/mega`，回传完整 commit SHA、远端引用、最终 DRAM reduction、total-cycle speedup 和未关闭边界
