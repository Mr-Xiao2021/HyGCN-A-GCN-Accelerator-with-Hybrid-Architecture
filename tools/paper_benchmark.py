#!/usr/bin/env python3
import argparse
import concurrent.futures
import configparser
import hashlib
import json
import math
import subprocess
import sys
from pathlib import Path


VARIANTS = {
    "optimized": {
        "figure": "figure_17",
        "scope": "full",
        "layer": "0",
        "pipeline": "latency-aware",
        "combination": "independent",
        "sparsity": "on",
        "priority": "batch-class",
        "mapping": "low-bits",
    },
    "sparsity_optimized": {
        "figure": "figure_15",
        "scope": "aggregation",
        "layer": "0",
        "pipeline": "latency-aware",
        "combination": "independent",
        "sparsity": "on",
        "priority": "batch-class",
        "mapping": "low-bits",
    },
    "sparsity_baseline": {
        "figure": "figure_15",
        "scope": "aggregation",
        "layer": "0",
        "pipeline": "latency-aware",
        "combination": "independent",
        "sparsity": "off",
        "priority": "batch-class",
        "mapping": "low-bits",
    },
    "pipeline_baseline": {
        "figure": "figure_16",
        "scope": "full",
        "layer": "0",
        "pipeline": "sequential",
        "combination": "independent",
        "sparsity": "on",
        "priority": "batch-class",
        "mapping": "low-bits",
    },
    "mapping_only": {
        "figure": "figure_17",
        "scope": "full",
        "layer": "0",
        "pipeline": "latency-aware",
        "combination": "independent",
        "sparsity": "on",
        "priority": "fifo",
        "mapping": "low-bits",
    },
    "priority_only": {
        "figure": "figure_17",
        "scope": "full",
        "layer": "0",
        "pipeline": "latency-aware",
        "combination": "independent",
        "sparsity": "on",
        "priority": "batch-class",
        "mapping": "row-first",
    },
    "coordination_baseline": {
        "figure": "figure_17",
        "scope": "full",
        "layer": "0",
        "pipeline": "latency-aware",
        "combination": "independent",
        "sparsity": "on",
        "priority": "fifo",
        "mapping": "row-first",
    },
}

PAIR_TARGETS = {
    "sparsity_baseline": ("sparsity_optimized", {"sparsity_elimination"}),
    "pipeline_baseline": ("optimized", {"pipeline"}),
    "mapping_only": (
        "coordination_baseline",
        {"address_mapping"},
    ),
    "priority_only": (
        "coordination_baseline",
        {"memory_priority"},
    ),
    "coordination_baseline": (
        "optimized",
        {"memory_priority", "address_mapping", "memory_coordination"},
    ),
}


def parse_args():
    parser = argparse.ArgumentParser(description="Run HyGCN paper ablations")
    parser.add_argument("--binary", default="build/hygcntest")
    parser.add_argument("--reference", default="configs/paper_metrics.json")
    parser.add_argument("--workloads", default="configs/paper_workloads.json")
    parser.add_argument(
        "--parameter-baseline", default="configs/paper_parameter_baseline.json"
    )
    parser.add_argument("--profile-path")
    parser.add_argument("--output-dir", default="res/paper")
    parser.add_argument("--datasets", nargs="*")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--force", action="store_true")
    return parser.parse_args()


def result_name(dataset, variant):
    flags = VARIANTS[variant]
    stem = (
        f"paper_paper_gcn_{dataset}_{flags['pipeline']}_{flags['combination']}_"
        f"sparse-{flags['sparsity']}_priority-{flags['priority']}_"
        f"mapping-{flags['mapping']}_seed-1"
    )
    if flags["scope"] == "aggregation":
        stem += "_scope-aggregation"
    if flags["layer"] != "all":
        stem += f"_layer-{flags['layer']}"
    return stem + ".json"


