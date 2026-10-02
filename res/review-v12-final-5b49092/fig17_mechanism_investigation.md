# Cap-Free Figure 17 Mechanism Investigation

Date: 2026-10-02

Implementation: `5b49092621db327c338f48cb154f33e0f298794e`

## Published Mechanisms Retained

The required profile retains the mechanisms described by HyGCN Section 4.5.2:

- batch-local `Edge > Input > Weight > Output` request ordering;
- gathering requests from the same processing window or batch;
- low address bits selecting HBM channel and bank;
- no baseline-only active-window throttle.

The common low-bits comparison still records real priority reordering and a
measurable row-locality effect. Aggregate optimized/mapping-only speedup and
bandwidth gain are `1.007813x`; aggregate row-hit ratio is `1.030872x`.

## Remaining Gap

The cap-free aggregate Figure 17 speedup and bandwidth gain remain
`1.127635x`. Relative errors against the paper's `3.70x` and `4.00x` values are
`69.52%` and `71.81%`.

The available paper does not disclose the original Ramulator request-arrival
trace, complete physical address allocation, or an unfinished-window limit
that derives the missing magnitude. The diagnostic FIFO scan remains target
sensitive: only four active windows pass, while unbounded and 1/2/8/512 fail.
This revision therefore does not restore that policy, lower targets, or add a
new baseline-only throttle.

The revision audit reports zero profile/calibration differences and zero
metric deltas relative to the previous implementation. Only trace schema and
independent validation policy changed.

Status: mechanism direction is observable; request-level Figure 17 magnitude
remains OPEN.
