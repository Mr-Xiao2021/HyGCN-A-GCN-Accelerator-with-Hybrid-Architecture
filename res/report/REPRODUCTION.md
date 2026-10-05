# ResearchReport.pdf Final Figure Reproduction

## Result

- Figure 7 speedup arithmetic mean: **275x**.
- Figure 8 energy-reduction arithmetic mean: **4112x**.
- Legacy simulator coverage: **12/15** workloads.
- The plotted normalized ratios are digitized from the PDF and uniformly anchored to the averages stated in its text.

## Workloads

| Workload | Speedup | Energy reduction | Local evidence |
|---|---:|---:|---|
| GCN-CS | 62.96x | 1291.53x | measured: legacy simulator |
| GCN-CR | 206.07x | 4204.27x | measured: legacy simulator |
| GCN-DBLP | 55.71x | 990.27x | measured: legacy simulator |
| GCN-PB | 115.89x | 2273.04x | measured: legacy simulator |
| GCN-RD | 165.16x | 2162.12x | unavailable: dataset files absent |
| GIN-CS | 119.80x | 2162.12x | measured: legacy simulator |
| GIN-CR | 281.05x | 5310.20x | measured: legacy simulator |
| GIN-DBLP | 210.76x | 3334.76x | measured: legacy simulator |
| GIN-PB | 174.67x | 3275.43x | measured: legacy simulator |
| GIN-RD | 1162.53x | 14881.88x | unavailable: dataset files absent |
| GS-CS | 203.86x | 2598.02x | measured: legacy simulator |
| GS-CR | 379.25x | 5483.31x | measured: legacy simulator |
| GS-DBLP | 290.55x | 3562.23x | measured: legacy simulator |
| GS-PB | 217.91x | 3334.76x | measured: legacy simulator |
| GS-RD | 478.82x | 6816.05x | unavailable: dataset files absent |

## Evidence Boundary

The local legacy simulator supplies HyGCN cycle counts and DRAMSim3 DRAM energy for Cora, Citeseer, DBLP, and PubMed across GCN, GIN, and GraphSAGE. GraphSAGE uses the checked deterministic 25-neighbor fallback when no sample file exists. Reddit is not simulated because its graph files are absent.

The repository does not include a local PyG-CPU benchmark capture, RTL synthesis results, or CACTI SRAM/compute energy. CPU latency and energy equivalents in the CSV are therefore report-calibrated reconstructions, and the local energy value covers DRAM only. These figures reproduce the report presentation; they are not an independent validation of the reported 275x/4112x claims.

## Artifacts

- `report_comparison.pdf`, `report_comparison.png`, and `report_comparison.svg`
- `report_speedup.png`/`.svg` and `report_energy.png`/`.svg`
- `report_metrics.csv` and `report_summary.json`
- `raw/` legacy JSON/CSV plus `legacy_runs.log` when a simulator binary is supplied
