# HyGCN Paper Metric Validation

Tolerance: 20%

Digitized bars are checked against their dataset-specific references. Paper-reported averages are checked only after applying the declared aggregation rule. Diagnostic-only metrics do not affect acceptance.

| Metric | Scope | Reference | Measured | Relative error | Status |
|---|---|---:|---:|---:|---|
| sparsity_speedup | cora | 1.082102 | 1.138446 | 5.21% | PASS |
| sparsity_speedup | citeseer | 2.883223 | 3.334554 | 15.65% | PASS |
| sparsity_speedup | pubmed | 1.115278 | 1.156091 | 3.66% | PASS |
| sparsity_ae_dram_ratio | cora | 0.879502 | 0.869152 | 1.18% | PASS |
| sparsity_ae_dram_ratio | citeseer | 0.339962 | 0.292966 | 13.82% | PASS |
| sparsity_ae_dram_ratio | pubmed | 0.896637 | 0.859107 | 4.19% | PASS |
| pipeline_speedup | cora | 2.125024 | 2.062418 | 2.95% | PASS |
| pipeline_speedup | citeseer | 1.842621 | 2.186294 | 18.65% | PASS |
| pipeline_speedup | pubmed | 1.366562 | 1.561088 | 14.23% | PASS |
| pipeline_dram_ratio | cora | 0.501466 | 0.580879 | 15.84% | PASS |
| pipeline_dram_ratio | citeseer | 0.535832 | 0.621853 | 16.05% | PASS |
| pipeline_dram_ratio | pubmed | 0.731763 | 0.781589 | 6.81% | PASS |
| coordination_speedup | aggregate | 3.700000 | 3.293600 | 10.98% | PASS |
| coordination_bandwidth_gain | aggregate | 4.000000 | 3.293600 | 17.66% | PASS |

## Diagnostic-Only Metrics

| Metric | Dataset | Measured | Reason |
|---|---|---:|---|
| sparsity_input_dram_ratio | cora | 0.868907 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| sparsity_input_dram_ratio | citeseer | 0.292831 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |
| sparsity_input_dram_ratio | pubmed | 0.858777 | Internal diagnostic; Figure 15(b) validates total Aggregation Engine DRAM access |

## Scope

This is a request-level GCN mechanism check. It is not a cycle-accurate Ramulator reproduction and does not validate CPU/GPU speedup, DiffPool, area, or complete chip energy.
