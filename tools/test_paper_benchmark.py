#!/usr/bin/env python3
import importlib.util
import math
import sys
from pathlib import Path


def load_benchmark_module(root):
    path = root / "tools/paper_benchmark.py"
    spec = importlib.util.spec_from_file_location("paper_benchmark", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    root = Path(__file__).resolve().parents[1]
    benchmark = load_benchmark_module(root)
    optimized = {
        "total_cycles": 1000,
        "total_aggregation_cycles": 100,
        "total_dram_bytes": 500,
        "total_aggregation_dram_bytes": 100,
        "total_input_dram_bytes": 80,
        "bandwidth_utilization": 0.5,
    }
    sparse_optimized = dict(optimized)
    sparse_base = dict(optimized)
    sparse_base.update({
        "total_aggregation_cycles": 200,
        "total_aggregation_dram_bytes": 200,
        "total_input_dram_bytes": 160,
    })
    pipeline_base = dict(optimized)
    pipeline_base.update({"total_cycles": 2000, "total_dram_bytes": 1000})
    coordination_base = dict(optimized)
    coordination_base.update({"total_cycles": 3000, "bandwidth_utilization": 0.1})

    metrics = benchmark.calculate_metrics(
        optimized, sparse_optimized, sparse_base, pipeline_base, coordination_base)
    expected = {
        "sparsity_speedup": 2.0,
        "sparsity_ae_dram_ratio": 0.5,
        "sparsity_input_dram_ratio": 0.5,
        "pipeline_speedup": 2.0,
        "pipeline_dram_ratio": 0.5,
        "coordination_speedup": 3.0,
        "coordination_bandwidth_gain": 5.0,
    }
    for name, value in expected.items():
        if not math.isclose(metrics[name], value, rel_tol=0.0, abs_tol=1e-12):
            raise RuntimeError(f"{name}: expected {value}, got {metrics[name]}")
    if benchmark.VARIANTS["sparsity_optimized"]["scope"] != "aggregation" or \
            benchmark.VARIANTS["sparsity_optimized"]["layer"] != "0":
        raise RuntimeError("Fig. 15 optimized run must be layer-0 aggregation-only")
    if benchmark.VARIANTS["sparsity_baseline"]["scope"] != "aggregation" or \
            benchmark.VARIANTS["sparsity_baseline"]["layer"] != "0":
        raise RuntimeError("Fig. 15 baseline must match layer-0 aggregation-only scope")
    print("F04_ae_only_and_dataset_metric_scope=PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AttributeError, OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
