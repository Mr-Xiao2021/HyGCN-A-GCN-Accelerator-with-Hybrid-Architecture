#!/usr/bin/env python3
import argparse
import concurrent.futures
import configparser
import json
import subprocess
import sys
from pathlib import Path

import paper_benchmark


VARIANTS = ("optimized", "mapping_only", "coordination_baseline")
SCENARIOS = {
    "baseline": {},
    "neighbor_delay_0": {("model", "neighbor_index_ready_cycles"): 0},
    "neighbor_delay_4": {("model", "neighbor_index_ready_cycles"): 4},
    "dram_timing_fast": {
        ("memory", "hbm_read_row_hit_cycles"): 10,
        ("memory", "hbm_read_row_miss_cycles"): 24,
        ("memory", "hbm_read_row_conflict_cycles"): 38,
        ("memory", "hbm_write_row_hit_cycles"): 2,
        ("memory", "hbm_write_row_miss_cycles"): 14,
        ("memory", "hbm_write_row_conflict_cycles"): 28,
        ("memory", "hbm_read_to_write_cycles"): 14,
        ("memory", "hbm_write_to_read_cycles"): 12,
    },
    "dram_timing_slow": {
        ("memory", "hbm_read_row_hit_cycles"): 18,
        ("memory", "hbm_read_row_miss_cycles"): 36,
        ("memory", "hbm_read_row_conflict_cycles"): 54,
        ("memory", "hbm_write_row_hit_cycles"): 6,
        ("memory", "hbm_write_row_miss_cycles"): 22,
        ("memory", "hbm_write_row_conflict_cycles"): 38,
        ("memory", "hbm_read_to_write_cycles"): 22,
        ("memory", "hbm_write_to_read_cycles"): 20,
    },
    "legacy_12_28_timing": {
        ("memory", "hbm_read_row_hit_cycles"): 12,
        ("memory", "hbm_read_row_miss_cycles"): 28,
        ("memory", "hbm_read_row_conflict_cycles"): 42,
    },
    "write_timing_slow": {
        ("memory", "hbm_write_row_hit_cycles"): 6,
        ("memory", "hbm_write_row_miss_cycles"): 22,
        ("memory", "hbm_write_row_conflict_cycles"): 38,
    },
    "direction_switching_slow": {
        ("memory", "hbm_read_to_write_cycles"): 22,
        ("memory", "hbm_write_to_read_cycles"): 20,
    },
    "row_first_interleave_2": {("model", "row_first_bank_interleave"): 2},
}


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Sweep request-release, DRAM timing counterfactuals, and mapping parameters"
        )
    )
    parser.add_argument("--binary", default="build/hygcntest")
    parser.add_argument("--profile", default="configs/HYGCN_PAPER.ini")
    parser.add_argument("--workloads", default="configs/paper_workloads.json")
    parser.add_argument("--output-dir", default="res/model-sensitivity")
    parser.add_argument("--datasets", nargs="+", default=["cora", "citeseer", "pubmed"])
    parser.add_argument("--jobs", type=int, default=4)
    return parser.parse_args()


def resolve(root, value):
    path = Path(value)
    return path if path.is_absolute() else root / path


def display_path(root, path):
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def write_profile(source, destination, changes):
    parser = configparser.ConfigParser()
    with source.open(encoding="utf-8") as stream:
        parser.read_file(stream)
    for (section, option), value in changes.items():
        parser[section][option] = str(value)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("w", encoding="utf-8", newline="\n") as stream:
        parser.write(stream)


def run_variant(root, binary, profile, output_dir, dataset, variant, workloads):
    return paper_benchmark.run_variant(
        root,
        binary,
        output_dir,
        dataset,
        variant,
        True,
        profile,
        workloads,
    )


