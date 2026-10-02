# Review v12 Closure Report

Date: 2026-10-02

Implementation: `5b49092621db327c338f48cb154f33e0f298794e`

Reviewed remote base: `333b6a8f6d3f5c473e43c3912c4290edff134483`

## Overall Conclusion

The temporal part of V10-02 is closed. The standalone validator now links every
command to the exact admitted request block, rejects commands issued before the
request producer/enqueue/admission boundary, and reconstructs the full
request-level command timeline from the compressed streams.

The cap-free Figure 17 request-level reproduction remains open. This trace
integrity change does not alter scheduling parameters or numerical metrics.

## V10-02: Temporal Request Identity Closure

Status: CLOSED

- Admission evidence is upgraded to reversible schema v3. Every admitted block
  carries `sequence` and `block_offset` alongside directional per-channel queue
  occupancy.
- The validator recovers the exact admission cycle of every expected request
  block and checks identity, direction, address mapping, uniqueness, queue
  deltas, request first/last admission, and terminal occupancy.
- Every PRE/ACT/READ/WRITE command must occur no earlier than the referenced
  request's producer-ready and enqueue cycles and the exact block admission.
- The command stream independently reconstructs request first issue,
  completion, PRE/ACT counts, and first/last PRE/ACT cycles, then compares all
  fields with `memory_requests`.
- The same-row/same-direction identity-swap mutation moves PubMed-style request
  identity `seq=56, block=14`, whose producer-ready cycle is 5715, to cycle 548.
  Chunks, checksum, counters, and formal first-32/last-32 samples are rebuilt;
  the validator rejects it specifically for preceding producer-ready.

Across 21 formal raw files, the independent second replay validates:

- 16,412,982 admission events and 64,675,995 admitted blocks;
- 61,999,387 read blocks and 2,676,608 write blocks;
- 70,813,185 command events and 73,928 reconstructed requests;
- zero identity, mapping, direction, duplicate/missing, command-lane, recovery,
  admission-causality, or request-timeline violations.

Evidence: `complete_trace_evidence.json`, `complete_trace_validation.log`,
`trace_validator_mutations.log`, `raw_file_manifest.csv`, and `raw/*.json.gz`.

## Figure 17 Acceptance Boundary

Status: OPEN, explicitly disclosed

- Required cap-free speedup: `1.127635x` versus `3.70x`, error `69.52%`.
- Required cap-free bandwidth gain: `1.127635x` versus `4.00x`, error `71.81%`.
- Paper metrics: `12/14 PASS`.
- Internal checks: schema v6 `8/12 PASS`; historical v5 `7/12 PASS`.
- No profile/calibration parameter changed and all benchmark metric deltas from
  the previous implementation are zero.
- The diagnostic four-window policy still passes, while unbounded, 1, 2, 8,
  and 512 do not. It remains diagnostic and is not restored as required input.

## Verification

- Clean Release configure/build: PASS
- CTest: 10/10 PASS
- Legacy GCN Cora/Citeseer: PASS
- Strict OpenSpec: PASS
- Trace mutations: 10/10 REJECTED
- Formal raw replay: 21/21 PASS
- Paper metrics: 12/14 PASS
- Internal checks: schema v6 8/12; historical v5 7/12
- Partition sensitivity: 4 MiB FAIL / 5 MiB PASS / 6 MiB FAIL
- Model sensitivity: 10/10 scenarios completed
- FIFO window sensitivity: 0/1/2/4/8/512 completed
- Raw evidence: 21 JSON gzip round-trips and 21 CSV files verified

## Acceptance Boundary

This artifact closes V10-02's command/admission temporal identity finding. It
does not claim that cap-free Figure 17 or the complete request-level HyGCN
mechanism reproduction has passed.