def fnv1a_digest(path):
    value = 1469598103934665603
    prime = 1099511628211
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(8192)
            if not chunk:
                break
            for byte in chunk:
                value ^= byte
                value = (value * prime) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def sha256_digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8192), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path):
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def derive_dramsim3_timing(root, profile_path, workloads):
    definition = workloads["memory_ablation"]["dram_timing"]
    dram_path = Path(definition["config"])
    if not dram_path.is_absolute():
        dram_path = root / dram_path
    profile = configparser.ConfigParser()
    dram = configparser.ConfigParser()
    with profile_path.open(encoding="utf-8") as stream:
        profile.read_file(stream)
    with dram_path.open(encoding="utf-8") as stream:
        dram.read_file(stream)
    frequency_ghz = profile.getfloat("architecture", "frequency_ghz")
    model_cycle_ns = 1.0 / frequency_ghz
    tck_ns = dram.getfloat("timing", "tCK")
    cl = dram.getint("timing", "CL")
    trcdrd = dram.getint("timing", "tRCDRD")
    trp = dram.getint("timing", "tRP")
    row_hit_cycles = math.ceil(cl * tck_ns / model_cycle_ns - 1e-12)
    row_miss_cycles = math.ceil((trcdrd + cl) * tck_ns / model_cycle_ns - 1e-12)
    row_conflict_cycles = math.ceil(
        (trp + trcdrd + cl) * tck_ns / model_cycle_ns - 1e-12
    )
    configured_hit = profile.getint("memory", "hbm_row_hit_cycles")
    configured_miss = profile.getint("memory", "hbm_row_miss_cycles")
    configured_conflict = profile.getint("memory", "hbm_row_conflict_cycles")
    transaction_queue_entries = dram.getint("system", "trans_queue_size")
    command_queue_entries = dram.getint("system", "cmd_queue_size")
    configured_transaction_queue = profile.getint(
        "memory", "hbm_transaction_queue_entries_per_channel"
    )
    configured_command_queue = profile.getint(
        "memory", "hbm_command_queue_entries_per_bank"
    )
    if (configured_hit, configured_miss, configured_conflict) != (
            row_hit_cycles, row_miss_cycles, row_conflict_cycles):
        raise ValueError(
            "paper HBM timing does not match the DRAMSim3-derived hit/miss/conflict "
            f"cycles: configured={configured_hit}/{configured_miss}/{configured_conflict} "
            f"derived={row_hit_cycles}/{row_miss_cycles}/{row_conflict_cycles}"
        )
    if (configured_transaction_queue, configured_command_queue) != (
            transaction_queue_entries, command_queue_entries):
        raise ValueError(
            "paper HBM queue capacities do not match DRAMSim3: "
            f"configured={configured_transaction_queue}/{configured_command_queue} "
            f"source={transaction_queue_entries}/{command_queue_entries}"
        )
    return {
        "config": str(dram_path.relative_to(root)),
        "config_sha256": sha256_digest(dram_path),
        "tck_ns": tck_ns,
        "cl": cl,
        "trcdrd": trcdrd,
        "trp": trp,
        "model_frequency_ghz": frequency_ghz,
        "model_cycle_ns": model_cycle_ns,
        "derived_row_hit_cycles": row_hit_cycles,
        "derived_row_miss_cycles": row_miss_cycles,
        "derived_row_conflict_cycles": row_conflict_cycles,
        "transaction_queue_entries_per_channel": transaction_queue_entries,
        "command_queue_entries_per_bank": command_queue_entries,
        "formula": definition,
    }


def expected_graph_digest(root, dataset):
    graph_dir = root / "gcn_dataset"
    return (
        f"{fnv1a_digest(graph_dir / f'{dataset}.txt')}-"
        f"{fnv1a_digest(graph_dir / f'{dataset}_edge.csv')}"
    )


def dataset_features(root, dataset):
    metadata = root / "gcn_dataset" / f"{dataset}.txt"
    with metadata.open(encoding="utf-8") as stream:
        fields = dict(line.strip().split(",", 1) for line in stream if line.strip())
    return int(fields["feature"])


def cached_result_valid(result, root, binary, dataset, variant, profile_path):
    flags = VARIANTS[variant]
    manifest = result.get("manifest", {})
    expected = {
        "model": "gcn",
        "dataset": dataset,
        "profile": "paper",
        "binary_digest": fnv1a_digest(binary),
        "graph_digest": expected_graph_digest(root, dataset),
        "config_digest": fnv1a_digest(profile_path),
        "seed": 1,
        "selected_layer": flags["layer"],
        "pipeline": flags["pipeline"],
        "combination": flags["combination"],
        "sparsity_elimination": flags["sparsity"] == "on",
        "memory_priority": flags["priority"],
        "address_mapping": flags["mapping"],
        "aggregation_only": flags["scope"] == "aggregation",
    }
    return all(manifest.get(key) == value for key, value in expected.items())


