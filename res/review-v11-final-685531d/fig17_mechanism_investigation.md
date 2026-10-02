# Cap-Free Figure 17 Mechanism Investigation

Date: 2026-10-02

Implementation: `685531d4c450d6154208578e9cade613bc85afed`

## Published Mechanisms Retained

The required profile retains the mechanisms explicitly described by HyGCN
Section 4.5.2:

- batch-local `Edge > Input > Weight > Output` request ordering;
- gathering requests from the same processing window or batch;
- low address bits selecting HBM channel and bank;
- no baseline-only active-window throttle.

The formal traces show that these mechanisms are active rather than no-ops.
On the common low-bits mapping, batch/class ordering records 650, 4,234, and
2,584 priority reorders for Cora, Citeseer, and PubMed. It reduces row misses
from 54,900 to 28,020, 320,540 to 253,829, and 291,782 to 190,016,
respectively. The aggregate optimized/mapping-only row-hit ratio is
`1.030872x`.

## Remaining Gap

Despite the observable mechanism effect, the cap-free aggregate Figure 17
speedup and bandwidth gain are both `1.127635x`. Their relative errors against
the paper's `3.70x` and `4.00x` values are `69.52%` and `71.81%`.

The available paper does not disclose the original Ramulator request arrival
trace, complete physical address allocation, or an unfinished-window resource
limit from which the missing magnitude can be derived. The diagnostic FIFO
window scan remains strongly target-sensitive: only four active windows pass,
while the required unbounded case and 1/2/8/512 alternatives fail. Therefore
this revision does not restore the rejected four-window policy, lower targets,
or introduce another baseline-only throttle.

Status: mechanism direction is observable; request-level Figure 17 magnitude
remains OPEN.
