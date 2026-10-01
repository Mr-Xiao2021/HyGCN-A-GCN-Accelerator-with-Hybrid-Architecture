# HyGCN Paper Metric And Internal Check Validation

Tolerance: 20%

Digitized bars are checked against their dataset-specific references. Paper-reported averages are checked only after applying the declared aggregation rule. All three datasets are historically exposed calibrated-fit workloads; this report does not claim an independent hold-out. Internal checks and diagnostic-only metrics are not paper evidence.

## Paper Metrics (14 rows)

| Metric | Scope | Reference | Measured | Relative error | Status |
|---|---|---:|---:|---:|---|
| sparsity_speedup | cora | 1.082102 | 1.068574 | 1.25% | PASS |
| sparsity_speedup | citeseer | 2.883223 | 2.930315 | 1.63% | PASS |
| sparsity_speedup | pubmed | 1.115278 | 1.127159 | 1.07% | PASS |
| sparsity_ae_dram_ratio | cora | 0.879502 | 0.930626 | 5.81% | PASS |
| sparsity_ae_dram_ratio | citeseer | 0.339962 | 0.336942 | 0.89% | PASS |
| sparsity_ae_dram_ratio | pubmed | 0.896637 | 0.885985 | 1.19% | PASS |
| pipeline_speedup | cora | 2.125024 | 1.951892 | 8.15% | PASS |
| pipeline_speedup | citeseer | 1.842621 | 2.178640 | 18.24% | PASS |
| pipeline_speedup | pubmed | 1.366562 | 1.632294 | 19.45% | PASS |
| pipeline_dram_ratio | cora | 0.501466 | 0.594714 | 18.60% | PASS |
| pipeline_dram_ratio | citeseer | 0.535832 | 0.632559 | 18.05% | PASS |
| pipeline_dram_ratio | pubmed | 0.731763 | 0.786438 | 7.47% | PASS |
| coordination_speedup | aggregate | 3.700000 | 4.208020 | 13.73% | PASS |
| coordination_bandwidth_gain | aggregate | 4.000000 | 4.208020 | 5.20% | PASS |

## Internal Regression Checks (12 rows)

| Check | Scope | Threshold | Measured | Deviation | Status |
|---|---|---:|---:|---:|---|
| priority_incremental_speedup | cora | >=1.005000 | 1.017382 | 0.00% | PASS |
| priority_incremental_speedup | citeseer | >=1.005000 | 1.040944 | 0.00% | PASS |
| priority_incremental_speedup | pubmed | >=1.005000 | 1.055079 | 0.00% | PASS |
| priority_incremental_speedup_aggregate | aggregate | >=1.005000 | 1.037802 | 0.00% | PASS |
| priority_incremental_bandwidth_gain | cora | >=1.005000 | 1.017382 | 0.00% | PASS |
| priority_incremental_bandwidth_gain | citeseer | >=1.005000 | 1.040944 | 0.00% | PASS |
| priority_incremental_bandwidth_gain | pubmed | >=1.005000 | 1.055079 | 0.00% | PASS |
| priority_incremental_bandwidth_gain_aggregate | aggregate | >=1.005000 | 1.037802 | 0.00% | PASS |
| priority_incremental_row_hit_ratio | cora | >=1.030000 | 1.050710 | 0.00% | PASS |
| priority_incremental_row_hit_ratio | citeseer | >=1.030000 | 1.037131 | 0.00% | PASS |
| priority_incremental_row_hit_ratio | pubmed | >=1.030000 | 1.070309 | 0.00% | PASS |
| priority_incremental_row_hit_ratio_aggregate | aggregate | >=1.030000 | 1.052716 | 0.00% | PASS |

## Historical Schema v5 Comparison

- Current schema v6: 12/12 internal checks PASS.
- Historical schema v5: 12/12 internal checks PASS. It used the unsupported aggregate row-hit floor 1.05x; this is retained as an audit comparison, not as the current acceptance specification.

| Historical check | Threshold | Measured | Status |
|---|---:|---:|---|
| priority_incremental_row_hit_ratio_aggregate | >=1.050000 | 1.052716 | PASS |

## Diagnostic-Only Metrics

| Metric | Dataset | Measured | Reason |
|---|---|---:|---|
| sparsity_input_dram_ratio | cora | 0.930453 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| sparsity_input_dram_ratio | citeseer | 0.336790 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| sparsity_input_dram_ratio | pubmed | 0.885651 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| priority_speedup | cora | 3.459608 | Internal causal decomposition of the paper coordinator |
| priority_speedup | citeseer | 3.609305 | Internal causal decomposition of the paper coordinator |
| priority_speedup | pubmed | 4.135000 | Internal causal decomposition of the paper coordinator |
| priority_bandwidth_gain | cora | 3.459608 | Internal causal decomposition of the paper coordinator |
| priority_bandwidth_gain | citeseer | 3.609305 | Internal causal decomposition of the paper coordinator |
| priority_bandwidth_gain | pubmed | 4.135000 | Internal causal decomposition of the paper coordinator |
| mapping_speedup | cora | 3.555864 | Internal causal decomposition of the paper coordinator |
| mapping_speedup | citeseer | 4.162820 | Internal causal decomposition of the paper coordinator |
| mapping_speedup | pubmed | 4.429168 | Internal causal decomposition of the paper coordinator |
| mapping_bandwidth_gain | cora | 3.555864 | Internal causal decomposition of the paper coordinator |
| mapping_bandwidth_gain | citeseer | 4.162820 | Internal causal decomposition of the paper coordinator |
| mapping_bandwidth_gain | pubmed | 4.429168 | Internal causal decomposition of the paper coordinator |
| coordination_active_bandwidth_gain | cora | 4.738099 | Diagnostic only because excluding producer-idle gaps can invert the end-to-end throughput conclusion |
| coordination_active_bandwidth_gain | citeseer | 4.730502 | Diagnostic only because excluding producer-idle gaps can invert the end-to-end throughput conclusion |
| coordination_active_bandwidth_gain | pubmed | 4.731695 | Diagnostic only because excluding producer-idle gaps can invert the end-to-end throughput conclusion |

## Scope

This is a request-level GCN mechanism check. It is not a cycle-accurate Ramulator reproduction and does not validate CPU/GPU speedup, DiffPool, area, or complete chip energy.