def validate_workload(result, root, dataset, definition):
    manifest = result["manifest"]
    layers = result["layers"]
    if manifest["model"] != definition["model"]:
        raise ValueError(f"{dataset} workload model does not match manifest")
    if manifest["selected_layer"] != definition["selected_layer"] or len(layers) != 1:
        raise ValueError(f"{dataset} workload must contain exactly selected layer 0")
    layer = layers[0]
    if layer["layer"] != 0 or layer["input_features"] != dataset_features(root, dataset):
        raise ValueError(f"{dataset} input layer shape does not match dataset metadata")
    if layer["output_features"] != definition["output_features"]:
        raise ValueError(f"{dataset} output width does not match Table 5 workload")
    expected_aggregation = definition["scope"] == "aggregation"
    if manifest["aggregation_only"] != expected_aggregation:
        raise ValueError(f"{dataset} workload scope does not match figure manifest")


def validate_graph_partition(result, workload_manifest, dataset):
    expected = workload_manifest["graph_partition"]["aggregation_shard_capacity_bytes"]
    measured = result["architecture"]["aggregation_shard_capacity_bytes"]
    if measured != expected:
        raise ValueError(
            f"{dataset} aggregation shard cap is {measured}, expected {expected}"
        )


def run_variant(
    root, binary, output_dir, dataset, variant, force, profile_path, workload_manifest
):
    result_path = output_dir / result_name(dataset, variant)
    result = None
    if not force and result_path.is_file():
        try:
            candidate = load_json(result_path)
            if cached_result_valid(candidate, root, binary, dataset, variant, profile_path):
                result = candidate
        except (OSError, KeyError, json.JSONDecodeError):
            result = None
    if result is None:
        flags = VARIANTS[variant]
        command = [
            str(binary),
            "--engine", "paper",
            "--profile", "paper",
            "--model", "gcn",
            "--dataset", dataset,
            "--scope", flags["scope"],
            "--layer", flags["layer"],
            "--pipeline", flags["pipeline"],
            "--combination", flags["combination"],
            "--sparsity", flags["sparsity"],
            "--priority", flags["priority"],
            "--mapping", flags["mapping"],
            "--seed", "1",
            "--output-dir", str(output_dir),
            "--quiet",
        ]
        if profile_path:
            command.extend(["--profile-path", str(profile_path)])
        subprocess.run(command, cwd=root, check=True)
        result = load_json(result_path)
        if not cached_result_valid(result, root, binary, dataset, variant, profile_path):
            raise ValueError(f"generated result manifest is invalid: {result_path}")
    validate_workload(
        result, root, dataset, workload_manifest["figures"][VARIANTS[variant]["figure"]]
    )
    validate_graph_partition(result, workload_manifest, dataset)
    return result


def ratio(numerator, denominator, name):
    if not math.isfinite(numerator) or not math.isfinite(denominator) or denominator == 0:
        raise ValueError(f"invalid ratio inputs for {name}: {numerator}/{denominator}")
    return numerator / denominator


def mean(values):
    if not values:
        raise ValueError("cannot average an empty metric list")
    return sum(values) / len(values)


def summarize_result(result):
    summary = dict(result["summary"])
    row_hits = sum(layer["row_buffer_hits"] for layer in result["layers"])
    row_misses = sum(layer["row_buffer_misses"] for layer in result["layers"])
    summary["row_buffer_hits"] = row_hits
    summary["row_buffer_misses"] = row_misses
    summary["row_hit_rate"] = ratio(row_hits, row_hits + row_misses, "row_hit_rate")
    return summary


