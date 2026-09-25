# Tasks

## 1. 配置、入口与基线

- [x] 1.1 固化当前 GCN+Cora/Citeseer 的 legacy 周期与流量快照，并通过回归测试验证现有可运行结果可重复生成
- [x] 1.2 将架构参数、运行参数和机制开关拆分为显式配置对象，新增 paper、legacy、smoke 三个配置档，并通过配置解析与非法值单元测试
- [x] 1.3 将 `hygcntest` 改造成可选择模型、数据集、层、策略、种子和输出目录的命令行入口，并验证缺失输入返回非零状态和明确错误
- [x] 1.4 增加 JSON/CSV 运行清单，记录代码版本、输入摘要、有效配置和全部开关，并通过重复运行测试验证确定性字段一致
- [x] 1.5 在 CMake/CTest 中注册配置与 CLI 测试，并验证默认 `ctest` 不依赖大型或缺失数据集

## 2. 地址、流量与统计正确性

- [x] 2.1 为每个 edge chunk 计算并保存实际编码字节数，替换基于 weight 大小的 Edge 请求，并通过不足/等于/超过单块容量的边界测试
- [x] 2.2 修正输出顶点地址计算和各 HBM 区域布局，增加连续顶点步长、分块偏移、区域不重叠和越界测试
- [x] 2.3 为 Event 与缓冲状态加入事务计数、负计数、未完成事务和地址范围断言，并通过故障注入测试验证非零失败
- [x] 2.4 修正 systolic 字节单位、有效 MAC/ADD 与流水填充周期统计，并用手算小矩阵测试验证周期和操作数
- [x] 2.5 增加层级统计守恒检查，验证请求字节、SPM 写入、SPM 读取和输出写回在已定义边界内一致

## 3. 聚合引擎与图稀疏机制

- [x] 3.1 引入包含 interval、shard、目标顶点范围、唯一邻居和实际字节数的显式分区产物，并通过小图黄金结果测试
- [x] 3.2 实现可开关的 Window Sliding & Shrinking 和动态稀疏消除，验证关闭时读取完整 interval、开启时只读取唯一邻居
- [x] 3.3 将 Vertex-Disperse 调度扩展到论文配置的 32×SIMD16，并通过高维与低维特征测试验证任务分散、并行顶点和利用率
- [x] 3.4 区分 SUM 与 MAX 聚合的操作类型和周期统计，并通过 GCN SUM 与 GraphSAGE MAX 的小图测试
- [x] 3.5 重构 Aggregation Buffer 为有界阶段指针模型，验证分配、ready、consume、reclaim、环绕和容量阻塞行为

## 4. 组合引擎模块集群

- [x] 4.1 建立 8 模块×4 阵列×128 宽的组合引擎配置与模块级任务调度器，并通过资源数量和并发上限测试
- [x] 4.2 实现矩阵 tile、权重装载、输入推进、流水填充、有效计算和输出写回周期模型，并用多种非整除矩阵尺寸验证
- [x] 4.3 实现 independent 策略，验证模块处理不同输入批次、权重流量和完成周期符合手算案例
- [x] 4.4 实现 cooperative 策略与权重级联统计，验证同批输入按输出列拆分且组合结果周期正确
- [x] 4.5 增加组合策略对比集成测试，验证两种模式产生相同有效操作数和输出字节但允许不同周期与权重流量

## 5. 跨引擎流水与访存协调

- [x] 5.1 实现 sequential、latency-aware、energy-aware 三种 AE/CE 策略，并通过数据依赖和批次启动时机测试
- [x] 5.2 为内存事件加入 batch ID、请求类别、入队周期和等待统计，并验证结构化输出包含这些指标
- [x] 5.3 实现“最早 batch 优先、batch 内 Edge→Input→Weight→Output”的仲裁，并通过跨批次饥饿测试
- [x] 5.4 增加 HBM 接收阻塞、队列等待、有效带宽和 channel/bank 分布统计，并通过合成地址流测试计算结果
- [x] 5.5 使用 smoke 图覆盖三种流水与两种组合策略的六种组合，验证全部组合完成、无死锁且统计守恒

## 6. 论文基准与 ±20% 校准

- [x] 6.1 新增版本化论文参考指标清单，写入来源、参考值、单位、聚合方式和 20% 容差，并通过格式校验测试
- [x] 6.2 实现成对消融实验驱动，确保每组只改变目标机制，并验证生成稀疏、流水、协调器三类原始结果和比率
- [x] 6.3 实现论文指标校验器，按相对误差公式输出逐项通过/失败并在缺失、非有限或超限时返回非零状态
- [x] 6.4 使用 Cora/Citeseer 调整仅限设计文档允许的实现参数，验证六个强制相对指标均进入论文值 ±20%
- [x] 6.5 使用未参与调整的 PubMed 执行保留验证，若任一强制指标超限则回到模型修正而非添加全局缩放系数
- [x] 6.6 生成逐数据集和聚合验收报告，明确已复现与未复现的论文结论，并验证报告不宣称 CPU/GPU 绝对加速或完整能效复现

