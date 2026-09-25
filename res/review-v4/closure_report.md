# HyGCN Review v3 Closure Evidence

Date: 2026-09-25

Implementation commit:
`738083279143c7b9fa80724e086e2faab30a2232`

Artifact directory: `res/review-v4/`

## Result

The clean Release reproduction passes all 14 required Figure 15-17 rows within
the declared 20% tolerance. All 21 raw benchmark JSON manifests identify source
commit `738083279143` and binary digest `48b2ce370cac9dc7`.

`parameter_recalibration` is `true`. The 5 MiB graph-partition scheduler cap is
not presented as a paper structural parameter: the 4/5/6 MiB sensitivity table
shows that only 5 MiB passes every required Figure 15/16 row. This is a disclosed
calibration point, not a robustness interval.

## R3-01: Priority And Mapping Decomposition

Tests and checks:

- `TestCoordinator` / `priority_trace_evidence`: Edge 0 completes at cycle 32,
  dependent Input 0 enqueues at 32, issues at 32 with batch-class priority, and
  issues at 136 with FIFO.
- `TestHbmLayoutAndMapping`: row-first and low-bits address distributions are
  independently asserted.
- `paper_benchmark_scope`: priority-only fixes row-first mapping; mapping-only
  fixes FIFO priority; combined changes both.
- `priority_trace_evidence.json`: Cora/Citeseer/PubMed record 40/314/312
  observable request reorders and issue/completion counterexamples.
- `mapping_distribution_evidence.json`: each dataset retains baseline and
  optimized channel/bank vectors.

The row-first two-lane value is now explicit in configuration and reports. Its
provenance is the review-v3 comparison baseline plus a request-level abstraction
of DRAMSim3 HBM dual-command width. The report explicitly does not claim that
DRAMSim3 pairs adjacent banks or that this value is published by the HyGCN paper.

Diagnostic decomposition averages:

| Ablation | Speedup | Bandwidth gain |
|---|---:|---:|
| priority-only | 2.816011x | 3.390723x |
| mapping-only | 3.165105x | 3.535497x |
| combined | 3.149923x | 3.556360x |

## R3-02: Exact Sequential Intermediate Traffic

Tests and checks:

- `TestProducerDependencies`: each intermediate read uses its write's address,
  bytes, and producer completion; CE starts after the final read.
- `validate_sequential_traffic`: each dataset requires read bytes and write bytes
  to equal block-aligned producer bytes.
- `sequential_traffic.json`: Cora 15,598,080 B; Citeseer 49,399,296 B; PubMed
  40,380,416 B in each direction.

No unused 4 MiB slot capacity is charged as DRAM traffic.

## R3-03: Versioned Workload Shape

Tests and checks:

- `paper_benchmark_scope` validates every Figure 15-17 variant uses selected
  layer 0 and output width 128.
- `workload_manifest.json` versions the Table 5 mapping as
  dataset feature width to 128.
- `scope_invariants.json` confirms all 21 raw runs contain only layer 0 and all
  output feature widths equal 128.

The implicit `128 -> num_class` classification layer is excluded from the
required benchmark.

## R3-04: Dynamic Edge To Input Dependency

Tests and checks:

- `TestCoordinator` constructs a cross-batch conflict and proves Input 0 is
  released only after Edge 0 completion, then advances ahead of queued batch 1
  work under batch-class priority.
- Every Input request stores its Edge `producer_sequence` plus the configured
  neighbor-index ready delay.
- `producer_timeline.csv` contains Input, Output, and intermediate producer
  timelines from all forced runs.
- The formal scan covers 73,640 producer-dependent requests with zero
  `enqueue < producer_ready` or `issue < enqueue` violations.

## R3-05: Auditable Parameters

Tests and checks:

- `parameter_diff.json` is calculated from the versioned review-v3 baseline and
  current effective behavior; it is not a hard-coded declaration.
- `config_source_parameter_diff.patch` records the configuration and source
  parameter changes from `9aa127d` to the implementation commit.
- `partition_sensitivity.json` and `partition_sensitivity.csv` retain all
  4/5/6 MiB results, source commit, binary digest, targets, values, errors, and
  pass/fail decisions.

The audit records changes to scheduler capacity, layer selection, Edge/Input
dependency, neighbor-index delay, and sequential spill alignment, and therefore
sets `parameter_recalibration: true`.

## Preserved F-01 To F-04

- `F01_window_sliding_shrinking=PASS`: `{0,2,4,6}` maps to `[0,7)`, 448 B,
  three internal holes, and seven transactions. Full traces are in
  `window_requests.csv`.
- `F02_fragmentation_invariance=PASS`: coalesced and fragmented 128-block flows
  both complete in 271 cycles with 64 hits, 64 misses, identical distributions,
  and identical active service cycles.
- `F03_producer_and_RAW_dependencies=PASS`: Output and intermediate producer/RAW
  ordering remain enforced.
- `paper_benchmark_scope=PASS`: Figure 15 remains layer-0 AE-only and changes
  only sparsity.

## Required Metric Table

| Metric | Scope | Paper target | Simulated | Error | Result |
|---|---|---:|---:|---:|---|
| Fig. 15 speedup | Cora | 1.082102 | 1.066987 | 1.40% | PASS |
| Fig. 15 speedup | Citeseer | 2.883223 | 2.952885 | 2.42% | PASS |
| Fig. 15 speedup | PubMed | 1.115278 | 1.133714 | 1.65% | PASS |
| Fig. 15 AE DRAM ratio | Cora | 0.879502 | 0.930626 | 5.81% | PASS |
| Fig. 15 AE DRAM ratio | Citeseer | 0.339962 | 0.336942 | 0.89% | PASS |
| Fig. 15 AE DRAM ratio | PubMed | 0.896637 | 0.885985 | 1.19% | PASS |
| Fig. 16 speedup | Cora | 2.125024 | 1.931166 | 9.12% | PASS |
| Fig. 16 speedup | Citeseer | 1.842621 | 2.131055 | 15.65% | PASS |
| Fig. 16 speedup | PubMed | 1.366562 | 1.633195 | 19.51% | PASS |
| Fig. 16 DRAM ratio | Cora | 0.501466 | 0.594714 | 18.60% | PASS |
| Fig. 16 DRAM ratio | Citeseer | 0.535832 | 0.632559 | 18.05% | PASS |
| Fig. 16 DRAM ratio | PubMed | 0.731763 | 0.786438 | 7.47% | PASS |
| Fig. 17 speedup | Aggregate | 3.700000 | 3.149923 | 14.87% | PASS |
| Fig. 17 bandwidth gain | Aggregate | 4.000000 | 3.556360 | 11.09% | PASS |

## Reproduction Status

- Clean Release configure/build: PASS
- CTest: 9/9 PASS
- Named unit and causal counterexamples: PASS
- Legacy Cora/Citeseer snapshots: PASS
- Strict OpenSpec validation: PASS
- Forced three-dataset benchmark: 14/14 required rows PASS
- 4/5/6 MiB sensitivity: completed and provenance-bound

## Retained Boundaries R3-06 To R3-08

- R3-06: Aggregation Buffer timing still uses a total-capacity release queue;
  exact ping-pong half ownership is not modeled.
- R3-07: reference hashes and digitization metadata are versioned, but the
  validator does not redownload and redigitize the SVG automatically.
- R3-08: this remains a request-level GCN reproduction for Cora, Citeseer, and
  PubMed, not a Ramulator/RTL, CPU/GPU, DiffPool, area, or full-chip energy
  reproduction.