def validate_pair(optimized, baseline, allowed, dataset, name):
    optimized_manifest = optimized["manifest"]
    baseline_manifest = baseline["manifest"]
    ignored = {"git_commit"}
    keys = set(optimized_manifest) | set(baseline_manifest)
    changed = {
        key
        for key in keys
        if key not in ignored and optimized_manifest.get(key) != baseline_manifest.get(key)
    }
    if changed != allowed:
        raise ValueError(
            f"{dataset} {name} ablation changed {sorted(changed)}, "
            f"expected {sorted(allowed)}"
        )
    if optimized.get("architecture") != baseline.get("architecture"):
        raise ValueError(f"{dataset} {name} ablation changed architecture parameters")


def calculate_metrics(
    optimized,
    sparse_optimized,
    sparse_base,
    pipeline_base,
    priority_only,
    mapping_only,
    coordination_base,
):
    return {
        "sparsity_speedup": ratio(
            sparse_base["total_aggregation_cycles"],
            sparse_optimized["total_aggregation_cycles"],
            "sparsity_speedup",
        ),
        "sparsity_ae_dram_ratio": ratio(
            sparse_optimized["total_aggregation_dram_bytes"],
            sparse_base["total_aggregation_dram_bytes"],
            "sparsity_ae_dram_ratio",
        ),
        "sparsity_input_dram_ratio": ratio(
            sparse_optimized["total_input_dram_bytes"],
            sparse_base["total_input_dram_bytes"],
            "sparsity_input_dram_ratio",
        ),
        "pipeline_speedup": ratio(
            pipeline_base["total_cycles"], optimized["total_cycles"], "pipeline_speedup"
        ),
        "pipeline_dram_ratio": ratio(
            optimized["total_dram_bytes"],
            pipeline_base["total_dram_bytes"],
            "pipeline_dram_ratio",
        ),
        "priority_speedup": ratio(
            coordination_base["total_cycles"],
            priority_only["total_cycles"],
            "priority_speedup",
        ),
        "priority_bandwidth_gain": ratio(
            priority_only["bandwidth_utilization"],
            coordination_base["bandwidth_utilization"],
            "priority_bandwidth_gain",
        ),
        "priority_incremental_speedup": ratio(
            mapping_only["total_cycles"],
            optimized["total_cycles"],
            "priority_incremental_speedup",
        ),
        "priority_incremental_speedup_aggregate": ratio(
            mapping_only["total_cycles"],
            optimized["total_cycles"],
            "priority_incremental_speedup_aggregate",
        ),
        "priority_incremental_bandwidth_gain": ratio(
            optimized["bandwidth_utilization"],
            mapping_only["bandwidth_utilization"],
            "priority_incremental_bandwidth_gain",
        ),
        "priority_incremental_bandwidth_gain_aggregate": ratio(
            optimized["bandwidth_utilization"],
            mapping_only["bandwidth_utilization"],
            "priority_incremental_bandwidth_gain_aggregate",
        ),
        "priority_incremental_row_hit_ratio": ratio(
            optimized["row_hit_rate"],
            mapping_only["row_hit_rate"],
            "priority_incremental_row_hit_ratio",
        ),
        "priority_incremental_row_hit_ratio_aggregate": ratio(
            optimized["row_hit_rate"],
            mapping_only["row_hit_rate"],
            "priority_incremental_row_hit_ratio_aggregate",
        ),
        "mapping_speedup": ratio(
            coordination_base["total_cycles"],
            mapping_only["total_cycles"],
            "mapping_speedup",
        ),
        "mapping_bandwidth_gain": ratio(
            mapping_only["bandwidth_utilization"],
            coordination_base["bandwidth_utilization"],
            "mapping_bandwidth_gain",
        ),
        "coordination_speedup": ratio(
            coordination_base["total_cycles"],
            optimized["total_cycles"],
            "coordination_speedup",
        ),
        "coordination_bandwidth_gain": ratio(
            optimized["bandwidth_utilization"],
            coordination_base["bandwidth_utilization"],
            "coordination_bandwidth_gain",
        ),
        "coordination_active_bandwidth_gain": ratio(
            optimized["active_bandwidth_utilization"],
            coordination_base["active_bandwidth_utilization"],
            "coordination_active_bandwidth_gain",
        ),
    }


