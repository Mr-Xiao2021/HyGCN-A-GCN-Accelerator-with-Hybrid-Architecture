# HyGCN Review v4 Closure Evidence

Date: 2026-09-27

Implementation commit:
`3457f4016098cf1960a446539a71b926845effd8`

Artifact directory: `res/review-v5/`

## Result

The clean Release reproduction passes all 26 required rows. This includes the
14 Figure 15-17 paper comparisons and 12 optimized/mapping-only causality gates.
All 21 forced benchmark manifests identify source commit `3457f4016098` and
binary digest `26d3224e2c0284d2`.

`parameter_recalibration` is `true` because the versioned baseline differs from
the current scheduler, mapping, dependency, and queue behavior. No parameter was
retuned after the Review v4 mechanism changes: the required benchmark uses the
committed profile, sensitivity points remain diagnostic, and there is no global
scale factor.

## V4-01: Complete Memory-Service Bandwidth

Required bandwidth uses cycle zero through final request completion, including
producer-idle gaps. The in-flight interval union remains available only as
`active_bandwidth_utilization`.

- Test: `V4_01_memory_service_bandwidth_scope=PASS`.
- Immediate case: 66 service cycles, 66 active cycles, 0.090909 utilization.
- Delayed producer case: 144 service cycles, 54 active cycles.
- Complete-interval utilization falls to 0.041667 while active utilization
  rises to 0.111111. This is the required counterexample against using the
  active interval as the acceptance denominator.
- Figure 17 complete-interval bandwidth gain: `3.337766x` versus paper `4.0x`,
  relative error `16.56%`, PASS.
- Active-interval diagnostic: `3.724220x`; it does not affect acceptance.

Evidence: `bandwidth_scope_counterexample.json`, `validation_report.md`, and
the per-run `memory_service_cycles` / `memory_active_cycles` fields.

## V4-02: Priority And Mapping Causality

The coordinator now has four source ports, a finite 32-entry transaction queue
per channel from the DRAMSim3 HBM configuration, batch/class/row assembly, and
same-priority open-row continuation. The required row-first baseline directly
collapses `rorabgbachco` with interleave 1; unsupported adjacent-bank striping is
removed. Low-bit channel/bank mapping remains the paper-optimized layout.

Same-low-bits optimized versus mapping-only results:

| Dataset | Speed / bandwidth | Mapping row hit | Optimized row hit | Row-hit ratio |
|---|---:|---:|---:|---:|
| Cora | 1.001818x | 0.901634 | 0.964549 | 1.069779x |
| Citeseer | 1.000047x | 0.759597 | 0.911037 | 1.199370x |
| PubMed | 0.999986x | 0.900923 | 0.963891 | 1.069892x |
| Mean | 1.000617x | - | - | 1.113014x |

The PubMed difference is 16 cycles out of 1,178,821 and remains within the
declared 0.01% tail boundary. Every dataset improves row-hit rate. Required
gates are per-dataset speed/bandwidth `>=0.9999x`, aggregate speed/bandwidth
`>=1.0005x`, per-dataset row-hit ratio `>=1.0x`, and aggregate row-hit ratio
`>=1.01x`; all pass.

`priority_trace_evidence.json` records 599 / 3,932 / 4,088 priority reorders for
Cora / Citeseer / PubMed and concrete issue/completion differences against FIFO
on the same low-bits layout. `request_class_row_hits.csv` retains row hit/miss
counts by request class.

The row-first-fixed priority-only diagnostic remains `0.915890x`; it is retained
as a decomposition result and is not substituted for the required same-low-bits
causal comparison. Mapping-only is `3.335968x`; combined is `3.337766x`.

## V4-03: Calibration, Hold-Out, And Sensitivity

The scheduler shard-cap selection uses only Cora and Citeseer. Among 4/5/6 MiB,
only 5 MiB passes every required calibration row. PubMed is excluded from
selection and then passes every hold-out row at the selected 5 MiB value.

| Capacity | Calibration result | Mean calibration error | PubMed hold-out |
|---:|---|---:|---|
| 4 MiB | FAIL | 13.96% | diagnostic only |
| 5 MiB | PASS / selected | 9.25% | PASS |
| 6 MiB | FAIL | 12.98% | diagnostic only |

