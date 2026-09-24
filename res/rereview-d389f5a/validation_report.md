# HyGCN Paper Metric Validation

Tolerance: 20%

Range metrics are checked per dataset. Paper-reported averages are checked only after applying the declared aggregation rule. Diagnostic-only metrics do not affect acceptance.

| Metric | Scope | Reference | Measured | Relative error | Status |
|---|---|---:|---:|---:|---|
| sparsity_speedup | cora | 1.100000-3.000000 | 1.055462 | 4.05% | PASS |
| sparsity_speedup | citeseer | 1.100000-3.000000 | 2.083155 | 0.00% | PASS |
| sparsity_speedup | pubmed | 1.100000-3.000000 | 1.741607 | 0.00% | PASS |
| pipeline_speedup | cora | 1.369863-2.127660 | 1.474686 | 0.00% | PASS |
| pipeline_speedup | citeseer | 1.369863-2.127660 | 1.654481 | 0.00% | PASS |
| pipeline_speedup | pubmed | 1.369863-2.127660 | 1.694886 | 0.00% | PASS |
| pipeline_dram_ratio | cora | 0.500000-0.730000 | 0.487340 | 2.53% | PASS |
| pipeline_dram_ratio | citeseer | 0.500000-0.730000 | 0.569497 | 0.00% | PASS |
| pipeline_dram_ratio | pubmed | 0.500000-0.730000 | 0.577204 | 0.00% | PASS |
| coordination_speedup | aggregate | 3.700000 | 3.262173 | 11.83% | PASS |
| coordination_bandwidth_gain | aggregate | 4.000000 | 4.090036 | 2.25% | PASS |

## Diagnostic-Only Metrics

| Metric | Dataset | Measured | Reason |
|---|---|---:|---|
| sparsity_input_dram_ratio | cora | 0.913377 | HyGCN HPCA 2020 Figure 15(b); exact bar values require traceable figure digitization |
| sparsity_input_dram_ratio | citeseer | 0.439998 | HyGCN HPCA 2020 Figure 15(b); exact bar values require traceable figure digitization |
| sparsity_input_dram_ratio | pubmed | 0.559734 | HyGCN HPCA 2020 Figure 15(b); exact bar values require traceable figure digitization |

## Scope

This is a request-level GCN mechanism check. It is not a cycle-accurate Ramulator reproduction and does not validate CPU/GPU speedup, DiffPool, area, or complete chip energy.