def validate_sequential_traffic(dataset, run):
    layer = run["layers"][0]
    expected = layer["aggregation_buffer_write_bytes"]
    write = layer["request_stats"]["intermediate_write"]["bytes"]
    read = layer["request_stats"]["intermediate_read"]["bytes"]
    if write != expected or read != expected:
        raise ValueError(
            f"{dataset} sequential traffic uses {write}/{read} bytes, expected {expected}"
        )
    return {
        "producer_bytes": expected,
        "intermediate_write_bytes": write,
        "intermediate_read_bytes": read,
        "alignment": run["architecture"]["sequential_spill_alignment"],
    }


def validate_transaction_admission(dataset, variant, run):
    architecture = run["architecture"]
    issue_limit = architecture["coordinator_issue_blocks_per_cycle"]
    queue_capacity = architecture["hbm_transaction_queue_entries_per_channel"]
    channels = architecture["hbm_channels"]
    block_size = architecture["block_size"]
    admitted_total = 0
    expected_total = 0
    max_total_occupancy = 0
    admission_cycles = 0
    for layer in run["layers"]:
        summary = layer["transaction_admission_trace"]
        traces = summary["trace_samples"]
        previous_cycle = None
        for trace in traces:
            admitted = trace["admitted_blocks"]
            if admitted <= 0 or admitted > issue_limit:
                raise ValueError(
                    f"{dataset} {variant} admits {admitted} blocks in one cycle; "
                    f"limit is {issue_limit}"
                )
            if previous_cycle is not None and trace["cycle"] <= previous_cycle:
                raise ValueError(f"{dataset} {variant} admission cycles are not increasing")
            if trace["max_channel_occupancy_after"] > queue_capacity:
                raise ValueError(f"{dataset} {variant} sample exceeds a channel queue capacity")
            previous_cycle = trace["cycle"]
        peaks = summary["max_channel_queue_occupancy"]
        if len(peaks) != channels or any(value > queue_capacity for value in peaks):
            raise ValueError(f"{dataset} {variant} exceeds a channel queue capacity")
        if summary["max_blocks_admitted_per_cycle"] > issue_limit:
            raise ValueError(f"{dataset} {variant} exceeds the shared admission bandwidth")
        admitted_total += summary["admitted_blocks"]
        admission_cycles += summary["cycle_count"]
        max_total_occupancy = max(
            max_total_occupancy, summary["max_total_queue_occupancy"]
        )
        for request in layer["memory_requests"]:
            expected_total += math.ceil(request["bytes"] / block_size)
            if not (
                request["first_admission_cycle"] >= request["enqueue_cycle"]
                and request["last_admission_cycle"] >= request["first_admission_cycle"]
                and request["first_issue_cycle"] >= request["first_admission_cycle"]
            ):
                raise ValueError(f"{dataset} {variant} request admission violates causality")
    if admitted_total != expected_total:
        raise ValueError(
            f"{dataset} {variant} admission trace covers {admitted_total} blocks, "
            f"expected {expected_total}"
        )
    return {
        "admitted_blocks": admitted_total,
        "admission_cycles": admission_cycles,
        "max_blocks_admitted_per_cycle": max(
            layer["transaction_admission_trace"]["max_blocks_admitted_per_cycle"]
            for layer in run["layers"]
        ),
        "issue_limit_blocks_per_cycle": issue_limit,
        "max_total_queue_occupancy": max_total_occupancy,
        "total_queue_capacity": channels * queue_capacity,
    }


def evidence_counts(reference):
    counts = {"paper_metric_rows": 0, "internal_check_rows": 0}
    for definition in reference["metrics"].values():
        if not definition["required"]:
            continue
        validation = definition["validation"]
        rows = len(reference["datasets"]) if validation.startswith("per_dataset") else 1
        key = (
            "paper_metric_rows"
            if definition["evidence_class"] == "paper_metric"
            else "internal_check_rows"
        )
        counts[key] += rows
    return counts


