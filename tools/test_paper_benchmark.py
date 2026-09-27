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


def load_partition_module(root, benchmark):
    path = root / "tools/partition_sensitivity.py"
    spec = importlib.util.spec_from_file_location("partition_sensitivity", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules["paper_benchmark"] = benchmark
    spec.loader.exec_module(module)
    return module


def main():
    root = Path(__file__).resolve().parents[1]
    benchmark = load_benchmark_module(root)
    partition = load_partition_module(root, benchmark)
    with (root / "configs/paper_workloads.json").open(encoding="utf-8") as stream:
        workloads = json.load(stream)
    with (root / "configs/paper_parameter_baseline.json").open(encoding="utf-8") as stream:
        parameter_baseline = json.load(stream)
    for figure in ("figure_15", "figure_16", "figure_17"):
        definition = workloads["figures"][figure]
        if definition["selected_layer"] != "0" or definition["output_features"] != 128:
            raise RuntimeError(f"{figure} is not bound to the Table 5 layer-0 shape")
    if workloads["memory_ablation"]["row_first_bank_interleave"] != 1:
        raise RuntimeError("row-first baseline must not add unsupported bank striping")
    if "Section 4.5.2" not in workloads["memory_ablation"]["priority_order_source"]:
        raise RuntimeError("batch-class ordering must retain its paper provenance")
    if workloads["memory_ablation"]["command_queue_entries_per_bank"] != 8:
        raise RuntimeError("command queue depth must retain its DRAMSim3 provenance")
    if workloads["graph_partition"]["calibrated_fit_datasets"] != [
            "cora", "citeseer", "pubmed"] or \
            workloads["graph_partition"]["independent_holdout_datasets"] != []:
        raise RuntimeError("all historically exposed datasets must be labeled calibrated fit")
    if workloads["graph_partition"]["aggregation_shard_capacity_bytes"] != 5242880:
        raise RuntimeError("graph partition scheduler cap must be versioned")
    optimized = {
        "total_cycles": 1000,
        "total_aggregation_cycles": 100,
        "total_dram_bytes": 500,
        "total_aggregation_dram_bytes": 100,
        "total_input_dram_bytes": 80,
        "bandwidth_utilization": 0.5,
        "active_bandwidth_utilization": 0.5,
        "row_hit_rate": 0.8,
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
    coordination_base.update({
        "total_cycles": 3000,
        "bandwidth_utilization": 0.1,
        "active_bandwidth_utilization": 0.1,
        "row_hit_rate": 0.7,
    })
    priority_only = dict(optimized)
    priority_only.update({
        "total_cycles": 2000,
        "bandwidth_utilization": 0.2,
        "active_bandwidth_utilization": 0.2,
        "row_hit_rate": 0.8,
    })
    mapping_only = dict(optimized)
    mapping_only.update({
        "total_cycles": 1500,
        "bandwidth_utilization": 0.25,
        "active_bandwidth_utilization": 0.3,
        "row_hit_rate": 0.75,
    })

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
        "priority_incremental_speedup": 1.5,
        "priority_incremental_speedup_aggregate": 1.5,
        "priority_incremental_bandwidth_gain": 2.0,
        "priority_incremental_bandwidth_gain_aggregate": 2.0,
        "priority_incremental_row_hit_ratio": 0.8 / 0.75,
        "priority_incremental_row_hit_ratio_aggregate": 0.8 / 0.75,
        "mapping_speedup": 2.0,
        "mapping_bandwidth_gain": 2.5,
        "coordination_speedup": 3.0,
        "coordination_bandwidth_gain": 5.0,
        "coordination_active_bandwidth_gain": 5.0,
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
        "hbm_read_queue_entries_per_channel": 32,
        "hbm_write_buffer_entries_per_channel": 32,
        "hbm_command_queue_entries_per_bank": 8,
        "hbm_read_row_hit_cycles": 14,
        "hbm_read_row_miss_cycles": 28,
        "hbm_read_row_conflict_cycles": 42,
        "hbm_write_row_hit_cycles": 4,
        "hbm_write_row_miss_cycles": 18,
        "hbm_write_row_conflict_cycles": 32,
        "hbm_read_to_write_cycles": 18,
        "hbm_write_to_read_cycles": 16,
        "coordinator_issue_blocks_per_cycle": 4,
        "input_ping_pong_regions": 2,
        "neighbor_index_ready_cycles": 2,
        "row_first_bank_interleave": 1,
        "sequential_spill_alignment": "block",
    }
    audit = benchmark.parameter_audit(architecture, workloads, parameter_baseline)
    if audit["parameter_recalibration"] != bool(audit["differences"]) or not audit["differences"]:
        raise RuntimeError("parameter recalibration must be derived from a non-empty diff")
    external_binary = Path("/tmp/hygcn-external-build/hygcntest")
    if partition.display_path(root, external_binary) != str(external_binary):
        raise RuntimeError("external absolute binary paths must remain printable")
    if partition.display_path(root, root / "build/hygcntest") != "build/hygcntest":
        raise RuntimeError("repository-local binary paths must remain relative")
    timing = benchmark.derive_dramsim3_timing(
        root, root / "configs/HYGCN_PAPER.ini", workloads
    )
    if timing["derived_read_row_hit_cycles"] != 14 or \
            timing["derived_read_row_miss_cycles"] != 28 or \
            timing["derived_read_row_conflict_cycles"] != 42 or \
            timing["derived_write_row_hit_cycles"] != 4 or \
            timing["derived_write_row_miss_cycles"] != 18 or \
            timing["derived_write_row_conflict_cycles"] != 32 or \
            timing["derived_read_to_write_cycles"] != 18 or \
            timing["derived_write_to_read_cycles"] != 16:
        raise RuntimeError("required directional HBM timing must be derived from DRAMSim3")
    if timing["unified_queue"] or \
            timing["read_queue_entries_per_channel"] != 32 or \
            timing["write_buffer_entries_per_channel"] != 32 or \
            timing["command_queue_entries_per_bank"] != 8:
        raise RuntimeError("required directional HBM queues must be derived from DRAMSim3")
    with (root / "configs/paper_metrics.json").open(encoding="utf-8") as stream:
        reference = json.load(stream)
    if benchmark.evidence_counts(reference) != {
            "paper_metric_rows": 14, "internal_check_rows": 12}:
        raise RuntimeError("evidence must remain split into 14 paper rows and 12 checks")
    print("F04_ae_only_and_R3_workload_scope=PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AttributeError, OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
