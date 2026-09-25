#!/usr/bin/env python3
import importlib.util
import json
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
    with (root / "configs/paper_workloads.json").open(encoding="utf-8") as stream:
        workloads = json.load(stream)
    with (root / "configs/paper_parameter_baseline.json").open(encoding="utf-8") as stream:
        parameter_baseline = json.load(stream)
    for figure in ("figure_15", "figure_16", "figure_17"):
        definition = workloads["figures"][figure]
        if definition["selected_layer"] != "0" or definition["output_features"] != 128:
            raise RuntimeError(f"{figure} is not bound to the Table 5 layer-0 shape")
    if workloads["memory_ablation"]["row_first_bank_interleave"] != 2:
        raise RuntimeError("row-first baseline must expose the audited HBM command lanes")
    if workloads["graph_partition"]["aggregation_shard_capacity_bytes"] != 5242880:
        raise RuntimeError("graph partition scheduler cap must be versioned")
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
    priority_only = dict(optimized)
    priority_only.update({"total_cycles": 2000, "bandwidth_utilization": 0.2})
    mapping_only = dict(optimized)
    mapping_only.update({"total_cycles": 1500, "bandwidth_utilization": 0.25})

    metrics = benchmark.calculate_metrics(
        optimized,
        sparse_optimized,
        sparse_base,
        pipeline_base,
        priority_only,
        mapping_only,
        coordination_base,
    )
    expected = {
        "sparsity_speedup": 2.0,
        "sparsity_ae_dram_ratio": 0.5,
        "sparsity_input_dram_ratio": 0.5,
        "pipeline_speedup": 2.0,
        "pipeline_dram_ratio": 0.5,
        "priority_speedup": 1.5,
        "priority_bandwidth_gain": 2.0,
        "mapping_speedup": 2.0,
        "mapping_bandwidth_gain": 2.5,
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
    for name, flags in benchmark.VARIANTS.items():
        if flags["layer"] != "0":
            raise RuntimeError(f"{name} must use the Table 5 layer-0 workload")
    if benchmark.VARIANTS["priority_only"]["mapping"] != "row-first":
        raise RuntimeError("priority-only ablation must preserve baseline mapping")
    if benchmark.VARIANTS["mapping_only"]["priority"] != "fifo":
        raise RuntimeError("mapping-only ablation must preserve baseline priority")
    architecture = {
        "aggregation_ping_pong_regions": 2,
        "aggregation_shard_capacity_bytes": 5242880,
        "batch_launch_interval_cycles": 1,
        "edge_ping_pong_regions": 2,
        "input_ping_pong_regions": 2,
        "neighbor_index_ready_cycles": 2,
        "row_first_bank_interleave": 2,
        "sequential_spill_alignment": "block",
    }
    audit = benchmark.parameter_audit(architecture, workloads, parameter_baseline)
    if audit["parameter_recalibration"] != bool(audit["differences"]) or not audit["differences"]:
        raise RuntimeError("parameter recalibration must be derived from a non-empty diff")
    print("F04_ae_only_and_R3_workload_scope=PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AttributeError, OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