def priority_trace_evidence(dataset, optimized, mapping_only):
    optimized_layer = optimized["layers"][0]
    baseline_layer = mapping_only["layers"][0]
    baseline_by_sequence = {
        trace["sequence"]: trace for trace in baseline_layer["memory_requests"]
    }
    changed = []
    for trace in optimized_layer["memory_requests"]:
        baseline = baseline_by_sequence.get(trace["sequence"])
        if baseline is None:
            continue
        if (
            trace["first_issue_cycle"] != baseline["first_issue_cycle"]
            or trace["completion_cycle"] != baseline["completion_cycle"]
        ):
            changed.append(
                {
                    "sequence": trace["sequence"],
                    "batch_id": trace["batch_id"],
                    "request_class": trace["request_class"],
                    "producer_sequence": trace.get("producer_sequence"),
                    "optimized_issue": trace["first_issue_cycle"],
                    "fifo_issue": baseline["first_issue_cycle"],
                    "optimized_completion": trace["completion_cycle"],
                    "fifo_completion": baseline["completion_cycle"],
                }
            )
            if len(changed) == 8:
                break
    if optimized_layer["priority_reorders"] <= 0 or not changed:
        raise ValueError(f"{dataset} priority-only ablation has no observable trace effect")
    return {
        "priority_reorders": optimized_layer["priority_reorders"],
        "address_mapping": optimized["manifest"]["address_mapping"],
        "optimized_row_buffer_hits": optimized_layer["row_buffer_hits"],
        "optimized_row_buffer_misses": optimized_layer["row_buffer_misses"],
        "fifo_row_buffer_hits": baseline_layer["row_buffer_hits"],
        "fifo_row_buffer_misses": baseline_layer["row_buffer_misses"],
        "changed_requests": changed,
    }


def current_parameters(architecture, workloads):
    return {
        "aggregation_ping_pong_regions": architecture["aggregation_ping_pong_regions"],
        "aggregation_shard_capacity_bytes": architecture["aggregation_shard_capacity_bytes"],
        "batch_launch_interval_cycles": architecture["batch_launch_interval_cycles"],
        "benchmark_selected_layer": workloads["figures"]["figure_17"]["selected_layer"],
        "edge_input_dependency": "edge_completion_plus_neighbor_index_ready",
        "edge_ping_pong_regions": architecture["edge_ping_pong_regions"],
        "hbm_transaction_queue_entries_per_channel":
            architecture["hbm_transaction_queue_entries_per_channel"],
        "hbm_command_queue_entries_per_bank":
            architecture["hbm_command_queue_entries_per_bank"],
        "hbm_row_hit_cycles": architecture["hbm_row_hit_cycles"],
        "hbm_row_miss_cycles": architecture["hbm_row_miss_cycles"],
        "hbm_row_conflict_cycles": architecture["hbm_row_conflict_cycles"],
        "coordinator_issue_blocks_per_cycle":
            architecture["coordinator_issue_blocks_per_cycle"],
        "input_ping_pong_regions": architecture["input_ping_pong_regions"],
        "neighbor_index_ready_cycles": architecture["neighbor_index_ready_cycles"],
        "row_first_bank_interleave": architecture["row_first_bank_interleave"],
        "sequential_spill_alignment": architecture["sequential_spill_alignment"],
    }


def parameter_audit(architecture, workloads, baseline):
    current = current_parameters(architecture, workloads)
    previous = baseline["parameters"]
    keys = sorted(set(previous) | set(current))
    differences = [
        {"parameter": key, "baseline": previous.get(key), "current": current.get(key)}
        for key in keys
        if previous.get(key) != current.get(key)
    ]
    return {
        "baseline_version": baseline["baseline_version"],
        "baseline_commit": baseline["baseline_commit"],
        "parameter_recalibration": bool(differences),
        "differences": differences,
        "current": current,
    }


