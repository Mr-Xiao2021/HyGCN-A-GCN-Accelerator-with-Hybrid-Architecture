# MEGA Final Validation

## Revisions

- Implementation commit: `ca1486b1eb3d61c0b9d6197dd568a0ebb38f5b84`
- Benchmark tooling commit: `b957baa9e106bb80bec1ae47a907a170af3465e2`
- Branch: `dev/mega`
- Remote: `origin/dev/mega`
- Result-embedded revision: `ca1486b1eb3d`

## Validation

- Clean Release build: PASS (`build-mega-final-ca1486b`)
- CTest: 18/18 PASS
- Legacy Cora/CiteSeer regression: PASS
- OpenSpec strict validation: PASS
- M0-M3 benchmark validator: PASS
- Raw result revision and claim-boundary check: PASS (12/12 JSON)

## Local Mechanism Results

All values compare M3 (Degree-Aware + Adaptive-Package + Condense-Edge) with the
same-workload M0 32-bit `A(XW)` baseline.

| Dataset | Total-cycle speedup | DRAM-byte reduction | Local gate |
|---|---:|---:|---|
| Cora | 641.531640x | 12.994182x | PASS |
| CiteSeer | 795.355857x | 17.730935x | PASS |
| PubMed | 353.722355x | 7.847424x | PASS |
| Arithmetic mean | 596.869950x | 12.857514x | PASS |

## Claim Boundary

These are deterministic request-model measurements from this repository, but they
use `diagnostic-heuristic` degree/precision manifests and deterministic density
surrogates because the repository does not contain author per-node quantization
artifacts or original feature tensors. The development partition is deterministic
contiguous rather than METIS. Therefore:

- Accepted: local M0-M3 mechanism result with independently recomputable bytes,
  transactions, stage cycles, and total cycles.
- Not claimed: complete reproduction of the paper's 38.3x HyGCN speedup, 108.1x
  DRAM reduction, training accuracy, energy, area, RTL synthesis, or five-workload
  average.
- `--required` is intentionally rejected until exact quantized tensors and a
  qualified METIS partition are supplied.

## Reproduction

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target mega_benchmark
```

The report, raw JSON/CSV, diagnostic manifests, and SHA256 artifact inventory are
under `res/mega/`.
