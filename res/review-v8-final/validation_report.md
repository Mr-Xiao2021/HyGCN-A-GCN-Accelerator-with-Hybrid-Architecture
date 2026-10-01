# HyGCN Paper Metric And Internal Check Validation

Tolerance: 20%

Digitized bars are checked against their dataset-specific references. Paper-reported averages are checked only after applying the declared aggregation rule. All three datasets are historically exposed calibrated-fit workloads; this report does not claim an independent hold-out. Internal checks and diagnostic-only metrics are not paper evidence.

## Paper Metrics (14 rows)

| Metric | Scope | Reference | Measured | Relative error | Status |
|---|---|---:|---:|---:|---|
| sparsity_speedup | cora | 1.082102 | 1.067903 | 1.31% | PASS |
| sparsity_speedup | citeseer | 2.883223 | 2.866658 | 0.57% | PASS |
| sparsity_speedup | pubmed | 1.115278 | 1.105366 | 0.89% | PASS |
| sparsity_ae_dram_ratio | cora | 0.879502 | 0.930626 | 5.81% | PASS |
| sparsity_ae_dram_ratio | citeseer | 0.339962 | 0.336942 | 0.89% | PASS |
| sparsity_ae_dram_ratio | pubmed | 0.896637 | 0.885985 | 1.19% | PASS |
| pipeline_speedup | cora | 2.125024 | 1.951856 | 8.15% | PASS |
| pipeline_speedup | citeseer | 1.842621 | 2.179591 | 18.29% | PASS |
| pipeline_speedup | pubmed | 1.366562 | 1.623951 | 18.83% | PASS |
| pipeline_dram_ratio | cora | 0.501466 | 0.594714 | 18.60% | PASS |
| pipeline_dram_ratio | citeseer | 0.535832 | 0.632559 | 18.05% | PASS |
| pipeline_dram_ratio | pubmed | 0.731763 | 0.786438 | 7.47% | PASS |
| coordination_speedup | aggregate | 3.700000 | 3.265968 | 11.73% | PASS |
| coordination_bandwidth_gain | aggregate | 4.000000 | 3.265968 | 18.35% | PASS |

## Internal Regression Checks (12 rows)

| Check | Scope | Threshold | Measured | Deviation | Status |
|---|---|---:|---:|---:|---|
| priority_incremental_speedup | cora | >=1.005000 | 1.066540 | 0.00% | PASS |
| priority_incremental_speedup | citeseer | >=1.005000 | 1.059054 | 0.00% | PASS |
| priority_incremental_speedup | pubmed | >=1.005000 | 1.062261 | 0.00% | PASS |
| priority_incremental_speedup_aggregate | aggregate | >=1.005000 | 1.062619 | 0.00% | PASS |
| priority_incremental_bandwidth_gain | cora | >=1.005000 | 1.066540 | 0.00% | PASS |
| priority_incremental_bandwidth_gain | citeseer | >=1.005000 | 1.059054 | 0.00% | PASS |
| priority_incremental_bandwidth_gain | pubmed | >=1.005000 | 1.062261 | 0.00% | PASS |
| priority_incremental_bandwidth_gain_aggregate | aggregate | >=1.005000 | 1.062619 | 0.00% | PASS |
| priority_incremental_row_hit_ratio | cora | >=1.030000 | 1.041981 | 0.00% | PASS |
| priority_incremental_row_hit_ratio | citeseer | >=1.030000 | 1.034890 | 0.00% | PASS |
| priority_incremental_row_hit_ratio | pubmed | >=1.030000 | 1.035427 | 0.00% | PASS |
| priority_incremental_row_hit_ratio_aggregate | aggregate | >=1.030000 | 1.037433 | 0.00% | PASS |

## Diagnostic-Only Metrics

| Metric | Dataset | Measured | Reason |
|---|---|---:|---|
| sparsity_input_dram_ratio | cora | 0.930453 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| sparsity_input_dram_ratio | citeseer | 0.336790 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| sparsity_input_dram_ratio | pubmed | 0.885651 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| priority_speedup | cora | 0.943810 | Internal causal decomposition of the paper coordinator |
| priority_speedup | citeseer | 0.996988 | Internal causal decomposition of the paper coordinator |
| priority_speedup | pubmed | 0.929280 | Internal causal decomposition of the paper coordinator |
| priority_bandwidth_gain | cora | 0.943810 | Internal causal decomposition of the paper coordinator |
| priority_bandwidth_gain | citeseer | 0.996988 | Internal causal decomposition of the paper coordinator |
| priority_bandwidth_gain | pubmed | 0.929280 | Internal causal decomposition of the paper coordinator |
| mapping_speedup | cora | 2.537623 | Internal causal decomposition of the paper coordinator |
| mapping_speedup | citeseer | 3.435110 | Internal causal decomposition of the paper coordinator |
| mapping_speedup | pubmed | 3.251045 | Internal causal decomposition of the paper coordinator |
| mapping_bandwidth_gain | cora | 2.537623 | Internal causal decomposition of the paper coordinator |
| mapping_bandwidth_gain | citeseer | 3.435110 | Internal causal decomposition of the paper coordinator |
| mapping_bandwidth_gain | pubmed | 3.251045 | Internal causal decomposition of the paper coordinator |
| coordination_active_bandwidth_gain | cora | 3.395400 | Diagnostic only because excluding producer-idle gaps can invert the end-to-end throughput conclusion |
| coordination_active_bandwidth_gain | citeseer | 3.863540 | Diagnostic only because excluding producer-idle gaps can invert the end-to-end throughput conclusion |
| coordination_active_bandwidth_gain | pubmed | 3.474807 | Diagnostic only because excluding producer-idle gaps can invert the end-to-end throughput conclusion |

## Scope

This is a request-level GCN mechanism check. It is not a cycle-accurate Ramulator reproduction and does not validate CPU/GPU speedup, DiffPool, area, or complete chip energy.