def metrics(runs):
    optimized = paper_benchmark.summarize_result(runs["optimized"])
    mapping_only = paper_benchmark.summarize_result(runs["mapping_only"])
    baseline = paper_benchmark.summarize_result(runs["coordination_baseline"])
    return {
        "coordination_speedup": paper_benchmark.ratio(
            baseline["total_cycles"], optimized["total_cycles"], "coordination_speedup"
        ),
        "coordination_bandwidth_gain": paper_benchmark.ratio(
            optimized["bandwidth_utilization"],
            baseline["bandwidth_utilization"],
            "coordination_bandwidth_gain",
        ),
        "priority_incremental_speedup": paper_benchmark.ratio(
            mapping_only["total_cycles"],
            optimized["total_cycles"],
            "priority_incremental_speedup",
        ),
        "priority_incremental_bandwidth_gain": paper_benchmark.ratio(
            optimized["bandwidth_utilization"],
            mapping_only["bandwidth_utilization"],
            "priority_incremental_bandwidth_gain",
        ),
        "priority_incremental_row_hit_ratio": paper_benchmark.ratio(
            optimized["row_hit_rate"],
            mapping_only["row_hit_rate"],
            "priority_incremental_row_hit_ratio",
        ),
    }


def main():
    args = parse_args()
    if args.jobs <= 0:
        raise ValueError("--jobs must be positive")
    root = Path(__file__).resolve().parents[1]
    binary = resolve(root, args.binary)
    source_profile = resolve(root, args.profile)
    workload_path = resolve(root, args.workloads)
    output_dir = resolve(root, args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    workloads = paper_benchmark.load_json(workload_path)
    dram_timing_basis = paper_benchmark.derive_dramsim3_timing(
        root, source_profile, workloads
    )

    profiles = {}
    for name, changes in SCENARIOS.items():
        scenario_dir = output_dir / name
        profile = scenario_dir / "HYGCN_PAPER.ini"
        write_profile(source_profile, profile, changes)
        profiles[name] = (scenario_dir, profile)

    runs = {
        name: {dataset: {} for dataset in args.datasets}
        for name in SCENARIOS
    }
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {}
        for name, (scenario_dir, profile) in profiles.items():
            for dataset in args.datasets:
                for variant in VARIANTS:
                    future = executor.submit(
                        run_variant,
                        root,
                        binary,
                        profile,
                        scenario_dir,
                        dataset,
                        variant,
                        workloads,
                    )
                    futures[future] = (name, dataset, variant)
        for future in concurrent.futures.as_completed(futures):
            name, dataset, variant = futures[future]
            runs[name][dataset][variant] = future.result()

    report = {
        "schema_version": 1,
        "binary": display_path(root, binary),
        "binary_sha256": paper_benchmark.sha256_digest(binary),
        "base_profile": display_path(root, source_profile),
        "base_profile_sha256": paper_benchmark.sha256_digest(source_profile),
        "workload_manifest": display_path(root, workload_path),
        "workload_manifest_sha256": paper_benchmark.sha256_digest(workload_path),
        "datasets": args.datasets,
        "selection_policy": (
            "The required baseline is derived from bundled DRAMSim3 read, write, and "
            "direction-switch timing. Alternative values are diagnostic sensitivity "
            "points and are not selected using paper targets."
        ),
        "required_dram_timing_basis": dram_timing_basis,
        "scenarios": {},
    }
    for name, changes in SCENARIOS.items():
        scenario_runs = runs[name]
        per_dataset = {
            dataset: metrics(scenario_runs[dataset])
            for dataset in args.datasets
        }
        report["scenarios"][name] = {
            "acceptance_role": "required_external_baseline" if name == "baseline" else "diagnostic",
            "changes": {
                f"{section}.{option}": value
                for (section, option), value in changes.items()
            },
            "profile_sha256": paper_benchmark.sha256_digest(profiles[name][1]),
            "per_dataset": per_dataset,
            "aggregate": {
                metric: paper_benchmark.mean([
                    values[metric] for values in per_dataset.values()
                ])
                for metric in next(iter(per_dataset.values()))
            },
        }

    report_path = output_dir / "model_sensitivity.json"
    with report_path.open("w", encoding="utf-8", newline="\n") as stream:
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
