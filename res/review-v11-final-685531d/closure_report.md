# Review v11 Closure Report

Date: 2026-10-02

Implementation: `685531d4c450d6154208578e9cade613bc85afed`

Base reviewed revision: `d0c39037605d80b812b0ddaeede927c0c665f450`

## Overall Conclusion

V10-02 is closed. The standalone validator now links every command event to a
real request block and independently validates identity, block bounds,
direction, address mapping, uniqueness/completeness, and directional admission
totals. The cap-free Figure 17 request-level reproduction remains open and is
not hidden by the trace-integrity fix.

## V10-02: Command Identity and Address Closure

Status: CLOSED

- `memory_requests` is converted into a unique sequence table and complete
  expected `(sequence, block_offset)` set.
- Every PRE/ACT/READ/WRITE command must reference an existing sequence and an
  in-range block.
- Channel, bank, and data/ACT row are recomputed from the request address and
  effective `low-bits` or `row-first` mapping. PRE may carry the old open row,
  but its target request identity and channel/bank must still match.
- READ/WRITE direction is derived from the request class.
- Every expected block must have exactly one data command; duplicate and
  missing blocks fail validation.
- Data-command read/write totals must equal independently reconstructed
  admission read/write totals.

Across 21 formal raw files, independent replay validates 70,813,185 command
events and 64,675,995 expected request blocks. It reconstructs 61,999,387
reads and 2,676,608 writes, exactly matching admission. Identity, mapping,
direction, duplicate/missing, command-lane, and timing violations are all zero.

Mutation tests reject 9/9 cases, including the five V10-02 counterexamples:
unknown sequence, out-of-range block, wrong direction, wrong mapping, and
duplicate plus missing data block. Chunks, counts, checksums, samples, and
simulator counters are regenerated in those mutations, so rejection does not
depend on stale self-reported metadata.

Evidence: `complete_trace_evidence.json`, `complete_trace_validation.log`,
`trace_validator_mutations.log`, `raw_file_manifest.csv`, and `raw/*.json.gz`.

## V10-01: Cap-Free Figure 17 Gap

Status: OPEN, explicitly disclosed

- Required cap-free speedup: `1.127635x` versus `3.70x`, error `69.52%`.
- Required cap-free bandwidth gain: `1.127635x` versus `4.00x`, error `71.81%`.
- Paper metrics: `12/14 PASS`.
- Internal checks: schema v6 `8/12 PASS`; historical v5 `7/12 PASS`.
- No active-window cap, target change, gate reduction, or new baseline-only
  throttle was introduced.

The source-defined priority and mapping mechanisms remain trace-observable, but
the paper does not publish enough request-arrival/address detail to derive the
missing magnitude without a new assumption. See
`fig17_mechanism_investigation.md`.

## Verification

- Clean Release configure/build: PASS
- CTest: 10/10 PASS
- Legacy GCN Cora/Citeseer: PASS
- Strict OpenSpec: PASS
- Trace mutations: 9/9 REJECTED
- Paper metrics: 12/14 PASS
- Internal checks: schema v6 8/12; historical v5 7/12
- Partition sensitivity: 4 MiB FAIL / 5 MiB PASS / 6 MiB FAIL
- Model sensitivity: 10/10 scenarios completed
- FIFO window sensitivity: 0/1/2/4/8/512 completed
- Raw evidence: 21 JSON gzip round-trips and 21 CSV files verified

## Acceptance Boundary

This artifact closes the V10-02 trace identity/address finding. It does not
claim that cap-free Figure 17 or the complete request-level HyGCN reproduction
has passed.
