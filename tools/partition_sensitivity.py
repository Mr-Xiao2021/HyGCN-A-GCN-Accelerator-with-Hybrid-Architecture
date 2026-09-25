#!/usr/bin/env python3
import argparse
import concurrent.futures
import configparser
import json
import subprocess
import sys
from pathlib import Path

import paper_benchmark


VARIANTS = (
    "optimized",
    "sparsity_optimized",
    "sparsity_baseline",
    "pipeline_baseline",
)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Sweep the fixed aggregation shard scheduler capacity"
    )
    parser.add_argument("--binary", default="build/hygcntest")
    parser.add_argument("--profile", default="configs/HYGCN_PAPER.ini")
    parser.add_argument("--reference", default="configs/paper_metrics.json")
    parser.add_argument("--output-dir", default="res/partition-sensitivity")
    parser.add_argument("--capacities-mib", nargs="+", type=int, default=[4, 5, 6])
    parser.add_argument("--datasets", nargs="+", default=["cora", "citeseer", "pubmed"])
    parser.add_argument("--jobs", type=int, default=4)
    return parser.parse_args()


def resolve(root, value):
    path = Path(value)
    return path if path.is_absolute() else root / path


def write_profile(source, destination, capacity_bytes):
    parser = configparser.ConfigParser()
    with source.open(encoding="utf-8") as stream:
        parser.read_file(stream)
    parser["model"]["aggregation_shard_capacity_bytes"] = str(capacity_bytes)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("w", encoding="utf-8") as stream:
        parser.write(stream)


def run_variant(root, binary, profile, output_dir, dataset, variant):
    flags = paper_benchmark.VARIANTS[variant]
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
        "--profile-path", str(profile),
        "--quiet",
    ]
    subprocess.run(command, cwd=root, check=True)
    return paper_benchmark.load_json(
        output_dir / paper_benchmark.result_name(dataset, variant)
    )


def relative_error(measured, reference):
    return abs(measured - reference) / abs(reference)


def summarize(reference, dataset, runs):
    optimized = runs["optimized"]["summary"]
    sparse_optimized = runs["sparsity_optimized"]["summary"]
    sparse_baseline = runs["sparsity_baseline"]["summary"]
    pipeline_baseline = runs["pipeline_baseline"]["summary"]
    metrics = {
        "sparsity_speedup": paper_benchmark.ratio(
            sparse_baseline["total_aggregation_cycles"],
            sparse_optimized["total_aggregation_cycles"],
            "sparsity_speedup",
        ),
        "sparsity_ae_dram_ratio": paper_benchmark.ratio(
            sparse_optimized["total_aggregation_dram_bytes"],
            sparse_baseline["total_aggregation_dram_bytes"],
            "sparsity_ae_dram_ratio",
        ),
        "pipeline_speedup": paper_benchmark.ratio(
            pipeline_baseline["total_cycles"],
            optimized["total_cycles"],
            "pipeline_speedup",
        ),
        "pipeline_dram_ratio": paper_benchmark.ratio(
            optimized["total_dram_bytes"],
            pipeline_baseline["total_dram_bytes"],
            "pipeline_dram_ratio",
        ),
    }
    selected = {}
    for name in (
        "sparsity_speedup",
        "sparsity_ae_dram_ratio",
        "pipeline_speedup",
        "pipeline_dram_ratio",
    ):
        target = reference["metrics"][name]["reference"][dataset]
        measured = metrics[name]
        error = relative_error(measured, target)
        selected[name] = {
            "reference": target,
            "measured": measured,
            "relative_error": error,
            "pass": error <= reference["tolerance"],
        }
    return selected


def main():
    args = parse_args()
    if args.jobs <= 0 or any(value <= 0 for value in args.capacities_mib):
        raise ValueError("jobs and capacities must be positive")
    root = Path(__file__).resolve().parents[1]
    binary = resolve(root, args.binary)
    source_profile = resolve(root, args.profile)
    reference_path = resolve(root, args.reference)
    output_dir = resolve(root, args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    reference = paper_benchmark.load_json(reference_path)

    report = {
        "schema_version": 1,
        "base_profile": str(source_profile.relative_to(root)),
        "base_profile_sha256": paper_benchmark.sha256_digest(source_profile),
        "binary": str(binary.relative_to(root)),
        "binary_sha256": paper_benchmark.sha256_digest(binary),
        "datasets": args.datasets,
        "fixed_parameters": {
            "physical_aggregation_buffer_bytes": 16 * 1024 * 1024,
            "aggregation_ping_pong_regions": 2,
            "changed_parameter": "aggregation_shard_capacity_bytes",
        },
        "capacities": {},
    }
    profiles = {}
    for capacity_mib in args.capacities_mib:
        capacity_bytes = capacity_mib * 1024 * 1024
        capacity_dir = output_dir / f"{capacity_mib}mib"
        profile = capacity_dir / "HYGCN_PAPER.ini"
        write_profile(source_profile, profile, capacity_bytes)
        profiles[capacity_mib] = (capacity_bytes, capacity_dir, profile)

    runs_by_capacity = {
        capacity_mib: {dataset: {} for dataset in args.datasets}
        for capacity_mib in args.capacities_mib
    }
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {}
        for capacity_mib, (_, capacity_dir, profile) in profiles.items():
            for dataset in args.datasets:
                for variant in VARIANTS:
                    future = executor.submit(
                        run_variant,
                        root,
                        binary,
                        profile,
                        capacity_dir,
                        dataset,
                        variant,
                    )
                    futures[future] = (capacity_mib, dataset, variant)
        for future in concurrent.futures.as_completed(futures):
            capacity_mib, dataset, variant = futures[future]
            runs_by_capacity[capacity_mib][dataset][variant] = future.result()

    for capacity_mib in args.capacities_mib:
        capacity_bytes, _, _ = profiles[capacity_mib]
        entries = {}
        source_commits = set()
        binary_digests = set()
        for dataset in args.datasets:
            runs = runs_by_capacity[capacity_mib][dataset]
            source_commits.update(run["manifest"]["git_commit"] for run in runs.values())
            binary_digests.update(run["manifest"]["binary_digest"] for run in runs.values())
            architectures = [run["architecture"] for run in runs.values()]
            if any(item != architectures[0] for item in architectures[1:]):
                raise ValueError(f"{dataset} sensitivity variants changed architecture")
            architecture = architectures[0]
            if architecture["aggregation_buffer_bytes"] != 16 * 1024 * 1024:
                raise ValueError("sensitivity changed physical aggregation buffer capacity")
            if architecture["aggregation_shard_capacity_bytes"] != capacity_bytes:
                raise ValueError("sensitivity profile did not apply the requested shard cap")
            entries[dataset] = summarize(reference, dataset, runs)
        if len(source_commits) != 1 or len(binary_digests) != 1:
            raise ValueError("sensitivity variants do not share one source and binary")
        _, _, profile = profiles[capacity_mib]
        report["capacities"][str(capacity_mib)] = {
            "capacity_bytes": capacity_bytes,
            "profile_sha256": paper_benchmark.sha256_digest(profile),
            "source_git_commit": next(iter(source_commits)),
            "binary_digest": next(iter(binary_digests)),
            "per_dataset": entries,
            "all_required_pass": all(
                metric["pass"]
                for dataset in entries.values()
                for metric in dataset.values()
            ),
        }

    report_path = output_dir / "partition_sensitivity.json"
    with report_path.open("w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(report_path)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
