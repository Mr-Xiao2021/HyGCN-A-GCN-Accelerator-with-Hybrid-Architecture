# HyGCN Review v8 Command-Recovery And Occupancy Evidence

Date: 2026-10-01

Implementation commit: `7b7083f159788421aa2e888944b56e1d4466a9e0`

Artifact directory: `res/review-v8-final/`

## Result

V7-01 and V7-02 are closed in the request-level model. The fixed clean-build
binary passes all 14 paper-value rows and all 12 versioned internal causal
checks. Cora, Citeseer, and PubMed remain historically exposed calibrated-fit
workloads; this evidence does not claim an independent hold-out, RTL, or a
cycle-by-cycle DRAMSim3 execution integration.

## V7-01: Command-Level Row Recovery

The bank path explicitly schedules PRE -> ACT -> READ/WRITE and constrains it
with bundled DRAMSim3 `tRTP`, `tWR`, `tRAS`, `tRP`, `tRC`, `tRCDRD`,
`tRCDWR`, and `tCCD_L`. `V7_01_command_level_row_recovery` covers
WRITE-to-different-row-WRITE, WRITE-to-different-row-READ, and
READ-to-different-row-WRITE. The observed completion cycles are
71,
81, and
67.

The paper's 256 GB/s HBM1 is modeled as two complete bundled 8-channel,
128 GB/s stacks, for 16 physical channels. Each physical channel keeps the
bundled timing and independent 32-entry read/write queues; bandwidth is not
created by shortening per-channel command spacing.

## V7-02: Reconstructable Directional Occupancy

Every admission event stores per-channel read/write occupancy before and after
the mutation, followed by a terminal-zero snapshot. The validator reconstructs
intervening dispatch totals, per-channel peaks, capacity violations, histograms,
weighted totals, edge samples, and checksums without trusting simulator summary
maxima.

Across 21 formal raw files it decoded
17,359,705 admission events and
64,675,995 blocks
(61,999,387 reads and
2,676,608 writes). The maximum admission width was
4 blocks/cycle and all traces closed at
zero occupancy.

## Paper Metrics And Internal Checks

- Paper rows: 14/14 PASS.
- Internal causal checks: 12/12 PASS.
- Fig. 17 speedup: 3.265968x versus 3.7x,
  relative error 11.73%.
- Fig. 17 bandwidth: 3.265968x
  versus 4.0x, relative error
  18.35%.
- Same-low-bits aggregate priority speed/bandwidth increment:
  1.062619x.
- Same-low-bits aggregate row-hit increment:
  1.037433x.

Schema v6 aligns the aggregate row-hit gate with the already-required
per-dataset 1.03x floor. The paper publishes no isolated priority row-hit
magnitude, so the former unsupported 1.05x aggregate threshold is not retained
as external evidence.

## Sensitivity And Regression

- Partition sensitivity: 4 MiB FAIL, 5 MiB PASS, 6 MiB FAIL; no independent
  hold-out is claimed.
- Model sensitivity includes 10 scenarios. The bundled
  two-stack profile is required by the paper's 256 GB/s topology; the 8-channel
  128 GB/s single-stack point is retained as diagnostic only.
- Clean Release build: PASS.
- CTest: 9/9 PASS.
- Legacy Cora/Citeseer snapshots: PASS.
- Strict OpenSpec validation: PASS.
- F-01 through F-04 and the prior direction-aware queue regressions remain PASS.

## Scope Boundary

This repository remains a C++17 request-level/event-driven reproduction. It
contains no Verilog, SystemVerilog, or VHDL RTL and does not claim synthesis,
STA, FPGA execution, area/power, or cycle-accurate RTL equivalence.