Unpublished parameter sensitivity is reported without selecting a new required
baseline:

| Scenario | Figure 17 speed / bandwidth | Priority increment | Row-hit ratio |
|---|---:|---:|---:|
| committed baseline | 3.337766x | 1.000617x | 1.113014x |
| neighbor delay 0 | 3.331343x | 1.000600x | 1.060213x |
| neighbor delay 4 | 3.334080x | 1.000613x | 1.113017x |
| DRAM 10/24 cycles | 2.878582x | 1.000609x | 1.060469x |
| DRAM 14/32 cycles | 3.777022x | 1.001265x | 1.061424x |
| row-first interleave 2 | 2.690162x | 1.000617x | 1.113014x |

The interleave-2 result is diagnostic evidence that the unsupported layout
constant materially changes the target metric; it is not a required baseline.

## P2: Path And Text Hygiene

- `paper_benchmark_scope` checks repository-local and external absolute paths.
- `external_path_evidence.txt` confirms `/tmp/hygcn-review-v5/hygcntest`
  remains printable without `Path.relative_to(root)` failure.
- Formal evidence is generated with LF line endings and checked for CRLF,
  trailing whitespace, and missing terminal newlines before commit.

## Preserved Findings And Regressions

- `F01_window_sliding_shrinking=PASS`: `{0,2,4,6}` maps to `[0,7)`, 448 B,
  three internal holes, and seven transactions.
- `F02_fragmentation_invariance=PASS`: coalesced and fragmented 128-block flows
  both finish in 271 cycles with 64 hits and 64 misses.
- `F03_producer_and_RAW_dependencies=PASS`: Output and intermediate RAW ordering
  remain enforced.
- `paper_benchmark_scope=PASS`: Figure 15 remains layer-0 AE-only.
- Producer scan: 73,766 dependent requests, zero causality violations.
- Workload scan: 21/21 raw runs are layer 0 with output width 128.
- Clean Release CTest: 9/9 PASS.
- Legacy Cora/Citeseer: PASS.
- Strict OpenSpec validation: PASS.

## Required Paper Metric Table

| Metric | Scope | Paper target | Simulated | Error | Result |
|---|---|---:|---:|---:|---|
| Fig. 15 speedup | Cora | 1.082102 | 1.068491 | 1.26% | PASS |
| Fig. 15 speedup | Citeseer | 2.883223 | 2.956218 | 2.53% | PASS |
| Fig. 15 speedup | PubMed | 1.115278 | 1.127283 | 1.08% | PASS |
| Fig. 15 AE DRAM ratio | Cora | 0.879502 | 0.930626 | 5.81% | PASS |
| Fig. 15 AE DRAM ratio | Citeseer | 0.339962 | 0.336942 | 0.89% | PASS |
| Fig. 15 AE DRAM ratio | PubMed | 0.896637 | 0.885985 | 1.19% | PASS |
| Fig. 16 speedup | Cora | 2.125024 | 1.950674 | 8.20% | PASS |
| Fig. 16 speedup | Citeseer | 1.842621 | 2.186107 | 18.64% | PASS |
| Fig. 16 speedup | PubMed | 1.366562 | 1.633584 | 19.54% | PASS |
| Fig. 16 DRAM ratio | Cora | 0.501466 | 0.594714 | 18.60% | PASS |
| Fig. 16 DRAM ratio | Citeseer | 0.535832 | 0.632559 | 18.05% | PASS |
| Fig. 16 DRAM ratio | PubMed | 0.731763 | 0.786438 | 7.47% | PASS |
| Fig. 17 speedup | Aggregate | 3.700000 | 3.337766 | 9.79% | PASS |
| Fig. 17 bandwidth gain | Aggregate | 4.000000 | 3.337766 | 16.56% | PASS |

## Retained Scope Boundaries

This remains a request-level GCN reproduction for Cora, Citeseer, and PubMed.
It is not a Ramulator/RTL reproduction and does not claim CPU/GPU absolute
speedup, DiffPool, area, exact ping-pong half ownership, or complete chip energy.
