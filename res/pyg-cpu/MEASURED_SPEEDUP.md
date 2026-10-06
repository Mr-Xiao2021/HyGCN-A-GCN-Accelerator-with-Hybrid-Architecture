# Measured PyG-CPU Speedup

## Result

- Arithmetic mean speedup: **10.765x**.
- Geometric mean speedup: **6.367x**.
- Workloads measured: **12**.
- CPU: `Kunpeng-920`, 20 pinned threads.
- Framework: PyTorch `2.4.0`, PyG `2.6.1`.

## Workloads

| Workload | PyG CPU median | HyGCN | Measured speedup | PDF target |
|---|---:|---:|---:|---:|
| GCN-CS | 14.621 ms | 10.727 ms | 1.363x | 62.96x |
| GCN-CR | 8.494 ms | 2.962 ms | 2.868x | 206.07x |
| GCN-DBLP | 48.405 ms | 25.008 ms | 1.936x | 55.71x |
| GCN-PB | 31.160 ms | 10.638 ms | 2.929x | 115.89x |
| GIN-CS | 42.538 ms | 13.793 ms | 3.084x | 119.80x |
| GIN-CR | 21.486 ms | 3.747 ms | 5.734x | 281.05x |
| GIN-DBLP | 165.902 ms | 30.383 ms | 5.460x | 210.76x |
| GIN-PB | 73.739 ms | 13.513 ms | 5.457x | 174.67x |
| GS-CS | 178.374 ms | 8.622 ms | 20.688x | 203.86x |
| GS-CR | 70.219 ms | 2.515 ms | 27.925x | 379.25x |
| GS-DBLP | 484.051 ms | 21.437 ms | 22.580x | 290.55x |
| GS-PB | 258.119 ms | 8.853 ms | 29.155x | 217.91x |

## Method

Each workload uses a two-layer, hidden-size-128 PyG model in eager CPU inference mode. Input tensors and graph parsing are outside the timed region. GCN normalization is cached during warmup. GraphSAGE uses the same deterministic 25-neighbor sampling policy as the local HyGCN fallback. The reported CPU value is the median wall-clock latency after warmup; output reduction to a scalar is included so execution cannot be elided.

HyGCN latency is the committed legacy simulator cycle count divided by 0.5 GHz. The model uses random deterministic features and weights because this benchmark measures inference execution time, not trained-model accuracy.

## Boundary

This is a real local project measurement, but the host is a Kunpeng-920 system rather than the report's dual Intel Xeon 4210R platform. It establishes a reproducible PyG-CPU baseline and measured local speedup; it does not independently reproduce the report's CPU hardware environment or its Reddit results.
