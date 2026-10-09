# MEGA Local Mechanism Report

All rows use diagnostic quantization inputs. The ratios are local M0-M3 mechanism results, not a full-paper reproduction claim.

| Dataset | M3/M0 speedup | M3/M0 DRAM reduction | Transaction reduction | Gate |
|---|---:|---:|---:|---|
| cora | 641.531640x | 12.994182x | 12.994182x | PASS |
| citeseer | 795.355857x | 17.730935x | 17.730935x | PASS |
| pubmed | 353.722355x | 7.847424x | 7.847424x | PASS |

Arithmetic-mean speedup: 596.869950x
Arithmetic-mean DRAM reduction: 12.857514x
Conclusion: diagnostic-local-mechanism

Paper reference values are retained for comparison only because author per-node quantization artifacts and the full five-workload set are unavailable.
