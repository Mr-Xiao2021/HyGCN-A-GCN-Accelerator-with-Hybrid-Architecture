# Review v13 Final Closure Report

Date: 2026-10-04

Implementation: `bb5d5d8d4e3b68a0805a2db2641ae06f6aa570f5`

Reviewed remote base: `9a3c1c19a428e88f134c13d70a655b73db38d649`

## Overall Conclusion

The Review v12 producer-graph evidence gap is closed. The standalone compiled
validator now reads the producer sequence, request class, base producer-ready,
base enqueue, and producer delay from every memory request. It derives the
consumer release boundary from the producer completion reconstructed from the
full command stream rather than trusting the simulator's reported ready cycle.

The cap-free Figure 17 request-level reproduction remains open. This evidence
repair does not change scheduling, calibration, or performance metrics. Per the
project owner's instruction, this revision is the final delivery candidate and
will be handed to the independent reviewer without another implementation round.

## Review v12 Producer Graph Closure

Status: CLOSED

- Unknown producer sequences and self dependencies are rejected before replay.
- The complete request graph is checked for dependency cycles.
- Input requests must depend on Edge requests with the configured two-cycle
  neighbor-index delay; intermediate reads must depend on their intermediate
  writes with zero delay.
- Effective producer-ready and enqueue cycles are independently derived from
  base fields plus the producer's reconstructed completion cycle.
- Consumer admission and command issue cannot precede producer completion plus
  the declared delay.
- Producer graph/timing results are emitted as a separate oracle rather than
  being folded into simulator-owned request counters.

The compiled mutation suite rejects all 13 mutations, including the new
unknown, self, and future/late producer cases. The future-producer counterexample
changes consumer sequence 15 to depend on a producer completing at cycle 5713;
with the two-cycle delay its dependency-ready cycle is 5715 while the consumer
issues at cycle 548, and the validator rejects it.

Across 21 formal raw files, independent replay validates:

- 16,412,982 admission events and 64,675,995 admitted blocks;
- 70,813,185 command events and 73,928 reconstructed requests;
- 73,640 producer dependencies: 73,619 Input and 21 intermediate RAW;
- delay histogram: 73,619 dependencies at two cycles and 21 at zero cycles;
- zero unknown, self, cycle, future/late, class, or delay violations.

Evidence: `complete_trace_evidence.json`, `complete_trace_validation.log`,
`trace_validator_mutations.log`, `raw_file_manifest.csv`, and `raw/*.json.gz`.

## Figure 17 Acceptance Boundary

Status: OPEN, final disclosed state

- Required cap-free speedup: `1.127635x` versus `3.70x`, error `69.52%`.
- Required cap-free bandwidth gain: `1.127635x` versus `4.00x`, error `71.81%`.
- Paper metrics: `12/14 PASS`.
- Internal checks: schema v6 `8/12 PASS`; historical v5 `7/12 PASS`.
- Profile and calibration differences are zero; all benchmark metric deltas
  from the previous implementation are zero.
- The four-window policy remains a diagnostic calibrated counterfactual and is
  not restored as the required baseline.

## Verification

- Clean Release configure/build: PASS
- CTest: 10/10 PASS
- Legacy GCN Cora/Citeseer: PASS
- Strict OpenSpec: PASS
- Trace mutations: 13/13 REJECTED
- Formal raw replay: 21/21 PASS
- Producer graph/timing replay: 73,640/73,640 PASS
- Paper metrics: 12/14 PASS
- Internal checks: schema v6 8/12; historical v5 7/12
- Partition sensitivity: 4 MiB FAIL / 5 MiB PASS / 6 MiB FAIL
- Model sensitivity: 10/10 scenarios completed
- FIFO window sensitivity: 0/1/2/4/8/512 completed
- Raw evidence: 21 JSON gzip round-trips and 21 CSV files verified

## Final Delivery Boundary

This artifact closes the Review v12 producer-graph and dependency-timing
finding. It does not claim that cap-free Figure 17 or the complete request-level
HyGCN mechanism reproduction has passed. No further implementation iteration is
planned after the independent final review.
