# Review v10 Closure Report

Date: 2026-10-01

Implementation: `aeb6aea9942b207aa5bc5cdf5a9dc2bee3cc081d`

Base reviewed revision: `f8a31defa030c96b82bc1065f76734de4f86f5f5`

## Overall Conclusion

The Review v9 findings are closed as engineering and evidence issues. The
required paper profile now removes the unsupported FIFO active-window cap,
stores a reversible complete command stream, and audits all workload policy
and scheduler source changes. This correction also invalidates the previous
unqualified Fig. 17 acceptance: the cap-free required result is 12/14 paper
metrics and 8/12 schema-v6 internal checks. It is not accepted as a complete
Fig. 15-17 mechanism reproduction.

## V9-01: Unsupported FIFO Active-Window Cap

Status: CLOSED

- `coordinator_fifo_active_windows=0` is the required paper profile.
- FIFO and batch-class share the same 4-block/cycle admission bandwidth,
  directional transaction queues, and bank command queues.
- Finite window counts are diagnostic counterfactuals only.
- The full `0/1/2/4/8/512` scan shows that only window=4 passes Fig. 17:

| Windows | Role | Speedup | Bandwidth gain | Fig. 17 |
|---:|---|---:|---:|---|
| 0 | required cap-free | 1.127635x | 1.127635x | FAIL |
| 1 | diagnostic | 16.475479x | 16.475479x | FAIL |
| 2 | diagnostic | 8.289560x | 8.289560x | FAIL |
| 4 | diagnostic calibrated policy | 4.208020x | 4.208020x | PASS |
| 8 | diagnostic | 2.192490x | 2.192490x | FAIL |
| 512 | diagnostic queue-capacity proxy | 1.127635x | 1.127635x | FAIL |

This sensitivity proves that the former window=4 baseline restriction was
target-sensitive and cannot be used as required paper evidence.

Evidence: `fifo_window_sensitivity.json`, `fifo_window_sensitivity.csv`,
`metric_table.csv`, and `validation_report.md`.

## V9-02: Complete Independently Reconstructable Command Trace

Status: CLOSED

- Every PRE/ACT/READ/WRITE event records cycle, sequence, block offset,
  channel, bank, row, and command type in reversible chunked varint/base64.
- The standalone `hygcn_trace_validator` does not link simulator state. It
  reconstructs event/type totals, checksums, per-channel command-lane
  exclusivity, bank row state, tCCD/turnaround, and PRE/ACT recovery.
- Across 21 formal raw files it reconstructs 70,813,185 command events with 21
  unique checksums and zero command-lane violations.
- The same replay reconstructs 16,412,982 admission events and 64,675,995
  admitted blocks; 61,999,387 are reads and 2,676,608 are writes. The observed
  maximum is 4 blocks/cycle.
- Mutation tests reject 4/4 cases: deleted event, changed middle cycle, empty
  edge samples, and forged checksum.

Evidence: `complete_trace_evidence.json`, `complete_trace_validation.log`,
`trace_validator_mutations.log`, `raw_file_manifest.csv`, and `raw/*.json.gz`.

## V9-03: Complete Revision Audit

Status: CLOSED

- The audit covers the complete figures, graph-partition, and memory-ablation
  policy trees, all declared calibration keys, and full source diffs for
  `hygcn/paper_sim.cpp` and `hygcn/paper_sim.h`.
- The audit correctly reports `no_target_parameter_retuning=false` because the
  FIFO scheduler policy and implementation changed.
- It records 52 before/after metric deltas. Removing the unsupported cap moves
  aggregate Fig. 17 speedup and bandwidth from 4.208020x to 1.127635x, a
  relative delta of -73.20%.

Evidence: `revision_audit.json`, `before_after_metric_delta.csv`, and
`config_source_parameter_diff.patch`.

## Verification

- Clean Release configure/build: PASS
- CTest: 10/10 PASS
- Legacy GCN Cora/Citeseer: PASS
- Strict OpenSpec: PASS
- Paper metrics: 12/14 PASS
- Internal checks: schema v6 8/12 PASS; historical schema v5 7/12 PASS
- Partition sensitivity: 4 MiB FAIL / 5 MiB PASS / 6 MiB FAIL
- Model sensitivity: 10/10 scenarios completed
- Raw evidence: 21 JSON gzip round-trips and 21 CSV files verified

## Acceptance Boundary

The artifact closes Review v9's modeling-disclosure, trace-integrity, and audit
findings. It deliberately does not restore Fig. 17 by adding a baseline-only
throttle. Fig. 15 and Fig. 16 remain within the paper's +/-20% targets, while
the cap-free Fig. 17 result remains outside tolerance and the overall
request-level reproduction remains not accepted.
