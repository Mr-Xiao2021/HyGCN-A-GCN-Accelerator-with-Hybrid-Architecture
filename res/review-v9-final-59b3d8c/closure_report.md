# HyGCN Review v9 Command-Lane And Audit Evidence

Date: 2026-10-01

Implementation commit: `59b3d8c91118547e435a42cb070c3d996ad15791`

Artifact directory: `res/review-v9-final-59b3d8c/`

## Result

V8-01, V8-02, and V8-03 are closed in the request-level model. The fixed
clean-build binary passes all 14 paper-value rows, all 12 schema-v6 internal
causal checks, and all 12 historical schema-v5 checks. Cora, Citeseer, and
PubMed remain historically exposed calibrated-fit workloads; this evidence
does not claim an independent hold-out, RTL, or cycle-by-cycle DRAMSim3
execution integration.

## V8-01: Exclusive Channel Command Lane

Bank requests advance through PRE, ACT, and DATA phases. All banks in a channel
share one command lane. READ/WRITE completion latency no longer gates the next
data command: the same-row READ counterexample issues at cycles 14 and 18,
matching `tCCD=4`, rather than the rejected 14 and 32 schedule. The cross-bank
counterexample issues READ at cycle 34 and PRE at cycle 36, so row and data
commands cannot occupy the same channel lane in one cycle.

A monotonic simulation clock covers admission, controller dispatch, and command
issue events. The queue-backpressure counterexample records 8 delayed events
without dispatching or issuing in the past. Formal command summaries report zero
lane violations and retain event counts, checksums, and first/last samples.

## V8-02: Versioned Gate Comparison

- Paper rows: 14/14 PASS.
- Internal schema v6: 12/12 PASS.
- Historical schema v5: 12/12 PASS.
- Aggregate row-hit increment: 1.052716x; it now also exceeds the
  old 1.05x threshold through mechanism changes, not a gate relaxation.
- Fig. 17 speedup: 4.208020x versus 3.7x, relative error
  13.73%.
- Fig. 17 bandwidth: 4.208020x versus 4.0x, relative error
  5.20%.

## V8-03: Revision Audit

`revision_parameter_diff.json` reconstructs the historical profile from
`7380832` and records `hbm_channels` as `8 -> 16`, not `null -> 16`. That audit
also records the earlier row-first calibration change and therefore does not
claim that the full project history is free of retuning.

`v8_revision_no_retuning_audit.json` separately compares `e3a4f13` with the
implementation SHA. The paper target digest and all declared calibration keys
are unchanged. The workload manifest changes are limited to FIFO window policy,
provenance text, and schema metadata. Therefore this review's
`no_target_parameter_retuning=true` is derived rather than asserted.

## Sensitivity And Regression

- Partition sensitivity: 4 MiB FAIL, 5 MiB PASS, 6 MiB FAIL; no independent
  hold-out is claimed.
- Model sensitivity: 10 scenarios completed. The bundled two-stack
  profile remains required; alternatives remain diagnostic and are not selected
  by paper-target error.
- Complete admission trace: 21 files,
  16,341,862 admission cycles,
  64,675,995 blocks, max
  4 blocks/cycle, all independently decoded.
- Clean Release build: PASS; CTest: 9/9 PASS; legacy Cora/Citeseer: PASS;
  strict OpenSpec: PASS.

## Scope Boundary

This repository remains a C++17 request-level/event-driven reproduction. It
contains no Verilog, SystemVerilog, or VHDL RTL and does not claim synthesis,
STA, FPGA execution, area/power, or cycle-accurate RTL equivalence.
