# MEGA 量化与 Adaptive-Package

## 量化 manifest

`mega/quantization.*` 实现版本化 manifest 加载、图/数据集/模型校验、来源分级和论文公式对应的 reference quantizer。`paper-derived` 或 `locally-trained` 只是 required benchmark 的来源前提；当前尚未接入精确量化 tensor payload，因此当前版本的端到端 MEGA 运行全部标为 diagnostic，`--required` 会拒绝执行。

当前仓库没有论文作者的逐节点位宽和 scale，也没有原始节点特征。因此先提供明确标记为 diagnostic 的 degree-quantile 生成器：

```bash
python3 tools/mega_quantization.py \
  --dataset cora \
  --model gcn \
  --output res/mega/manifests/cora-gcn.json
```

生成器计算输入文件 SHA256 和模拟器使用的 FNV64 digest；低、中、高度数区间分别使用 2/3/4 bit。该映射不参与论文数值验收。

本阶段尚未实现 PyG 本地训练/导出。`locally-trained` 标签不得手工写入 diagnostic manifest；
只有后续训练入口同时提供原始特征/权重产物、准确率摘要和精确图哈希后，才允许进入 required 表。

## Adaptive-Package

`mega/adaptive_package.*` 提供位精确 reference codec：

- Mode `00/01/10` 对应 64/128/192 bit，`11` 非法；
- 3 bit Bitwidth 以 `bitwidth - 1` 编码 1-8 bit；
- 同位宽连续节点共享 package，位宽变化强制关闭当前 package；
- 非零位置使用逐节点 bitmap；节点 value offset 作为保守 boundary stream 计费；
- traffic 分解为 payload、header、padding、bitmap、boundary、scale 和 DRAM alignment。

独立 decoder 不读取原始整数张量，必须从 package、bitmap 和 boundary stream 恢复全部节点。单元测试覆盖 exact golden bytes、三种 mode、全零节点、混合稀疏度、截断、非法 mode 和 bitmap mutation。
