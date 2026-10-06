# ResearchReport.pdf Final Figures

## Goal

This workflow reproduces the final normalized speed and energy comparison charts on page 9 of
`ResearchReport.pdf` while keeping source and measurement provenance explicit.

The source page contains 15 workloads: GCN, GIN, and GraphSAGE across Citeseer, Cora, DBLP,
PubMed, and Reddit. Figure 7 normalizes PyG-CPU latency to 1 and reports HyGCN speedup. Figure 8
normalizes HyGCN energy to 1 and reports PyG-CPU energy. The report text states arithmetic means
of 275x speedup and 4112x energy reduction.

## Reproduction Method

`configs/report_figure_targets.json` stores the PDF vector coordinates for each bar and the log-axis
tick coordinates. `tools/report_figures.py` fits each log10 axis, maps the bar tops back to numeric
values, and applies one uniform scale per figure so that the arithmetic mean agrees with the report
text. The required correction is about -0.4% for speed and -3.0% for energy, which is consistent
with the precision of the compact source graph.

The plotting path uses no plotting framework. SVG is generated with the Python standard library;
PNG and PDF are also produced when Pillow is available.

## Run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
python3 tools/report_figures.py \
  --binary build/hygcntest \
  --output-dir res/report
```

The equivalent CMake target is:

```bash
cmake --build build --target report_figures
```

Without `--binary`, the tool still reproduces the PDF charts and emits the digitization manifest,
but it marks all local measurements as not run.

## Local Simulator Evidence

The legacy simulator now writes total and per-layer DRAM energy to JSON. It can run GCN and GIN
on the four graph files present in the repository. For GraphSAGE, a checked deterministic
25-neighbor sampling fallback is used when `sample/<dataset>_sample.csv` is absent. This gives local
cycle and DRAM-energy evidence for 12 of the 15 chart points.

The generated CSV also computes report-calibrated PyG-CPU equivalents by multiplying local HyGCN
latency/DRAM energy by the digitized normalized ratios. Those equivalents are useful for chart
reconstruction only; they are not local CPU measurements.

An independent local CPU timing path is available through `tools/pyg_cpu_benchmark.py`. It runs
two-layer PyG GCN, GIN, and GraphSAGE inference, pins the process to 20 CPUs by default, excludes
graph and feature setup from the timed interval, and compares the median CPU wall-clock latency
with the committed 0.5 GHz HyGCN legacy-simulator latency. Its outputs are written to
`res/pyg-cpu/` and are separate from the PDF-digitized report reproduction.

## Limits

- Reddit graph files are absent, so its three local simulator points are unavailable.
- The repository still has no captured PyG-CPU benchmark from the report's dual Xeon 4210R system;
  local PyG results record the actual host CPU instead.
- The repository contains no RTL synthesis or CACTI output for compute/SRAM energy.
- Legacy energy is DRAMSim3 DRAM energy, not total accelerator energy.
- Therefore the charts are a high-fidelity report reproduction, not an independent confirmation of
  the 275x speedup or 4112x energy-reduction claims.

## Artifacts

- `report_comparison.pdf`, `report_comparison.png`, `report_comparison.svg`
- `report_speedup.png`, `report_speedup.svg`
- `report_energy.png`, `report_energy.svg`
- `report_metrics.csv`, `report_summary.json`, `REPRODUCTION.md`
- `raw/legacy_*.json`, `raw/legacy_*.csv`, `legacy_runs.log`