## 7. 集成验收

- [x] 7.1 执行干净 Release 构建与完整 `ctest`，确认编译无错误且所有默认测试通过
- [x] 7.2 执行 GCN+Cora/Citeseer/PubMed 完整论文配置消融套件，确认结果文件、运行清单和缓存均可重复使用
- [x] 7.3 执行论文 ±20% 校验目标，确认所有强制指标通过并保存最终汇总报告
- [x] 7.4 运行 `openspec validate reproduce-hygcn-paper-performance --strict`，确认规格、实现任务状态和变更关系全部有效

## 8. 独立审查整改

- [x] 8.1 修复 `simd.h` 缺少 `<cstdint>` 导致的干净 Release 构建失败，并从新构建目录验证
- [x] 8.2 删除协调效率、固定流水重叠率和经验 spill 系数，改用 channel/bank/row-buffer 请求时序与 batch 事件流水
- [x] 8.3 让稀疏邻居生成真实离散地址并仅合并连续顶点请求，保证流量统计与地址映射语义一致
- [x] 8.4 按 Aggregation Buffer ping-pong 半区生成分区，并让 ready、consume、reclaim 与容量 backpressure 进入时间线
- [x] 8.5 修正 independent 模式权重只从 HBM 装载一次、随后由 Weight Buffer 跨 batch/模块复用
- [x] 8.6 重建论文指标清单：范围逐数据集检查、正文平均值聚合检查、无可靠值指标降为诊断项
- [x] 8.7 增加协调器 row-conflict 单测并复跑干净构建、默认 CTest、完整三数据集成对消融和严格 OpenSpec 校验

## 9. Review v2 因果与验收整改

- [x] 9.1 按论文 Algorithm 4 把稀疏输入实现为保留内部空洞的连续窗口，并用 `{0,2,4,6} -> [0,7)` 黄金用例记录地址、跨度、空洞和事务数
- [x] 9.2 把内存请求展平为统一 block 事务流，并用 coalesced/fragmented 同地址流验证周期、row hit/miss、channel 和 bank 事务完全一致
- [x] 9.3 为 Output 和 intermediate 请求加入 producer-ready，验证 Output 不早于 CE、intermediate read 与 write 同地址且等待写完成，并删除重复 spill 计时
- [x] 9.4 增加第一层 AE-only scope，保证 Fig. 15 固定图、层和 AE 工作量，只切换连续窗口稀疏优化
- [x] 9.5 数字化 Fig. 15/16 逐数据集 SVG 柱值，保存来源 URL、SHA256 和坐标，并按逐柱相对误差执行 ±20% 门禁
- [x] 9.6 修正 Combination Module 单 batch 顶点组并行度，并验证跨 batch producer/RAW 依赖保持约束
- [x] 9.7 在 JSON 中记录派生驻留容量、producer 时间线和窗口证据，声明 `parameter_recalibration=false` 且论文配置文件未修改
- [x] 9.8 从整改提交执行干净 Release 构建、默认 CTest、legacy 回归、严格 OpenSpec 和强制三数据集 benchmark，保存正式复跑产物并推送远端

## 10. Review v3 工作负载、流量与协调器整改

- [x] 10.1 sequential intermediate 只按真实 producer bytes 的 block 对齐值读写，并验证严格 AE→Write→Read→CE 阶段
- [x] 10.2 将 priority 和 address mapping 拆成独立开关，把原隐藏 interleave 改为有 DRAMSim3 HBM dual-command 依据的显式配置，并保存 row-first/low-bits 实现依据
- [x] 10.3 建立 Edge completion 与 neighbor-index ready 到 Input enqueue 的动态依赖，覆盖跨 batch priority 冲突
- [x] 10.4 新增 Table 5 layer-0 workload manifest，确保 Fig. 15-17 不包含隐式 `128→num_class` 分类层
- [x] 10.5 生成 priority-only、mapping-only、combined 分解消融，并保存请求 timeline 与 channel/bank 分布
- [x] 10.6 将行为参数纳入配置和自动 baseline diff，禁止硬编码 `parameter_recalibration`
- [x] 10.7 保持 F-01 至 F-04、CTest、legacy 与现有因果回归通过，并完成三数据集强制 benchmark
