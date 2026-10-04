# Cap-Free Figure 17 Final Status

Date: 2026-10-04

Implementation: `bb5d5d8d4e3b68a0805a2db2641ae06f6aa570f5`

The required profile retains batch-local request ordering, low-bit channel/bank
mapping, shared admission and queue capacities, and no baseline-only active
window throttle. Aggregate optimized/mapping-only speedup and bandwidth gain
remain `1.007813x`; aggregate row-hit ratio remains `1.030872x`.

The cap-free aggregate Figure 17 speedup and bandwidth gain remain
`1.127635x`. Relative errors against the paper's `3.70x` and `4.00x` values are
`69.52%` and `71.81%`. The FIFO window scan remains target-sensitive: only the
diagnostic four-window policy passes, while unbounded and 1/2/8/512 fail.

This revision changes trace schema and independent validation only. It does not
restore the four-window throttle, lower targets, add a baseline-only limit, or
retune performance parameters. Profile/calibration differences and benchmark
metric deltas versus the previous implementation are both zero.

Final status: mechanism direction is observable; request-level Figure 17
magnitude remains OPEN.
