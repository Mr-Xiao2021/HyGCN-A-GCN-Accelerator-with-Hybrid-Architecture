# HyGCN Paper Metric Validation

Tolerance: 20%

Digitized bars are checked against their dataset-specific references. Paper-reported averages are checked only after applying the declared aggregation rule. Diagnostic-only metrics do not affect acceptance.

| Metric | Scope | Reference | Measured | Relative error | Status |
|---|---|---:|---:|---:|---|
| sparsity_speedup | cora | 1.082102 | 1.066987 | 1.40% | PASS |
| sparsity_speedup | citeseer | 2.883223 | 2.952885 | 2.42% | PASS |
| sparsity_speedup | pubmed | 1.115278 | 1.133714 | 1.65% | PASS |
| sparsity_ae_dram_ratio | cora | 0.879502 | 0.930626 | 5.81% | PASS |
| sparsity_ae_dram_ratio | citeseer | 0.339962 | 0.336942 | 0.89% | PASS |
| sparsity_ae_dram_ratio | pubmed | 0.896637 | 0.885985 | 1.19% | PASS |
| pipeline_speedup | cora | 2.125024 | 1.931166 | 9.12% | PASS |
| pipeline_speedup | citeseer | 1.842621 | 2.131055 | 15.65% | PASS |
| pipeline_speedup | pubmed | 1.366562 | 1.633195 | 19.51% | PASS |
| pipeline_dram_ratio | cora | 0.501466 | 0.594714 | 18.60% | PASS |
| pipeline_dram_ratio | citeseer | 0.535832 | 0.632559 | 18.05% | PASS |
| pipeline_dram_ratio | pubmed | 0.731763 | 0.786438 | 7.47% | PASS |
| coordination_speedup | aggregate | 3.700000 | 3.149923 | 14.87% | PASS |
| coordination_bandwidth_gain | aggregate | 4.000000 | 3.556360 | 11.09% | PASS |

## Diagnostic-Only Metrics

| Metric | Dataset | Measured | Reason |
|---|---|---:|---|
| sparsity_input_dram_ratio | cora | 0.930453 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| sparsity_input_dram_ratio | citeseer | 0.336790 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| sparsity_input_dram_ratio | pubmed | 0.885651 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| priority_speedup | cora | 2.188724 | Internal causal decomposition of the paper coordinator |
| priority_speedup | citeseer | 2.823583 | Internal causal decomposition of the paper coordinator |
| priority_speedup | pubmed | 3.435727 | Internal causal decomposition of the paper coordinator |
| priority_bandwidth_gain | cora | 3.204717 | Internal causal decomposition of the paper coordinator |
| priority_bandwidth_gain | citeseer | 3.498257 | Internal causal decomposition of the paper coordinator |
| priority_bandwidth_gain | pubmed | 3.469195 | Internal causal decomposition of the paper coordinator |
| mapping_speedup | cora | 2.782121 | Internal causal decomposition of the paper coordinator |
| mapping_speedup | citeseer | 3.223369 | Internal causal decomposition of the paper coordinator |
| mapping_speedup | pubmed | 3.489827 | Internal causal decomposition of the paper coordinator |
| mapping_bandwidth_gain | cora | 3.546371 | Internal causal decomposition of the paper coordinator |
| mapping_bandwidth_gain | citeseer | 3.519302 | Internal causal decomposition of the paper coordinator |
| mapping_bandwidth_gain | pubmed | 3.540819 | Internal causal decomposition of the paper coordinator |

## Scope

This is a request-level GCN mechanism check. It is not a cycle-accurate Ramulator reproduction and does not validate CPU/GPU speedup, DiffPool, area, or complete chip energy.
