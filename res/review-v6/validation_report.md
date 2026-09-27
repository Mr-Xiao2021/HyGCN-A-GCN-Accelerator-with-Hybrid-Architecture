# HyGCN Paper Metric And Internal Check Validation

Tolerance: 20%

Digitized bars are checked against their dataset-specific references. Paper-reported averages are checked only after applying the declared aggregation rule. All three datasets are historically exposed calibrated-fit workloads; this report does not claim an independent hold-out. Internal checks and diagnostic-only metrics are not paper evidence.

## Paper Metrics (14 rows)

| Metric | Scope | Reference | Measured | Relative error | Status |
|---|---|---:|---:|---:|---|
| sparsity_speedup | cora | 1.082102 | 1.067695 | 1.33% | PASS |
| sparsity_speedup | citeseer | 2.883223 | 2.894937 | 0.41% | PASS |
| sparsity_speedup | pubmed | 1.115278 | 1.125679 | 0.93% | PASS |
| sparsity_ae_dram_ratio | cora | 0.879502 | 0.930626 | 5.81% | PASS |
| sparsity_ae_dram_ratio | citeseer | 0.339962 | 0.336942 | 0.89% | PASS |
| sparsity_ae_dram_ratio | pubmed | 0.896637 | 0.885985 | 1.19% | PASS |
| pipeline_speedup | cora | 2.125024 | 1.954017 | 8.05% | PASS |
| pipeline_speedup | citeseer | 1.842621 | 2.162622 | 17.37% | PASS |
| pipeline_speedup | pubmed | 1.366562 | 1.629114 | 19.21% | PASS |
| pipeline_dram_ratio | cora | 0.501466 | 0.594714 | 18.60% | PASS |
| pipeline_dram_ratio | citeseer | 0.535832 | 0.632559 | 18.05% | PASS |
| pipeline_dram_ratio | pubmed | 0.731763 | 0.786438 | 7.47% | PASS |
| coordination_speedup | aggregate | 3.700000 | 3.247379 | 12.23% | PASS |
| coordination_bandwidth_gain | aggregate | 4.000000 | 3.247379 | 18.82% | PASS |

## Internal Regression Checks (12 rows)

| Check | Scope | Threshold | Measured | Deviation | Status |
|---|---|---:|---:|---:|---|
| priority_incremental_speedup | cora | >=1.005000 | 1.008982 | 0.00% | PASS |
| priority_incremental_speedup | citeseer | >=1.005000 | 1.007252 | 0.00% | PASS |
| priority_incremental_speedup | pubmed | >=1.005000 | 1.011493 | 0.00% | PASS |
| priority_incremental_speedup_aggregate | aggregate | >=1.005000 | 1.009242 | 0.00% | PASS |
| priority_incremental_bandwidth_gain | cora | >=1.005000 | 1.008982 | 0.00% | PASS |
| priority_incremental_bandwidth_gain | citeseer | >=1.005000 | 1.007252 | 0.00% | PASS |
| priority_incremental_bandwidth_gain | pubmed | >=1.005000 | 1.011493 | 0.00% | PASS |
| priority_incremental_bandwidth_gain_aggregate | aggregate | >=1.005000 | 1.009242 | 0.00% | PASS |
| priority_incremental_row_hit_ratio | cora | >=1.030000 | 1.055028 | 0.00% | PASS |
| priority_incremental_row_hit_ratio | citeseer | >=1.030000 | 1.040775 | 0.00% | PASS |
| priority_incremental_row_hit_ratio | pubmed | >=1.030000 | 1.078664 | 0.00% | PASS |
| priority_incremental_row_hit_ratio_aggregate | aggregate | >=1.050000 | 1.058156 | 0.00% | PASS |

## Diagnostic-Only Metrics

| Metric | Dataset | Measured | Reason |
|---|---|---:|---|
| sparsity_input_dram_ratio | cora | 0.930453 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| sparsity_input_dram_ratio | citeseer | 0.336790 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| sparsity_input_dram_ratio | pubmed | 0.885651 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| priority_speedup | cora | 1.299100 | Internal causal decomposition of the paper coordinator |
| priority_speedup | citeseer | 1.201233 | Internal causal decomposition of the paper coordinator |
| priority_speedup | pubmed | 1.264017 | Internal causal decomposition of the paper coordinator |
| priority_bandwidth_gain | cora | 1.299100 | Internal causal decomposition of the paper coordinator |
| priority_bandwidth_gain | citeseer | 1.201233 | Internal causal decomposition of the paper coordinator |
| priority_bandwidth_gain | pubmed | 1.264017 | Internal causal decomposition of the paper coordinator |
| mapping_speedup | cora | 2.800115 | Internal causal decomposition of the paper coordinator |
| mapping_speedup | citeseer | 3.331811 | Internal causal decomposition of the paper coordinator |
| mapping_speedup | pubmed | 3.520439 | Internal causal decomposition of the paper coordinator |
| mapping_bandwidth_gain | cora | 2.800115 | Internal causal decomposition of the paper coordinator |
| mapping_bandwidth_gain | citeseer | 3.331811 | Internal causal decomposition of the paper coordinator |
| mapping_bandwidth_gain | pubmed | 3.520439 | Internal causal decomposition of the paper coordinator |
| coordination_active_bandwidth_gain | cora | 3.565917 | Diagnostic only because excluding producer-idle gaps can invert the end-to-end throughput conclusion |
| coordination_active_bandwidth_gain | citeseer | 3.648842 | Diagnostic only because excluding producer-idle gaps can invert the end-to-end throughput conclusion |
| coordination_active_bandwidth_gain | pubmed | 3.610973 | Diagnostic only because excluding producer-idle gaps can invert the end-to-end throughput conclusion |

## Scope

This is a request-level GCN mechanism check. It is not a cycle-accurate Ramulator reproduction and does not validate CPU/GPU speedup, DiffPool, area, or complete chip energy.
