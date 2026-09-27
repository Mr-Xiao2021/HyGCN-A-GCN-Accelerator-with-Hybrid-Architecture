# HyGCN Review v5 Closure Evidence

Date: 2026-09-27

Implementation commit:
`7ba361fa11896e470ccc4b0aa62c06313151924a`

Artifact directory: `res/review-v6/`

## Result

The clean Release reproduction passes all 14 Figure 15-17 paper metric rows and
all 12 internal causality checks. These counts are reported separately and are
not presented as 26 paper metrics.

All 21 forced benchmark manifests identify source commit `7ba361fa1189`, binary
digest `fb0868bb77ebc297`, and config digest `9db9fcd14e456d02`. The clean binary
SHA256 is recorded in `binary_sha256.txt`.

`parameter_recalibration` remains `true` because the versioned review-v3
baseline differs from the current scheduler, mapping, dependency, queue, and
workload behavior. The current result is a calibrated request-level fit, not an
independent hold-out validation and not a Ramulator/RTL reproduction.

## V5-01: Shared Transaction Admission

FIFO and batch-class now share the same global admission clock. Across all four
buffer ports, no more than four 64-byte blocks enter the channel transaction
queues in one model cycle. Each channel transaction queue is bounded at 32
entries and each bank command queue is bounded at 8 entries.

The queue capacities are parsed and checked against bundled DRAMSim3
`configs/HBM1_4Gb_x128.ini` (`trans_queue_size=32`, `cmd_queue_size=8`). The
four-block admission limit is derived from 256 bytes/cycle divided by 64 bytes.

- Test: `V5_01_transaction_admission_bandwidth=PASS`.
- Cora optimized: 715,266 blocks, 179,120 admission cycles, max 4 blocks/cycle,
  peak 256/256 total transaction queue entries.
- Citeseer optimized: 2,657,567 blocks, 669,606 admission cycles, max 4
  blocks/cycle, peak 256/256 entries.
- PubMed optimized: 4,646,886 blocks, 1,165,375 admission cycles, max 4
  blocks/cycle, peak 256/256 entries.
- Every request records enqueue, first/last admission, first issue, and
  completion cycles. Runtime checks and the unit test reject queue overflows or
  admission above the common width.

Evidence: `transaction_admission_evidence.json`, per-run
`transaction_admission_trace`, `producer_timeline.csv`, and `unit.log`.

## V5-02: Calibrated Fit, No Retrospective Hold-Out

Cora, Citeseer, and PubMed were all exposed to the 4/5/6 MiB scheduler-cap
sweep before this revision. The workload manifest and reports therefore label
all three datasets as calibrated-fit datasets and expose an empty independent
hold-out set.

| Capacity | All Figure 15-16 rows pass | Mean error | Role |
|---:|---|---:|---|
| 4 MiB | No | 11.61% | diagnostic candidate |
| 5 MiB | Yes | 8.28% | selected calibrated fit |
| 6 MiB | No | 11.28% | diagnostic candidate |

`independent_validation.available` is `false`; no
`used_for_selection=false` claim is emitted for PubMed.

Evidence: `partition_sensitivity.json`, `calibrated_fit.csv`,
`calibrated_fit_disclosure.json`, and `workload_manifest.json`.

## V5-03: External HBM Timing Basis

The required paper profile is derived from the bundled DRAMSim3 HBM timing:

- row hit: `ceil(CL * tCK / 1 ns) = ceil(7 * 2 / 1) = 14` cycles;
- closed-row miss: `ceil((tRCDRD + CL) * tCK / 1 ns) = 28` cycles;
- row conflict: `ceil((tRP + tRCDRD + CL) * tCK / 1 ns) = 42` cycles.

The benchmark fails if the effective profile differs from these derived values
or from the 32/8 queue capacities. Alternative timing profiles remain
diagnostic and are not selected using paper target errors.

| Scenario | Fig. 17 speed | Fig. 17 bandwidth | Priority increment | Row-hit ratio |
|---|---:|---:|---:|---:|
| required DRAMSim3 14/28/42 | 3.247379x | 3.247379x | 1.009242x | 1.058156x |
| fast 10/24/38 diagnostic | 2.626904x | 2.626904x | 1.004173x | 1.055834x |
| slow 18/36/54 diagnostic | 3.928014x | 3.928014x | 1.022716x | 1.058254x |
| legacy 12/28/42 diagnostic | 2.959055x | 2.959055x | 1.009211x | 1.058172x |
| row-first interleave 2 diagnostic | 5.769992x | 5.769992x | 1.009242x | 1.058156x |