def main():
    args = parse_args()
    if args.jobs <= 0:
        raise ValueError("--jobs must be positive")
    root = Path(__file__).resolve().parents[1]
    binary = Path(args.binary)
    if not binary.is_absolute():
        binary = root / binary
    reference_path = Path(args.reference)
    workload_path = Path(args.workloads)
    baseline_path = Path(args.parameter_baseline)
    output_dir = Path(args.output_dir)
    profile_path = Path(args.profile_path) if args.profile_path else Path("configs/HYGCN_PAPER.ini")
    paths = [reference_path, workload_path, baseline_path, output_dir, profile_path]
    for index, path in enumerate(paths):
        if not path.is_absolute():
            paths[index] = root / path
    reference_path, workload_path, baseline_path, output_dir, profile_path = paths
    output_dir.mkdir(parents=True, exist_ok=True)

    reference = load_json(reference_path)
    workloads = load_json(workload_path)
    parameter_baseline = load_json(baseline_path)
    dram_timing_basis = derive_dramsim3_timing(root, profile_path, workloads)
    datasets = args.datasets or reference["datasets"]
    per_dataset = {}
    metric_values = {name: [] for name in reference["metrics"]}
    architecture = None

    for dataset in datasets:
        def execute_variant(name):
            return run_variant(
                root,
                binary,
                output_dir,
                dataset,
                name,
                args.force,
                profile_path,
                workloads,
            )

        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
            futures = {
                name: executor.submit(execute_variant, name) for name in VARIANTS
            }
            runs = {name: future.result() for name, future in futures.items()}
        for baseline_name, (optimized_name, allowed) in PAIR_TARGETS.items():
            validate_pair(
                runs[optimized_name], runs[baseline_name], allowed, dataset, baseline_name
            )
        architecture = runs["optimized"]["architecture"]
        admission_oracles = {
            name: validate_transaction_admission(dataset, name, result)
            for name, result in runs.items()
        }
        summaries = {name: summarize_result(result) for name, result in runs.items()}
        metrics = calculate_metrics(
            summaries["optimized"],
            summaries["sparsity_optimized"],
            summaries["sparsity_baseline"],
            summaries["pipeline_baseline"],
            summaries["priority_only"],
            summaries["mapping_only"],
            summaries["coordination_baseline"],
        )
        per_dataset[dataset] = {
            "metrics": metrics,
            "runs": {name: result_name(dataset, name) for name in VARIANTS},
            "raw": summaries,
            "sequential_traffic_oracle": validate_sequential_traffic(
                dataset, runs["pipeline_baseline"]
            ),
            "priority_trace_evidence": priority_trace_evidence(
                dataset, runs["optimized"], runs["mapping_only"]
            ),
            "mapping_evidence": {
                "baseline_channel_blocks": runs["coordination_baseline"]["layers"][0][
                    "channel_blocks"
                ],
                "optimized_channel_blocks": runs["mapping_only"]["layers"][0][
                    "channel_blocks"
                ],
                "baseline_bank_blocks": runs["coordination_baseline"]["layers"][0][
                    "bank_blocks"
                ],
                "optimized_bank_blocks": runs["mapping_only"]["layers"][0][
                    "bank_blocks"
                ],
            },
            "transaction_admission_oracles": admission_oracles,
        }
        for name, value in metrics.items():
            metric_values[name].append(value)

    aggregate = {name: mean(values) for name, values in metric_values.items()}
    audit = parameter_audit(architecture, workloads, parameter_baseline)
    report = {
        "schema_version": 2,
        "reference_file": str(reference_path.relative_to(root)),
        "workload_manifest": {
            "path": str(workload_path.relative_to(root)),
            "sha256": sha256_digest(workload_path),
            "version": workloads["workload_version"],
            "figures": workloads["figures"],
            "graph_partition": workloads["graph_partition"],
        },
        "parameter_baseline_file": str(baseline_path.relative_to(root)),
        "datasets": datasets,
        "aggregate": aggregate,
        "per_dataset": per_dataset,
        "memory_ablation": workloads["memory_ablation"],
        "dram_timing_basis": dram_timing_basis,
        "evidence_counts": evidence_counts(reference),
        "parameter_audit": audit,
        "parameter_recalibration": audit["parameter_recalibration"],
        "scope": {
            "validated": (
                "Fig. 15 layer-0 AE-only sparsity and Fig. 16-17 layer-0 "
                "request-level microarchitectural effects for GCN"
            ),
            "not_validated": [
                "absolute CPU speedup",
                "absolute GPU speedup",
                "DiffPool",
                "complete chip energy and area",
                "exact ping-pong half ownership",
                "automatic SVG redownload and redigitization",
            ],
        },
        "cache_validation": "input and configuration digests plus complete run manifest",
    }
    report_path = output_dir / "benchmark_report.json"
    with report_path.open("w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(report_path)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
