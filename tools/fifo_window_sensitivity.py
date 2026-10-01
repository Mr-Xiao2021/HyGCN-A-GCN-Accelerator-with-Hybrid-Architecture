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


def parse_args():
    parser = argparse.ArgumentParser(
        description="Sweep the diagnostic FIFO active-window throttle"
    )
    parser.add_argument("--binary", default="build/hygcntest")
    parser.add_argument("--profile", default="configs/HYGCN_PAPER.ini")
    parser.add_argument("--reference", default="configs/paper_metrics.json")
    parser.add_argument("--workloads", default="configs/paper_workloads.json")
    parser.add_argument("--output-dir", default="res/fifo-window-sensitivity")
    parser.add_argument("--limits", nargs="+", type=int)
    parser.add_argument(
        "--datasets", nargs="+", default=["cora", "citeseer", "pubmed"]
    )
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


def write_profile(source, destination, limit):
    parser = configparser.ConfigParser()
    with source.open(encoding="utf-8") as stream:
        parser.read_file(stream)
    parser["model"]["coordinator_fifo_active_windows"] = str(limit)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("w", encoding="utf-8", newline="\n") as stream:
        parser.write(stream)


def metrics(runs):
    optimized = paper_benchmark.summarize_result(runs["optimized"])
    mapping_only = paper_benchmark.summarize_result(runs["mapping_only"])
    baseline = paper_benchmark.summarize_result(runs["coordination_baseline"])
    return {
        "coordination_speedup": paper_benchmark.ratio(
            baseline["total_cycles"], optimized["total_cycles"],
            "coordination_speedup",
        ),
        "coordination_bandwidth_gain": paper_benchmark.ratio(
            optimized["bandwidth_utilization"], baseline["bandwidth_utilization"],
            "coordination_bandwidth_gain",
        ),
        "priority_incremental_speedup": paper_benchmark.ratio(
            mapping_only["total_cycles"], optimized["total_cycles"],
            "priority_incremental_speedup",
        ),
        "priority_incremental_bandwidth_gain": paper_benchmark.ratio(
            optimized["bandwidth_utilization"],
            mapping_only["bandwidth_utilization"],
            "priority_incremental_bandwidth_gain",
        ),
        "priority_incremental_row_hit_ratio": paper_benchmark.ratio(
            optimized["row_hit_rate"], mapping_only["row_hit_rate"],
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
    reference_path = resolve(root, args.reference)
    workload_path = resolve(root, args.workloads)
    output_dir = resolve(root, args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    reference = paper_benchmark.load_json(reference_path)
    workloads = paper_benchmark.load_json(workload_path)
    limits = args.limits or workloads["memory_ablation"][
        "fifo_window_sensitivity"]
    if any(limit < 0 for limit in limits) or 0 not in limits:
        raise ValueError("limits must be non-negative and include cap-free value 0")

    profiles = {}
    for limit in limits:
        label = "unbounded" if limit == 0 else str(limit)
        scenario_dir = output_dir / label
        profile = scenario_dir / "HYGCN_PAPER.ini"
        write_profile(source_profile, profile, limit)
        profiles[limit] = (scenario_dir, profile)

    runs = {
        limit: {dataset: {} for dataset in args.datasets}
        for limit in limits
    }
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {}
        for limit, (scenario_dir, profile) in profiles.items():
            for dataset in args.datasets:
                for variant in VARIANTS:
                    future = executor.submit(
                        paper_benchmark.run_variant,
                        root, binary, scenario_dir, dataset, variant, True,
                        profile, workloads,
                    )
                    futures[future] = (limit, dataset, variant)
        for future in concurrent.futures.as_completed(futures):
            limit, dataset, variant = futures[future]
            runs[limit][dataset][variant] = future.result()

    report = {
        "schema_version": 1,
        "binary": display_path(root, binary),
        "binary_sha256": paper_benchmark.sha256_digest(binary),
        "base_profile": display_path(root, source_profile),
        "base_profile_sha256": paper_benchmark.sha256_digest(source_profile),
        "workload_manifest": display_path(root, workload_path),
        "workload_manifest_sha256": paper_benchmark.sha256_digest(workload_path),
        "datasets": args.datasets,
        "required_policy": {
            "fifo_active_windows": 0,
            "basis": "The paper specifies four request sources, not four unfinished address windows; finite limits are diagnostic calibrated policies only.",
        },
        "scenarios": {},
    }
    speed_target = reference["metrics"]["coordination_speedup"]["reference"]
    bandwidth_target = reference["metrics"]["coordination_bandwidth_gain"][
        "reference"]
    tolerance = reference["tolerance"]
    for limit in limits:
        per_dataset = {
            dataset: metrics(runs[limit][dataset]) for dataset in args.datasets
        }
        aggregate = {
            metric: paper_benchmark.mean([
                values[metric] for values in per_dataset.values()
            ])
            for metric in next(iter(per_dataset.values()))
        }
        aggregate["coordination_speedup_relative_error"] = abs(
            aggregate["coordination_speedup"] - speed_target) / speed_target
        aggregate["coordination_bandwidth_relative_error"] = abs(
            aggregate["coordination_bandwidth_gain"] - bandwidth_target
        ) / bandwidth_target
        aggregate["fig17_pass"] = (
            aggregate["coordination_speedup_relative_error"] <= tolerance
            and aggregate["coordination_bandwidth_relative_error"] <= tolerance
        )
        report["scenarios"][str(limit)] = {
            "label": "unbounded" if limit == 0 else f"limit-{limit}",
            "acceptance_role": "required_cap_free" if limit == 0 else "diagnostic",
            "profile_sha256": paper_benchmark.sha256_digest(profiles[limit][1]),
            "per_dataset": per_dataset,
            "aggregate": aggregate,
        }

    report_path = output_dir / "fifo_window_sensitivity.json"
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