The failing or materially different counterfactuals demonstrate sensitivity;
they do not replace the externally derived required profile.

Evidence: `model_sensitivity.json`, `model_sensitivity.csv`, profile snapshots,
and `benchmark_report.json` `dram_timing_basis`.

## V5-04: Paper Metrics Versus Internal Checks

The metric manifest, validator, benchmark report, and validation report all
carry an explicit `evidence_class` and fixed row counts:

- 14 paper metric rows: Figure 15 six rows, Figure 16 six rows, and Figure 17
  two aggregate rows.
- 12 internal checks: same-low-bits priority speed, complete-interval bandwidth,
  and row-hit improvement for three datasets plus aggregate checks.

The previous 0.05% aggregate threshold has been replaced. On the same low-bits
layout, each dataset must improve speed and bandwidth by at least 0.5%, each
row-hit ratio by at least 3%, and aggregate row-hit ratio by at least 5%.

| Dataset | Priority speed | Priority bandwidth | Row-hit ratio | Reorders |
|---|---:|---:|---:|---:|
| Cora | 1.008982x | 1.008982x | 1.055028x | 593 |
| Citeseer | 1.007252x | 1.007252x | 1.040775x | 4,488 |
| PubMed | 1.011493x | 1.011493x | 1.078664x | 3,508 |
| Mean | 1.009242x | 1.009242x | 1.058156x | - |

`priority_trace_evidence.json` includes concrete optimized/FIFO issue and
completion differences. `request_class_row_hits.csv` retains the class-level
row hit/miss cause.

## Paper Metric Table

| Metric | Scope | Paper target | Simulated | Error | Result |
|---|---|---:|---:|---:|---|
| Fig. 15 speedup | Cora | 1.082102 | 1.067695 | 1.33% | PASS |
| Fig. 15 speedup | Citeseer | 2.883223 | 2.894937 | 0.41% | PASS |
| Fig. 15 speedup | PubMed | 1.115278 | 1.125679 | 0.93% | PASS |
| Fig. 15 AE DRAM ratio | Cora | 0.879502 | 0.930626 | 5.81% | PASS |
| Fig. 15 AE DRAM ratio | Citeseer | 0.339962 | 0.336942 | 0.89% | PASS |
| Fig. 15 AE DRAM ratio | PubMed | 0.896637 | 0.885985 | 1.19% | PASS |
| Fig. 16 speedup | Cora | 2.125024 | 1.954017 | 8.05% | PASS |
| Fig. 16 speedup | Citeseer | 1.842621 | 2.162622 | 17.37% | PASS |
| Fig. 16 speedup | PubMed | 1.366562 | 1.629114 | 19.21% | PASS |
| Fig. 16 DRAM ratio | Cora | 0.501466 | 0.594714 | 18.60% | PASS |
| Fig. 16 DRAM ratio | Citeseer | 0.535832 | 0.632559 | 18.05% | PASS |
| Fig. 16 DRAM ratio | PubMed | 0.731763 | 0.786438 | 7.47% | PASS |
| Fig. 17 speedup | Aggregate | 3.700000 | 3.247379 | 12.23% | PASS |
| Fig. 17 bandwidth gain | Aggregate | 4.000000 | 3.247379 | 18.82% | PASS |

## Preserved Findings And Regressions

- `F01_window_sliding_shrinking=PASS`: `{0,2,4,6}` maps to `[0,7)`, 448 B,
  three internal holes, and seven transactions.
- `F02_fragmentation_invariance=PASS`: coalesced and fragmented 128-block flows
  both finish in 278 cycles with 64 hits and 64 misses.
- `F03_producer_and_RAW_dependencies=PASS`: Output and intermediate RAW ordering
  remain enforced.
- Figure 15 remains layer-0 AE-only; all 21 forced runs use layer 0 and output
  width 128.
- Required bandwidth uses cycle zero through final memory completion. The
  active-interval union remains diagnostic only, with the producer-idle
  counterexample retained.
- Clean Release CTest: 9/9 PASS.
- Legacy Cora/Citeseer: PASS.
- Strict OpenSpec validation: PASS.

## Retained Scope Boundaries

This remains a request-level GCN reproduction for Cora, Citeseer, and PubMed.
It does not claim Ramulator/RTL cycle accuracy, independent hold-out validation,
CPU/GPU absolute speedup, DiffPool, area, exact ping-pong half ownership, or
complete chip energy reproduction.
