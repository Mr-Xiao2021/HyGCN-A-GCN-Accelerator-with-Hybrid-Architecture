#!/usr/bin/env python3
import argparse
import base64
import bisect
import collections
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
    parser.add_argument("--trace-validator")
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


def run_compiled_trace_validator(validator, result_path):
    completed = subprocess.run(
        [str(validator), str(result_path)],
        check=True,
        capture_output=True,
        text=True,
    )
    try:
        evidence = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise ValueError(
            f"trace validator returned invalid JSON for {result_path}: {error}"
        ) from error
    if set(evidence) != {
            "transaction_admission_oracle", "command_trace_oracle"}:
        raise ValueError(f"trace validator result schema differs for {result_path}")
    return evidence


def strip_complete_trace_payloads(result):
    for layer in result.get("layers", ()):
        layer.get("transaction_admission_trace", {}).pop("trace_chunks", None)
        layer.get("command_trace", {}).pop("trace_chunks", None)


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
    cwl = dram.getint("timing", "CWL")
    trcdrd = dram.getint("timing", "tRCDRD")
    trcdwr = dram.getint("timing", "tRCDWR")
    trp = dram.getint("timing", "tRP")
    bl = dram.getint("dram_structure", "BL")
    burst_cycle = bl // 2
    al = dram.getint("timing", "AL", fallback=0)
    trtrs = dram.getint("timing", "tRTRS", fallback=2)
    twtr_l = dram.getint("timing", "tWTR_L")
    trtp = dram.getint("timing", "tRTP", fallback=5)
    twr = dram.getint("timing", "tWR")
    tras = dram.getint("timing", "tRAS")
    tccd_l = dram.getint("timing", "tCCD_L")
    source_channels = dram.getint("system", "channels")
    source_bus_width = dram.getint("system", "bus_width")
    configured_channels = profile.getint("memory", "hbm_channels")
    configured_bandwidth_gbps = profile.getfloat("memory", "hbm_bandwidth_gbps")
    read_hit_cycles = math.ceil(cl * tck_ns / model_cycle_ns - 1e-12)
    read_miss_cycles = math.ceil((trcdrd + cl) * tck_ns / model_cycle_ns - 1e-12)
    read_conflict_cycles = math.ceil(
        (trp + trcdrd + cl) * tck_ns / model_cycle_ns - 1e-12
    )
    write_hit_cycles = math.ceil(cwl * tck_ns / model_cycle_ns - 1e-12)
    write_miss_cycles = math.ceil((trcdwr + cwl) * tck_ns / model_cycle_ns - 1e-12)
    write_conflict_cycles = math.ceil(
        (trp + trcdwr + cwl) * tck_ns / model_cycle_ns - 1e-12
    )
    rl = al + cl
    wl = al + cwl
    write_delay = wl + burst_cycle
    read_to_write_cycles = math.ceil(
        (rl + burst_cycle - wl + trtrs) * tck_ns / model_cycle_ns - 1e-12
    )
    write_to_read_cycles = math.ceil(
        (write_delay + twtr_l) * tck_ns / model_cycle_ns - 1e-12
    )
    activate_to_read_cycles = math.ceil(
        trcdrd * tck_ns / model_cycle_ns - 1e-12)
    activate_to_write_cycles = math.ceil(
        trcdwr * tck_ns / model_cycle_ns - 1e-12)
    same_direction_cycles = math.ceil(
        max(burst_cycle, tccd_l) * tck_ns / model_cycle_ns - 1e-12)
    command_issue_interval_cycles = math.ceil(
        tck_ns / model_cycle_ns - 1e-12)
    read_to_precharge_cycles = math.ceil(
        (al + trtp) * tck_ns / model_cycle_ns - 1e-12)
    write_to_precharge_cycles = math.ceil(
        (wl + burst_cycle + twr) * tck_ns / model_cycle_ns - 1e-12)
    activate_to_precharge_cycles = math.ceil(
        tras * tck_ns / model_cycle_ns - 1e-12)
    precharge_to_activate_cycles = math.ceil(
        trp * tck_ns / model_cycle_ns - 1e-12)
    activate_to_activate_cycles = math.ceil(
        (tras + trp) * tck_ns / model_cycle_ns - 1e-12)
    configured_read = tuple(profile.getint("memory", name) for name in (
        "hbm_read_row_hit_cycles",
        "hbm_read_row_miss_cycles",
        "hbm_read_row_conflict_cycles",
    ))
    configured_write = tuple(profile.getint("memory", name) for name in (
        "hbm_write_row_hit_cycles",
        "hbm_write_row_miss_cycles",
        "hbm_write_row_conflict_cycles",
    ))
    configured_switch = tuple(profile.getint("memory", name) for name in (
        "hbm_read_to_write_cycles", "hbm_write_to_read_cycles"
    ))
    configured_commands = tuple(profile.getint("memory", name) for name in (
        "hbm_activate_to_read_cycles",
        "hbm_activate_to_write_cycles",
        "hbm_read_to_read_cycles",
        "hbm_write_to_write_cycles",
        "hbm_read_to_precharge_cycles",
        "hbm_write_to_precharge_cycles",
        "hbm_activate_to_precharge_cycles",
        "hbm_precharge_to_activate_cycles",
        "hbm_activate_to_activate_cycles",
        "hbm_command_issue_interval_cycles",
    ))
    transaction_queue_entries = dram.getint("system", "trans_queue_size")
    command_queue_entries = dram.getint("system", "cmd_queue_size")
    unified_queue = dram.getboolean("system", "unified_queue")
    configured_read_queue = profile.getint(
        "memory", "hbm_read_queue_entries_per_channel")
    configured_write_buffer = profile.getint(
        "memory", "hbm_write_buffer_entries_per_channel")
    configured_command_queue = profile.getint(
        "memory", "hbm_command_queue_entries_per_bank"
    )
    if configured_read != (
            read_hit_cycles, read_miss_cycles, read_conflict_cycles):
        raise ValueError(
            "paper HBM read timing does not match DRAMSim3: "
            f"configured={configured_read} "
            f"derived={(read_hit_cycles, read_miss_cycles, read_conflict_cycles)}"
        )
    if configured_write != (
            write_hit_cycles, write_miss_cycles, write_conflict_cycles):
        raise ValueError(
            "paper HBM write timing does not match DRAMSim3: "
            f"configured={configured_write} "
            f"derived={(write_hit_cycles, write_miss_cycles, write_conflict_cycles)}"
        )
    if configured_switch != (read_to_write_cycles, write_to_read_cycles):
        raise ValueError(
            "paper HBM direction switching does not match DRAMSim3: "
            f"configured={configured_switch} "
            f"derived={(read_to_write_cycles, write_to_read_cycles)}"
        )
    derived_commands = (
        activate_to_read_cycles,
        activate_to_write_cycles,
        same_direction_cycles,
        same_direction_cycles,
        read_to_precharge_cycles,
        write_to_precharge_cycles,
        activate_to_precharge_cycles,
        precharge_to_activate_cycles,
        activate_to_activate_cycles,
        command_issue_interval_cycles,
    )
    if configured_commands != derived_commands:
        raise ValueError(
            "paper HBM command timing does not match DRAMSim3: "
            f"configured={configured_commands} derived={derived_commands}"
        )
    if unified_queue:
        raise ValueError("paper HBM config must expose independent read/write queues")
    if configured_channels % source_channels != 0:
        raise ValueError("paper HBM channels must replicate complete DRAMSim3 stacks")
    stack_count = configured_channels // source_channels
    source_stack_bandwidth_gbps = (
        source_channels * (source_bus_width / 8.0) * bl /
        (burst_cycle * tck_ns)
    )
    if not math.isclose(
            configured_bandwidth_gbps,
            stack_count * source_stack_bandwidth_gbps,
            rel_tol=0.0, abs_tol=1e-9):
        raise ValueError(
            "paper HBM channel replication does not match configured bandwidth: "
            f"channels={configured_channels} source_channels={source_channels} "
            f"configured={configured_bandwidth_gbps} "
            f"derived={stack_count * source_stack_bandwidth_gbps}"
        )
    if (configured_read_queue, configured_write_buffer, configured_command_queue) != (
            transaction_queue_entries, transaction_queue_entries, command_queue_entries):
        raise ValueError(
            "paper HBM queue capacities do not match DRAMSim3: "
            f"configured={configured_read_queue}/{configured_write_buffer}/"
            f"{configured_command_queue} source={transaction_queue_entries}/"
            f"{transaction_queue_entries}/{command_queue_entries}"
        )
    return {
        "config": str(dram_path.relative_to(root)),
        "config_sha256": sha256_digest(dram_path),
        "tck_ns": tck_ns,
        "cl": cl,
        "cwl": cwl,
        "trcdrd": trcdrd,
        "trcdwr": trcdwr,
        "trp": trp,
        "burst_cycle": burst_cycle,
        "trtrs": trtrs,
        "twtr_l": twtr_l,
        "trtp": trtp,
        "twr": twr,
        "tras": tras,
        "tccd_l": tccd_l,
        "model_frequency_ghz": frequency_ghz,
        "model_cycle_ns": model_cycle_ns,
        "derived_read_row_hit_cycles": read_hit_cycles,
        "derived_read_row_miss_cycles": read_miss_cycles,
        "derived_read_row_conflict_cycles": read_conflict_cycles,
        "derived_write_row_hit_cycles": write_hit_cycles,
        "derived_write_row_miss_cycles": write_miss_cycles,
        "derived_write_row_conflict_cycles": write_conflict_cycles,
        "derived_read_to_write_cycles": read_to_write_cycles,
        "derived_write_to_read_cycles": write_to_read_cycles,
        "derived_activate_to_read_cycles": activate_to_read_cycles,
        "derived_activate_to_write_cycles": activate_to_write_cycles,
        "derived_read_to_read_cycles": same_direction_cycles,
        "derived_write_to_write_cycles": same_direction_cycles,
        "derived_read_to_precharge_cycles": read_to_precharge_cycles,
        "derived_write_to_precharge_cycles": write_to_precharge_cycles,
        "derived_activate_to_precharge_cycles": activate_to_precharge_cycles,
        "derived_precharge_to_activate_cycles": precharge_to_activate_cycles,
        "derived_activate_to_activate_cycles": activate_to_activate_cycles,
        "derived_command_issue_interval_cycles": command_issue_interval_cycles,
        "source_channels_per_stack": source_channels,
        "source_stack_bandwidth_gbps": source_stack_bandwidth_gbps,
        "modeled_stack_count": stack_count,
        "modeled_physical_channels": configured_channels,
        "modeled_bandwidth_gbps": configured_bandwidth_gbps,
        "unified_queue": unified_queue,
        "read_queue_entries_per_channel": transaction_queue_entries,
        "write_buffer_entries_per_channel": transaction_queue_entries,
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
    root, binary, output_dir, dataset, variant, force, profile_path,
    workload_manifest, trace_validator=None,
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
    if trace_validator is not None:
        result["_trace_oracles"] = run_compiled_trace_validator(
            trace_validator, result_path)
    strip_complete_trace_payloads(result)
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


ADMISSION_TRACE_FIELDS = (
    "cycle_delta",
    "terminal_snapshot",
    "admitted_blocks",
    "admitted_read_blocks",
    "admitted_write_blocks",
    "identity_count",
    "admitted_block_identities[sequence,block_offset]",
    "channel_read_occupancy_before[]",
    "channel_read_occupancy_after[]",
    "channel_write_occupancy_before[]",
    "channel_write_occupancy_after[]",
)

COMMAND_TRACE_FIELDS = (
    "cycle_delta",
    "sequence",
    "block_offset",
    "channel",
    "bank",
    "row",
    "command",
)
COMMAND_NAMES = ("PRE", "ACT", "READ", "WRITE")


def decode_varint(payload, offset):
    value = 0
    shift = 0
    while offset < len(payload):
        byte = payload[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if byte & 0x80 == 0:
            return value, offset
        shift += 7
        if shift >= 70:
            raise ValueError("admission trace contains an oversized varint")
    raise ValueError("admission trace ends inside a varint")


def iter_admission_trace(summary):
    if summary.get("representation") != \
            "directional_occupancy_identity_delta_varint_base64_v3":
        raise ValueError("admission evidence is not a reconstructable directional trace")
    if tuple(summary.get("fields", ())) != ADMISSION_TRACE_FIELDS:
        raise ValueError("admission trace field schema does not match the decoder")
    channels = summary.get("channel_count")
    if not isinstance(channels, int) or channels <= 0:
        raise ValueError("admission trace has an invalid channel count")
    for chunk in summary.get("trace_chunks", ()):
        payload = base64.b64decode(chunk["payload_base64"], validate=True)
        offset = 0
        previous_cycle = 0
        count = chunk["event_count"]
        for index in range(count):
            scalar_values = []
            for _ in range(5):
                value, offset = decode_varint(payload, offset)
                scalar_values.append(value)
            cycle = scalar_values[0] if index == 0 else previous_cycle + scalar_values[0]
            previous_cycle = cycle
            identity_count, offset = decode_varint(payload, offset)
            identities = []
            for _ in range(identity_count):
                sequence, offset = decode_varint(payload, offset)
                block_offset, offset = decode_varint(payload, offset)
                identities.append({
                    "sequence": sequence,
                    "block_offset": block_offset,
                })
            arrays = []
            for _ in range(4):
                values = []
                for _ in range(channels):
                    value, offset = decode_varint(payload, offset)
                    values.append(value)
                arrays.append(values)
            yield {
                "cycle": cycle,
                "terminal_snapshot": bool(scalar_values[1]),
                "admitted_blocks": scalar_values[2],
                "admitted_read_blocks": scalar_values[3],
                "admitted_write_blocks": scalar_values[4],
                "admitted_block_identities": identities,
                "channel_read_occupancy_before": arrays[0],
                "channel_read_occupancy_after": arrays[1],
                "channel_write_occupancy_before": arrays[2],
                "channel_write_occupancy_after": arrays[3],
            }
        if offset != len(payload):
            raise ValueError("admission trace chunk has trailing bytes")


def iter_command_trace(summary):
    if summary.get("representation") != "command_delta_varint_base64_v2":
        raise ValueError("command evidence is not a reconstructable full trace")
    if tuple(summary.get("fields", ())) != COMMAND_TRACE_FIELDS:
        raise ValueError("command trace field schema does not match the decoder")
    chunks = summary.get("trace_chunks")
    if not isinstance(chunks, list) or not chunks:
        raise ValueError("command trace does not contain complete event chunks")
    for chunk in chunks:
        payload = base64.b64decode(chunk["payload_base64"], validate=True)
        offset = 0
        previous_cycle = 0
        count = chunk["event_count"]
        if not isinstance(count, int) or count <= 0:
            raise ValueError("command trace chunk has an invalid event count")
        for index in range(count):
            values = []
            for _ in COMMAND_TRACE_FIELDS:
                value, offset = decode_varint(payload, offset)
                values.append(value)
            cycle = values[0] if index == 0 else previous_cycle + values[0]
            previous_cycle = cycle
            command = values[6]
            if command >= len(COMMAND_NAMES):
                raise ValueError("command trace contains an unknown command")
            yield {
                "cycle": cycle,
                "sequence": values[1],
                "block_offset": values[2],
                "channel": values[3],
                "bank": values[4],
                "row": values[5],
                "command": COMMAND_NAMES[command],
                "command_id": command,
            }
        if offset != len(payload):
            raise ValueError("command trace chunk has trailing bytes")


def normalized_histogram(counter):
    return {str(value): counter[value] for value in sorted(counter)}


def validate_transaction_admission(dataset, variant, run):
    architecture = run["architecture"]
    issue_limit = architecture["coordinator_issue_blocks_per_cycle"]
    read_capacity = architecture["hbm_read_queue_entries_per_channel"]
    write_capacity = architecture["hbm_write_buffer_entries_per_channel"]
    channels = architecture["hbm_channels"]
    block_size = architecture["block_size"]
    admitted_total = 0
    admitted_read_total = 0
    admitted_write_total = 0
    expected_total = 0
    expected_read_total = 0
    expected_write_total = 0
    max_total_read_occupancy = 0
    max_total_write_occupancy = 0
    admission_cycles = 0
    checksum_values = []
    for layer in run["layers"]:
        summary = layer["transaction_admission_trace"]
        if summary.get("channel_count") != channels:
            raise ValueError(f"{dataset} {variant} trace channel count differs")
        previous_cycle = None
        previous_read = [0] * channels
        previous_write = [0] * channels
        read_peaks = [0] * channels
        write_peaks = [0] * channels
        event_count = 0
        admission_event_count = 0
        first_cycle = None
        last_cycle = None
        edge_first = []
        edge_small = []
        edge_last = collections.deque(maxlen=16)
        weighted = collections.Counter()
        histograms = {
            name: collections.Counter() for name in (
                "admitted_blocks",
                "admitted_read_blocks",
                "admitted_write_blocks",
                "total_read_occupancy_after",
                "total_write_occupancy_after",
                "max_channel_read_occupancy_after",
                "max_channel_write_occupancy_after",
            )
        }
        actual_max = collections.Counter()
        checksum = 1469598103934665603
        saw_terminal = False
        for trace in iter_admission_trace(summary):
            admitted = trace["admitted_blocks"]
            if len(trace["admitted_block_identities"]) != admitted:
                raise ValueError(
                    f"{dataset} {variant} admission identity count differs"
                )
            if previous_cycle is not None and trace["cycle"] <= previous_cycle:
                raise ValueError(f"{dataset} {variant} occupancy events are not increasing")
            if saw_terminal:
                raise ValueError(f"{dataset} {variant} has events after terminal occupancy")
            if trace["terminal_snapshot"]:
                saw_terminal = True
                if admitted != 0:
                    raise ValueError(f"{dataset} {variant} terminal snapshot admits blocks")
            elif admitted <= 0 or admitted > issue_limit:
                raise ValueError(
                    f"{dataset} {variant} admits {admitted} blocks in one cycle; "
                    f"limit is {issue_limit}"
                )
            arrays = (
                trace["channel_read_occupancy_before"],
                trace["channel_read_occupancy_after"],
                trace["channel_write_occupancy_before"],
                trace["channel_write_occupancy_after"],
            )
            if any(len(values) != channels for values in arrays):
                raise ValueError(f"{dataset} {variant} directional vector width differs")
            read_before, read_after, write_before, write_after = arrays
            inferred_read_dispatch = 0
            inferred_write_dispatch = 0
            derived_read_admission = 0
            derived_write_admission = 0
            for channel in range(channels):
                if read_before[channel] > previous_read[channel] or \
                        write_before[channel] > previous_write[channel]:
                    raise ValueError(
                        f"{dataset} {variant} occupancy grows without admission"
                    )
                if read_after[channel] < read_before[channel] or \
                        write_after[channel] < write_before[channel]:
                    raise ValueError(
                        f"{dataset} {variant} occupancy falls inside admission"
                    )
                inferred_read_dispatch += previous_read[channel] - read_before[channel]
                inferred_write_dispatch += previous_write[channel] - write_before[channel]
                derived_read_admission += read_after[channel] - read_before[channel]
                derived_write_admission += write_after[channel] - write_before[channel]
                if read_after[channel] > read_capacity or \
                        write_after[channel] > write_capacity:
                    raise ValueError(
                        f"{dataset} {variant} exceeds a directional queue capacity"
                    )
                read_peaks[channel] = max(read_peaks[channel], read_after[channel])
                write_peaks[channel] = max(write_peaks[channel], write_after[channel])
            if derived_read_admission != trace["admitted_read_blocks"] or \
                    derived_write_admission != trace["admitted_write_blocks"] or \
                    admitted != derived_read_admission + derived_write_admission:
                raise ValueError(f"{dataset} {variant} per-channel admission delta differs")
            if trace["terminal_snapshot"] and any(read_before + read_after +
                                                    write_before + write_after):
                raise ValueError(f"{dataset} {variant} terminal occupancy is not zero")
            total_read_before = sum(read_before)
            total_read_after = sum(read_after)
            total_write_before = sum(write_before)
            total_write_after = sum(write_after)
            max_channel_read_after = max(read_after, default=0)
            max_channel_write_after = max(write_after, default=0)
            derived = dict(trace)
            for key in (
                    "total_read_occupancy_before", "total_read_occupancy_after",
                    "total_write_occupancy_before", "total_write_occupancy_after",
                    "max_channel_read_occupancy_after",
                    "max_channel_write_occupancy_after"):
                derived.pop(key, None)
            if event_count < 16:
                edge_first.append(derived)
            if event_count < 33:
                edge_small.append(derived)
            edge_last.append(derived)
            scalar_values = {
                "admitted_blocks": admitted,
                "admitted_read_blocks": derived_read_admission,
                "admitted_write_blocks": derived_write_admission,
                "total_read_occupancy_after": total_read_after,
                "total_write_occupancy_after": total_write_after,
                "max_channel_read_occupancy_after": max_channel_read_after,
                "max_channel_write_occupancy_after": max_channel_write_after,
            }
            for name, value in scalar_values.items():
                histograms[name][value] += 1
            weighted["admitted_blocks"] += admitted
            weighted["admitted_read_blocks"] += derived_read_admission
            weighted["admitted_write_blocks"] += derived_write_admission
            weighted["inferred_dispatched_read_blocks"] += inferred_read_dispatch
            weighted["inferred_dispatched_write_blocks"] += inferred_write_dispatch
            weighted["total_read_occupancy_before"] += total_read_before
            weighted["total_read_occupancy_after"] += total_read_after
            weighted["total_write_occupancy_before"] += total_write_before
            weighted["total_write_occupancy_after"] += total_write_after
            actual_max["blocks_admitted_per_cycle"] = max(
                actual_max["blocks_admitted_per_cycle"], admitted)
            actual_max["total_read_occupancy"] = max(
                actual_max["total_read_occupancy"], total_read_after)
            actual_max["total_write_occupancy"] = max(
                actual_max["total_write_occupancy"], total_write_after)
            actual_max["channel_read_occupancy"] = max(
                actual_max["channel_read_occupancy"], max_channel_read_after)
            actual_max["channel_write_occupancy"] = max(
                actual_max["channel_write_occupancy"], max_channel_write_after)
            checksum_inputs = (
                trace["cycle"], int(trace["terminal_snapshot"]), admitted,
                derived_read_admission, derived_write_admission,
            )
            for value in checksum_inputs:
                checksum ^= value
                checksum = (checksum * 1099511628211) & 0xFFFFFFFFFFFFFFFF
            for identity in trace["admitted_block_identities"]:
                for value in (identity["sequence"], identity["block_offset"]):
                    checksum ^= value
                    checksum = (checksum * 1099511628211) & 0xFFFFFFFFFFFFFFFF
            for channel in range(channels):
                for value in (
                        read_before[channel], read_after[channel],
                        write_before[channel], write_after[channel]):
                    checksum ^= value
                    checksum = (checksum * 1099511628211) & 0xFFFFFFFFFFFFFFFF
            previous_read = list(read_after)
            previous_write = list(write_after)
            first_cycle = trace["cycle"] if first_cycle is None else first_cycle
            last_cycle = trace["cycle"]
            previous_cycle = trace["cycle"]
            event_count += 1
            admission_event_count += 0 if trace["terminal_snapshot"] else 1
        if not saw_terminal or any(previous_read) or any(previous_write):
            raise ValueError(f"{dataset} {variant} lacks a zero terminal occupancy snapshot")
        if weighted["inferred_dispatched_read_blocks"] != \
                weighted["admitted_read_blocks"] or \
                weighted["inferred_dispatched_write_blocks"] != \
                weighted["admitted_write_blocks"]:
            raise ValueError(f"{dataset} {variant} inferred dispatch totals differ")
        if event_count != summary["event_count"] or \
                admission_event_count != summary["admission_event_count"] or \
                (first_cycle or 0) != summary["first_cycle"] or \
                (last_cycle or 0) != summary["last_cycle"]:
            raise ValueError(f"{dataset} {variant} compressed trace bounds differ")
        expected_edges = edge_small if event_count <= 32 else edge_first + list(edge_last)
        if expected_edges != summary["edge_samples"]:
            raise ValueError(f"{dataset} {variant} edge samples differ from complete trace")
        if dict(weighted) != summary["weighted_totals"]:
            raise ValueError(f"{dataset} {variant} weighted admission totals differ")
        for name, counter in histograms.items():
            if normalized_histogram(counter) != summary["histograms"][name]:
                raise ValueError(f"{dataset} {variant} {name} histogram differs")
        if dict(actual_max) != summary["actual_max"]:
            raise ValueError(f"{dataset} {variant} actual admission maxima differ")
        if f"{checksum:016x}" != summary["trace_checksum_fnv1a64"]:
            raise ValueError(f"{dataset} {variant} admission checksum differs")
        if read_peaks != summary["max_channel_read_queue_occupancy"] or \
                write_peaks != summary["max_channel_write_buffer_occupancy"]:
            raise ValueError(f"{dataset} {variant} independently derived peak vectors differ")
        admitted_total += weighted["admitted_blocks"]
        admitted_read_total += weighted["admitted_read_blocks"]
        admitted_write_total += weighted["admitted_write_blocks"]
        admission_cycles += admission_event_count
        max_total_read_occupancy = max(
            max_total_read_occupancy, actual_max["total_read_occupancy"])
        max_total_write_occupancy = max(
            max_total_write_occupancy, actual_max["total_write_occupancy"])
        checksum_values.append(summary["trace_checksum_fnv1a64"])
        for request in layer["memory_requests"]:
            blocks = math.ceil(request["bytes"] / block_size)
            expected_total += blocks
            if request["request_class"] in {"output", "intermediate_write"}:
                expected_write_total += blocks
            else:
                expected_read_total += blocks
            if not (
                request["first_admission_cycle"] >= request["enqueue_cycle"]
                and request["last_admission_cycle"] >= request["first_admission_cycle"]
                and request["first_issue_cycle"] >= request["first_admission_cycle"]
            ):
                raise ValueError(f"{dataset} {variant} request admission violates causality")
    if (admitted_total, admitted_read_total, admitted_write_total) != (
            expected_total, expected_read_total, expected_write_total):
        raise ValueError(
            f"{dataset} {variant} admission trace covers "
            f"{admitted_total}/{admitted_read_total}/{admitted_write_total} blocks, "
            f"expected {expected_total}/{expected_read_total}/{expected_write_total}"
        )
    return {
        "admitted_blocks": admitted_total,
        "admitted_read_blocks": admitted_read_total,
        "admitted_write_blocks": admitted_write_total,
        "admission_cycles": admission_cycles,
        "max_blocks_admitted_per_cycle": max(
            layer["transaction_admission_trace"]["actual_max"][
                "blocks_admitted_per_cycle"]
            for layer in run["layers"]
        ),
        "issue_limit_blocks_per_cycle": issue_limit,
        "max_total_read_queue_occupancy": max_total_read_occupancy,
        "max_total_write_buffer_occupancy": max_total_write_occupancy,
        "total_read_queue_capacity": channels * read_capacity,
        "total_write_buffer_capacity": channels * write_capacity,
        "trace_checksums": checksum_values,
        "occupancy_reconstruction": (
            "per-channel before/after admission snapshots plus request-block identity"
        ),
    }


def validate_command_trace(dataset, variant, run):
    architecture = run["architecture"]
    interval = architecture["hbm_command_issue_interval_cycles"]
    channels = architecture["hbm_channels"]
    banks_per_channel = architecture["hbm_banks_per_channel"]
    event_count = 0
    checksums = []
    for layer in run["layers"]:
        summary = layer["command_trace"]
        expected = (
            layer["precharge_commands"] + layer["activate_commands"]
            + layer["read_commands"] + layer["write_commands"]
        )
        channel_state = [
            {"last_command": None, "next_read": 0, "next_write": 0}
            for _ in range(channels)
        ]
        bank_state = [
            {
                "open_row": None,
                "next_precharge": 0,
                "next_activate": 0,
                "next_read": 0,
                "next_write": 0,
            }
            for _ in range(channels * banks_per_channel)
        ]
        counts = collections.Counter()
        checksum = 1469598103934665603
        first = []
        last = collections.deque(maxlen=32)
        previous_cycle = None
        decoded = 0
        for event in iter_command_trace(summary):
            cycle = event["cycle"]
            channel = event["channel"]
            bank = event["bank"]
            row = event["row"]
            command = event["command"]
            if channel >= channels or bank >= banks_per_channel:
                raise ValueError(f"{dataset} {variant} command address is out of range")
            if previous_cycle is not None and cycle < previous_cycle:
                raise ValueError(f"{dataset} {variant} command trace is not time ordered")
            previous_cycle = cycle
            channel_timing = channel_state[channel]
            last_command = channel_timing["last_command"]
            if last_command is not None and cycle < last_command + interval:
                raise ValueError(
                    f"{dataset} {variant} commands overlap on channel {channel}"
                )
            channel_timing["last_command"] = cycle
            timing = bank_state[channel * banks_per_channel + bank]
            if command == "PRE":
                if timing["open_row"] != row or cycle < timing["next_precharge"]:
                    raise ValueError(
                        f"{dataset} {variant} PRE violates row recovery on "
                        f"channel {channel} bank {bank}"
                    )
                timing["open_row"] = None
                timing["next_activate"] = max(
                    timing["next_activate"],
                    cycle + architecture["hbm_precharge_to_activate_cycles"],
                )
            elif command == "ACT":
                if timing["open_row"] is not None or cycle < timing["next_activate"]:
                    raise ValueError(
                        f"{dataset} {variant} ACT violates bank recovery on "
                        f"channel {channel} bank {bank}"
                    )
                timing["open_row"] = row
                timing["next_read"] = max(
                    timing["next_read"],
                    cycle + architecture["hbm_activate_to_read_cycles"],
                )
                timing["next_write"] = max(
                    timing["next_write"],
                    cycle + architecture["hbm_activate_to_write_cycles"],
                )
                timing["next_precharge"] = max(
                    timing["next_precharge"],
                    cycle + architecture["hbm_activate_to_precharge_cycles"],
                )
                timing["next_activate"] = max(
                    timing["next_activate"],
                    cycle + architecture["hbm_activate_to_activate_cycles"],
                )
            elif command == "READ":
                if timing["open_row"] != row or cycle < timing["next_read"] or \
                        cycle < channel_timing["next_read"]:
                    raise ValueError(
                        f"{dataset} {variant} READ violates tRCD/tCCD/turnaround on "
                        f"channel {channel} bank {bank}"
                    )
                channel_timing["next_read"] = max(
                    channel_timing["next_read"],
                    cycle + architecture["hbm_read_to_read_cycles"],
                )
                channel_timing["next_write"] = max(
                    channel_timing["next_write"],
                    cycle + architecture["hbm_read_to_write_cycles"],
                )
                timing["next_precharge"] = max(
                    timing["next_precharge"],
                    cycle + architecture["hbm_read_to_precharge_cycles"],
                )
            elif command == "WRITE":
                if timing["open_row"] != row or cycle < timing["next_write"] or \
                        cycle < channel_timing["next_write"]:
                    raise ValueError(
                        f"{dataset} {variant} WRITE violates tRCD/tCCD/turnaround on "
                        f"channel {channel} bank {bank}"
                    )
                channel_timing["next_write"] = max(
                    channel_timing["next_write"],
                    cycle + architecture["hbm_write_to_write_cycles"],
                )
                channel_timing["next_read"] = max(
                    channel_timing["next_read"],
                    cycle + architecture["hbm_write_to_read_cycles"],
                )
                timing["next_precharge"] = max(
                    timing["next_precharge"],
                    cycle + architecture["hbm_write_to_precharge_cycles"],
                )
            counts[command] += 1
            for value in (
                    cycle, event["sequence"], event["block_offset"], channel,
                    bank, row, event["command_id"]):
                checksum ^= value
                checksum = (checksum * 1099511628211) & 0xFFFFFFFFFFFFFFFF
            sample = {key: value for key, value in event.items()
                      if key != "command_id"}
            if decoded < 32:
                first.append(sample)
            else:
                last.append(sample)
            decoded += 1
        if decoded != expected or decoded != summary.get("event_count"):
            raise ValueError(f"{dataset} {variant} command event count differs")
        expected_counts = {
            "PRE": layer["precharge_commands"],
            "ACT": layer["activate_commands"],
            "READ": layer["read_commands"],
            "WRITE": layer["write_commands"],
        }
        if any(counts[name] != expected_counts[name]
               for name in COMMAND_NAMES):
            raise ValueError(f"{dataset} {variant} command type totals differ")
        expected_samples = first + list(last)
        if expected_samples != summary.get("samples"):
            raise ValueError(
                f"{dataset} {variant} command samples differ from complete trace"
            )
        if f"{checksum:016x}" != summary.get("checksum_fnv1a64"):
            raise ValueError(f"{dataset} {variant} command checksum differs")
        if summary.get("command_lane_violations") != 0:
            raise ValueError(f"{dataset} {variant} reports a command-lane violation")
        event_count += decoded
        checksums.append(summary["checksum_fnv1a64"])
    return {
        "event_count": event_count,
        "checksums": checksums,
        "command_issue_interval_cycles": interval,
        "command_lane_violations": 0,
        "trace_representation": "command_delta_varint_base64_v2",
        "independently_reconstructed": True,
    }

def directional_memory_evidence(run):
    evidence = []
    for layer in run["layers"]:
        reads = sorted(
            (trace["first_issue_cycle"], trace["completion_cycle"])
            for trace in layer["memory_requests"]
            if trace["request_class"] not in {"output", "intermediate_write"}
        )
        read_starts = [start for start, _ in reads]
        prefix_completion = []
        for _, completion in reads:
            prefix_completion.append(max(
                completion,
                prefix_completion[-1] if prefix_completion else 0,
            ))
        write_requests = collections.Counter()
        overlapping_writes = collections.Counter()
        for trace in layer["memory_requests"]:
            request_class = trace["request_class"]
            if request_class not in {"output", "intermediate_write"}:
                continue
            write_requests[request_class] += 1
            index = bisect.bisect_left(read_starts, trace["completion_cycle"]) - 1
            if index >= 0 and prefix_completion[index] > trace["first_issue_cycle"]:
                overlapping_writes[request_class] += 1
        admission = layer["transaction_admission_trace"]
        command_trace = layer["command_trace"]
        row_transition_samples = [
            {
                "sequence": trace["sequence"],
                "request_class": trace["request_class"],
                "precharge_commands": trace["precharge_commands"],
                "activate_commands": trace["activate_commands"],
                "first_precharge_cycle": trace["first_precharge_cycle"],
                "last_precharge_cycle": trace["last_precharge_cycle"],
                "first_activate_cycle": trace["first_activate_cycle"],
                "last_activate_cycle": trace["last_activate_cycle"],
                "first_issue_cycle": trace["first_issue_cycle"],
            }
            for trace in layer["memory_requests"]
            if trace["precharge_commands"] > 0
        ][:16]
        evidence.append({
            "layer": layer["layer"],
            "read_requests": len(reads),
            "write_requests": dict(write_requests),
            "write_requests_overlapping_read_service": dict(overlapping_writes),
            "read_to_write_switches": layer["read_to_write_switches"],
            "write_to_read_switches": layer["write_to_read_switches"],
            "direction_switch_stall_cycles": layer["direction_switch_stall_cycles"],
            "precharge_commands": layer["precharge_commands"],
            "activate_commands": layer["activate_commands"],
            "read_commands": layer["read_commands"],
            "write_commands": layer["write_commands"],
            "row_transition_request_samples": row_transition_samples,
            "max_channel_read_queue_occupancy":
                admission["max_channel_read_queue_occupancy"],
            "max_channel_write_buffer_occupancy":
                admission["max_channel_write_buffer_occupancy"],
            "trace_checksum_fnv1a64": admission["trace_checksum_fnv1a64"],
            "command_trace_event_count": command_trace["event_count"],
            "command_trace_checksum_fnv1a64": command_trace["checksum_fnv1a64"],
            "command_lane_violations": command_trace["command_lane_violations"],
            "command_trace_samples": command_trace["samples"],
        })
    return {
        "read_row_cycles": [
            run["architecture"]["hbm_read_row_hit_cycles"],
            run["architecture"]["hbm_read_row_miss_cycles"],
            run["architecture"]["hbm_read_row_conflict_cycles"],
        ],
        "write_row_cycles": [
            run["architecture"]["hbm_write_row_hit_cycles"],
            run["architecture"]["hbm_write_row_miss_cycles"],
            run["architecture"]["hbm_write_row_conflict_cycles"],
        ],
        "direction_switch_cycles": [
            run["architecture"]["hbm_read_to_write_cycles"],
            run["architecture"]["hbm_write_to_read_cycles"],
        ],
        "command_recovery_cycles": {
            "activate_to_read": run["architecture"]["hbm_activate_to_read_cycles"],
            "activate_to_write": run["architecture"]["hbm_activate_to_write_cycles"],
            "read_to_read": run["architecture"]["hbm_read_to_read_cycles"],
            "write_to_write": run["architecture"]["hbm_write_to_write_cycles"],
            "read_to_precharge":
                run["architecture"]["hbm_read_to_precharge_cycles"],
            "write_to_precharge":
                run["architecture"]["hbm_write_to_precharge_cycles"],
            "activate_to_precharge":
                run["architecture"]["hbm_activate_to_precharge_cycles"],
            "precharge_to_activate":
                run["architecture"]["hbm_precharge_to_activate_cycles"],
            "activate_to_activate":
                run["architecture"]["hbm_activate_to_activate_cycles"],
        },
        "layers": evidence,
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
        "hbm_read_queue_entries_per_channel":
            architecture["hbm_read_queue_entries_per_channel"],
        "hbm_write_buffer_entries_per_channel":
            architecture["hbm_write_buffer_entries_per_channel"],
        "hbm_command_queue_entries_per_bank":
            architecture["hbm_command_queue_entries_per_bank"],
        "hbm_read_row_hit_cycles": architecture["hbm_read_row_hit_cycles"],
        "hbm_read_row_miss_cycles": architecture["hbm_read_row_miss_cycles"],
        "hbm_read_row_conflict_cycles": architecture["hbm_read_row_conflict_cycles"],
        "hbm_write_row_hit_cycles": architecture["hbm_write_row_hit_cycles"],
        "hbm_write_row_miss_cycles": architecture["hbm_write_row_miss_cycles"],
        "hbm_write_row_conflict_cycles": architecture["hbm_write_row_conflict_cycles"],
        "hbm_activate_to_read_cycles": architecture["hbm_activate_to_read_cycles"],
        "hbm_activate_to_write_cycles": architecture["hbm_activate_to_write_cycles"],
        "hbm_read_to_read_cycles": architecture["hbm_read_to_read_cycles"],
        "hbm_write_to_write_cycles": architecture["hbm_write_to_write_cycles"],
        "hbm_read_to_write_cycles": architecture["hbm_read_to_write_cycles"],
        "hbm_write_to_read_cycles": architecture["hbm_write_to_read_cycles"],
        "hbm_read_to_precharge_cycles": architecture["hbm_read_to_precharge_cycles"],
        "hbm_write_to_precharge_cycles": architecture["hbm_write_to_precharge_cycles"],
        "hbm_activate_to_precharge_cycles":
            architecture["hbm_activate_to_precharge_cycles"],
        "hbm_precharge_to_activate_cycles":
            architecture["hbm_precharge_to_activate_cycles"],
        "hbm_activate_to_activate_cycles":
            architecture["hbm_activate_to_activate_cycles"],
        "hbm_command_issue_interval_cycles":
            architecture["hbm_command_issue_interval_cycles"],
        "hbm_channels": architecture["hbm_channels"],
        "coordinator_issue_blocks_per_cycle":
            architecture["coordinator_issue_blocks_per_cycle"],
        "coordinator_fifo_active_windows":
            architecture["coordinator_fifo_active_windows"],
        "coordinator_fifo_window_blocks":
            architecture["coordinator_fifo_window_blocks"],
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
    trace_validator = (
        Path(args.trace_validator) if args.trace_validator
        else binary.with_name("hygcn_trace_validator")
    )
    if not trace_validator.is_absolute():
        trace_validator = root / trace_validator
    if not trace_validator.is_file():
        raise ValueError(
            f"compiled trace validator is missing: {trace_validator}"
        )
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
                trace_validator,
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
            name: result["_trace_oracles"]["transaction_admission_oracle"]
            for name, result in runs.items()
        }
        command_oracles = {
            name: result["_trace_oracles"]["command_trace_oracle"]
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
            "command_trace_oracles": command_oracles,
            "directional_memory_evidence": {
                name: directional_memory_evidence(result)
                for name, result in runs.items()
            },
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
        "trace_validator": {
            "path": str(trace_validator),
            "sha256": sha256_digest(trace_validator),
            "implementation": "standalone independent C++ replay",
        },
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
