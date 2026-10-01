# HyGCN Review v6 Directional-Memory Evidence

Date: 2026-10-01

Implementation commit:
`0ab8c611d7294683aec2dc20f1f296d3c68acb29`

Artifact directory: `res/review-v7/`

## Result

V6-01 and V6-02 are implemented without tuning a parameter against the paper
targets in this revision. The fixed clean-build binary passes all 14 Figure
15-17 paper-value rows. The stricter direction-aware controller passes 7 of the
12 pre-existing internal threshold checks; all isolated priority effects remain
positive, but five previous magnitude thresholds no longer pass. This regression
is retained and reported instead of being hidden by parameter retuning.

This remains a calibrated request-level fit over Cora, Citeseer, and PubMed. It
is not an independent hold-out validation, a DRAMSim3 execution integration, or
an RTL implementation.

## V6-01: Direction-Aware HBM Queues And Timing

The request-level controller now follows the bundled DRAMSim3 configuration's
`unified_queue=False` organization:

- each of 8 channels has an independent 32-entry read queue;
- each channel has an independent 32-entry write buffer;
- each bank retains the externally sourced 8-entry command queue;
- all channels and request classes still share the global four-block-per-cycle
  admission limit;
- controller dispatch uses read priority plus bounded write draining, and bank
  future queues are split by direction.

The required timing is derived from `configs/HBM1_4Gb_x128.ini` at the model's
1 ns cycle:

| Timing | Derived cycles |
|---|---:|
| Read hit / miss / conflict | 14 / 28 / 42 |
| Write hit / miss / conflict | 4 / 18 / 32 |
| Read-to-write switch | 18 |
| Write-to-read switch | 16 |

`V6_01_directional_hbm_queues_and_timing` constructs a two-entry read queue
and a separate two-entry write buffer. The observed peaks are 2 reads and 1
write at the same time; the first read issues at cycle 0, the write at cycle 19,
and the direction constraint records 17 stall cycles. This rejects a merged
32-entry queue and rejects applying read timing to writes.

The three optimized formal runs contain 1, 8, and 7 Output requests whose
service intervals overlap read service for Cora, Citeseer, and PubMed. Read and
write behavior therefore enters the required execution interval rather than a
detached diagnostic. Directional occupancy, switches, stalls, overlap counts,
and per-run trace checksums are retained in
`directional_memory_evidence.json`.

## V6-02: Complete Admission Evidence

Every layer now stores the full admission event stream as reversible
`complete_delta_varint_base64_v1` chunks. Each decoded event contains cycle
delta, total/read/write admitted blocks, directional occupancy before and after
admission, and directional per-channel maxima.

`tools/paper_benchmark.py` does not trust the summary. It decodes every event
and independently recomputes:

- event count and first/last cycle;
- admitted read, write, and total blocks;
- histograms and weighted totals;
- aggregate and per-channel actual maxima;
- first/last edge samples;
- the FNV-1a trace checksum;
- request-byte coverage and enqueue/admission/issue causality.

`paper_output_determinism` exercises the same complete decoder in CTest. The
formal per-run recomputation is recorded in
`complete_trace_validation.log`; compact per-variant totals and checksums are in
`transaction_admission_evidence.json`. Lossless compressed raw JSON and the
matching CSV files are retained under `raw/`.

The independent formal pass decoded 21/21 raw JSON files, covered 64,675,995
admitted blocks (61,999,387 reads and 2,676,608 writes), observed a maximum of
four blocks in every admission cycle, and reproduced 21 unique per-layer trace
checksums.

## Paper Metrics

| Metric | Scope | Paper | Before v6 | After v7 | Error | Result |
|---|---|---:|---:|---:|---:|---|
| Fig. 15 speedup | Cora | 1.082102 | 1.067695 | 1.067695 | 1.33% | PASS |
| Fig. 15 speedup | Citeseer | 2.883223 | 2.894937 | 2.894937 | 0.41% | PASS |
| Fig. 15 speedup | PubMed | 1.115278 | 1.125679 | 1.125679 | 0.93% | PASS |
| Fig. 15 AE DRAM ratio | Cora | 0.879502 | 0.930626 | 0.930626 | 5.81% | PASS |
| Fig. 15 AE DRAM ratio | Citeseer | 0.339962 | 0.336942 | 0.336942 | 0.89% | PASS |
| Fig. 15 AE DRAM ratio | PubMed | 0.896637 | 0.885985 | 0.885985 | 1.19% | PASS |
| Fig. 16 speedup | Cora | 2.125024 | 1.954017 | 1.951496 | 8.17% | PASS |
| Fig. 16 speedup | Citeseer | 1.842621 | 2.162622 | 2.160007 | 17.22% | PASS |
| Fig. 16 speedup | PubMed | 1.366562 | 1.629114 | 1.627548 | 19.10% | PASS |
| Fig. 16 DRAM ratio | Cora | 0.501466 | 0.594714 | 0.594714 | 18.60% | PASS |
| Fig. 16 DRAM ratio | Citeseer | 0.535832 | 0.632559 | 0.632559 | 18.05% | PASS |
| Fig. 16 DRAM ratio | PubMed | 0.731763 | 0.786438 | 0.786438 | 7.47% | PASS |
| Fig. 17 speedup | Aggregate | 3.700000 | 3.247379 | 3.239663 | 12.44% | PASS |
| Fig. 17 bandwidth | Aggregate | 4.000000 | 3.247379 | 3.239663 | 19.01% | PASS |

The complete machine-readable comparison is
`before_after_14_metrics.csv`.

## Internal Checks

Direction-aware write service reduces the previous priority-effect magnitude.
The aggregate priority speed/bandwidth increment remains 1.006170x and the
aggregate row-hit ratio remains 1.030515x. The failed checks are PubMed
speed/bandwidth, Citeseer and PubMed row-hit thresholds, and the aggregate
row-hit threshold. No timing, queue, mapping, scheduler-cap, or workload
parameter was changed to restore those thresholds.

The exact 7/12 result and before/after values are retained in
`priority_incremental_table.csv`, `metric_table.csv`, and
`validation_report.md`.

## Sensitivity And Regression

- Partition sensitivity preserves the disclosed calibrated-fit result: 4 MiB
  FAIL, 5 MiB PASS, and 6 MiB FAIL. There is no independent hold-out.
- The required model point is the DRAMSim3-derived directional timing. Eight
  alternative request-release, read/write timing, direction-switch, and mapping
  scenarios remain diagnostic and are not selected from target error.
- Clean Release build: PASS.
- CTest: 9/9 PASS.
- Legacy Cora/Citeseer snapshots: PASS.
- Strict OpenSpec validation: PASS.
- `F01_window_sliding_shrinking`: PASS.
- `F02_fragmentation_invariance`: PASS.
- `F03_producer_and_RAW_dependencies`: PASS.
- V4 complete memory-service bandwidth scope: PASS.
- Sequential actual-byte traffic and Table 5 layer-0 scope are preserved.

## Scope Boundary

The repository implements a C++17 request-level/event-driven reproduction. It
contains no Verilog, SystemVerilog, or VHDL RTL, and it does not claim synthesis,
STA, FPGA execution, area/power, or cycle-accurate RTL equivalence.
