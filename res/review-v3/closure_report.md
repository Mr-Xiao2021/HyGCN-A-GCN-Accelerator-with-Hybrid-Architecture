# HyGCN Review v2 Closure Evidence

Date: 2026-09-24

Implementation source commit:
`44ff038da0fb563e06141b880e135a7a0581cbf3`

Artifact directory: `res/review-v3/`

## Configuration And Recalibration

- `configs/HYGCN_PAPER.ini` SHA256:
  `6e3b29fc9f93c578c8c850fde98bc7a4d426b0666624ef52f2a6ed596cdf821d`
- The paper configuration file was not changed by the review-v2 implementation.
- `benchmark_report.json` records `parameter_recalibration: false`.
- All 15 raw benchmark manifests record source commit `44ff038da0fb`.

## F-01: Continuous Sparse Window

Test: `F01_window_sliding_shrinking=PASS`

Golden case `{0,2,4,6}` produces one continuous window `[0,7)`:

- address: `0`
- span: `448` bytes
- internal holes: `3`
- transactions: `7`

The complete paper-run window trace is indexed by `window_requests.csv`; each row
retains interval/window bounds, request address, bytes, transaction count, unique
vertices, and internal holes.

## F-02: Fragmentation Invariance

Test: `F02_fragmentation_invariance=PASS`

The same 128-block stream was executed as one coalesced request and as 128
single-block requests:

- coalesced completion: `271` cycles
- fragmented completion: `271` cycles
- row hits: `64`
- row misses: `64`
- channel and bank transaction vectors: identical

## F-03: Producer And RAW Causality

Test: `F03_producer_and_RAW_dependencies=PASS`

The unit counterexample records 10 Output requests and 10 intermediate read/write
pairs; the latest intermediate read completes at cycle `604`, exactly the sequential
CE start cycle.

The forced paper-run scan in `scope_invariants.txt` additionally verifies:

- 155 Output/intermediate timeline rows satisfy
  `enqueue >= producer_ready` and `first_issue >= enqueue`.
- 31 intermediate RAW pairs use identical addresses and byte ranges.
- every intermediate read producer-ready cycle equals its write completion cycle.
- intermediate traffic is represented only by the unified request timeline; no
  analytical spill delay is added again.

Every request is listed in `producer_timeline.csv` with producer-ready, enqueue,
first-issue, and completion cycles.

## F-04: AE-Only Scope And Dataset-Specific Acceptance

Tests:

- `paper_benchmark_scope=PASS`
- `scope_and_causality_invariants=PASS`
- `validate_paper_metrics.py=PASS`

All six Fig. 15 runs fix GCN layer 0 and AE-only scope. They contain Edge and Input
traffic only, with zero Weight, CE, Output, and intermediate activity. Optimized and
baseline runs differ only in the continuous-window sparsity switch.

| Metric | Dataset | Paper target | Simulated | Relative error | Result |
|---|---|---:|---:|---:|---|
| Fig. 15 AE speedup | Cora | 1.082102 | 1.138446 | 5.21% | PASS |
| Fig. 15 AE speedup | Citeseer | 2.883223 | 3.334554 | 15.65% | PASS |
| Fig. 15 AE speedup | PubMed | 1.115278 | 1.156091 | 3.66% | PASS |
| Fig. 15 AE DRAM ratio | Cora | 0.879502 | 0.869152 | 1.18% | PASS |
| Fig. 15 AE DRAM ratio | Citeseer | 0.339962 | 0.292966 | 13.82% | PASS |
| Fig. 15 AE DRAM ratio | PubMed | 0.896637 | 0.859107 | 4.19% | PASS |
| Fig. 16 speedup | Cora | 2.125024 | 2.062418 | 2.95% | PASS |
| Fig. 16 speedup | Citeseer | 1.842621 | 2.186294 | 18.65% | PASS |
| Fig. 16 speedup | PubMed | 1.366562 | 1.561088 | 14.23% | PASS |
| Fig. 16 DRAM ratio | Cora | 0.501466 | 0.580879 | 15.84% | PASS |
| Fig. 16 DRAM ratio | Citeseer | 0.535832 | 0.621853 | 16.05% | PASS |
| Fig. 16 DRAM ratio | PubMed | 0.731763 | 0.781589 | 6.81% | PASS |
| Fig. 17 coordination speedup | Aggregate | 3.700000 | 3.293600 | 10.98% | PASS |
| Fig. 17 bandwidth gain | Aggregate | 4.000000 | 3.293600 | 17.66% | PASS |

## Combination Module Constraint

`combination_parallelism_evidence` verifies that one legal batch with 64 rows uses
all 8 independent modules in one batch wave. F-03 producer and RAW tests ensure this
intra-batch parallelism does not relax cross-batch or memory data dependencies.

## Reproduction Commands And Results

- Clean Release configure/build: PASS (`clean_configure.log`, `clean_build.log`)
- Default CTest: 9/9 PASS (`ctest.log`)
- Named unit/counterexample suite: PASS (`unit.log`)
- Legacy Cora and Citeseer snapshots: PASS (`legacy_regression.log`)
- Strict OpenSpec validation: PASS (`openspec.log`)
- Forced 3-dataset benchmark: PASS (`benchmark.log`, `benchmark_report.json`)
- Dataset-specific metric validation: all 14 required rows PASS
  (`validation.log`, `validation_report.md`)

This remains a request-level GCN mechanism reproduction, not a Ramulator/RTL
cycle-accurate or end-to-end CPU/GPU reproduction.
