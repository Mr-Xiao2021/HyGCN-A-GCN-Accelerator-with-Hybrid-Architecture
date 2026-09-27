# HyGCN Paper Metric Validation

Tolerance: 20%

Digitized bars are checked against their dataset-specific references. Paper-reported averages are checked only after applying the declared aggregation rule. Diagnostic-only metrics do not affect acceptance.

| Metric | Scope | Reference | Measured | Relative error | Status |
|---|---|---:|---:|---:|---|
| sparsity_speedup | cora | 1.082102 | 1.068491 | 1.26% | PASS |
| sparsity_speedup | citeseer | 2.883223 | 2.956218 | 2.53% | PASS |
| sparsity_speedup | pubmed | 1.115278 | 1.127283 | 1.08% | PASS |
| sparsity_ae_dram_ratio | cora | 0.879502 | 0.930626 | 5.81% | PASS |
| sparsity_ae_dram_ratio | citeseer | 0.339962 | 0.336942 | 0.89% | PASS |
| sparsity_ae_dram_ratio | pubmed | 0.896637 | 0.885985 | 1.19% | PASS |
| pipeline_speedup | cora | 2.125024 | 1.950674 | 8.20% | PASS |
| pipeline_speedup | citeseer | 1.842621 | 2.186107 | 18.64% | PASS |
| pipeline_speedup | pubmed | 1.366562 | 1.633584 | 19.54% | PASS |
| pipeline_dram_ratio | cora | 0.501466 | 0.594714 | 18.60% | PASS |
| pipeline_dram_ratio | citeseer | 0.535832 | 0.632559 | 18.05% | PASS |
| pipeline_dram_ratio | pubmed | 0.731763 | 0.786438 | 7.47% | PASS |
| priority_incremental_speedup | cora | >=0.999900 | 1.001818 | 0.00% | PASS |
| priority_incremental_speedup | citeseer | >=0.999900 | 1.000047 | 0.00% | PASS |
| priority_incremental_speedup | pubmed | >=0.999900 | 0.999986 | 0.00% | PASS |
| priority_incremental_speedup_aggregate | aggregate | >=1.000500 | 1.000617 | 0.00% | PASS |
| priority_incremental_bandwidth_gain | cora | >=0.999900 | 1.001818 | 0.00% | PASS |
| priority_incremental_bandwidth_gain | citeseer | >=0.999900 | 1.000047 | 0.00% | PASS |
| priority_incremental_bandwidth_gain | pubmed | >=0.999900 | 0.999986 | 0.00% | PASS |
| priority_incremental_bandwidth_gain_aggregate | aggregate | >=1.000500 | 1.000617 | 0.00% | PASS |
| priority_incremental_row_hit_ratio | cora | >=1.000000 | 1.069779 | 0.00% | PASS |
| priority_incremental_row_hit_ratio | citeseer | >=1.000000 | 1.199370 | 0.00% | PASS |
| priority_incremental_row_hit_ratio | pubmed | >=1.000000 | 1.069892 | 0.00% | PASS |
| priority_incremental_row_hit_ratio_aggregate | aggregate | >=1.010000 | 1.113014 | 0.00% | PASS |
| coordination_speedup | aggregate | 3.700000 | 3.337766 | 9.79% | PASS |
| coordination_bandwidth_gain | aggregate | 4.000000 | 3.337766 | 16.56% | PASS |

## Diagnostic-Only Metrics

| Metric | Dataset | Measured | Reason |
|---|---|---:|---|
| sparsity_input_dram_ratio | cora | 0.930453 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| sparsity_input_dram_ratio | citeseer | 0.336790 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| sparsity_input_dram_ratio | pubmed | 0.885651 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| priority_speedup | cora | 0.973310 | Internal causal decomposition of the paper coordinator |
| priority_speedup | citeseer | 0.866360 | Internal causal decomposition of the paper coordinator |
| priority_speedup | pubmed | 0.908001 | Internal causal decomposition of the paper coordinator |
| priority_bandwidth_gain | cora | 0.973310 | Internal causal decomposition of the paper coordinator |
| priority_bandwidth_gain | citeseer | 0.866360 | Internal causal decomposition of the paper coordinator |
| priority_bandwidth_gain | pubmed | 0.908001 | Internal causal decomposition of the paper coordinator |
| mapping_speedup | cora | 2.906574 | Internal causal decomposition of the paper coordinator |
| mapping_speedup | citeseer | 3.455673 | Internal causal decomposition of the paper coordinator |
| mapping_speedup | pubmed | 3.645656 | Internal causal decomposition of the paper coordinator |
| mapping_bandwidth_gain | cora | 2.906574 | Internal causal decomposition of the paper coordinator |
| mapping_bandwidth_gain | citeseer | 3.455673 | Internal causal decomposition of the paper coordinator |
| mapping_bandwidth_gain | pubmed | 3.645656 | Internal causal decomposition of the paper coordinator |
| coordination_active_bandwidth_gain | cora | 3.703466 | Diagnostic only because excluding producer-idle gaps can invert the end-to-end throughput conclusion |
| coordination_active_bandwidth_gain | citeseer | 3.770033 | Diagnostic only because excluding producer-idle gaps can invert the end-to-end throughput conclusion |
| coordination_active_bandwidth_gain | pubmed | 3.699162 | Diagnostic only because excluding producer-idle gaps can invert the end-to-end throughput conclusion |

## Scope

This is a request-level GCN mechanism check. It is not a cycle-accurate Ramulator reproduction and does not validate CPU/GPU speedup, DiffPool, area, or complete chip energy.
