#!/usr/bin/env python3
import argparse
import json
import math
import subprocess
import sys
from pathlib import Path


VARIANTS = {
    "optimized": {
        "pipeline": "latency-aware",
        "combination": "independent",
        "sparsity": "on",
        "coordination": "on",
    },
    "sparsity_baseline": {
        "pipeline": "latency-aware",
        "combination": "independent",
        "sparsity": "off",
        "coordination": "on",
    },
    "pipeline_baseline": {
        "pipeline": "sequential",
        "combination": "independent",
        "sparsity": "on",
        "coordination": "on",
    },
    "coordination_baseline": {
        "pipeline": "latency-aware",
        "combination": "independent",
        "sparsity": "on",
        "coordination": "off",
    },
}

PAIR_TARGETS = {
    "sparsity_baseline": "sparsity",
    "pipeline_baseline": "pipeline",
    "coordination_baseline": "coordination",
}


def parse_args():
    parser = argparse.ArgumentParser(description="Run HyGCN paper ablations")
    parser.add_argument("--binary", default="build/hygcntest")
    parser.add_argument("--reference", default="configs/paper_metrics.json")
    parser.add_argument("--profile-path")
    parser.add_argument("--output-dir", default="res/paper")
    parser.add_argument("--datasets", nargs="*")
    parser.add_argument("--force", action="store_true")
    return parser.parse_args()


def result_name(dataset, variant):
    flags = VARIANTS[variant]
    return (
        f"paper_paper_gcn_{dataset}_{flags['pipeline']}_{flags['combination']}_"
        f"sparse-{flags['sparsity']}_coord-{flags['coordination']}_seed-1.json"
    )


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


def expected_graph_digest(root, dataset):
    graph_dir = root / "gcn_dataset"
    return (
        f"{fnv1a_digest(graph_dir / f'{dataset}.txt')}-"
        f"{fnv1a_digest(graph_dir / f'{dataset}_edge.csv')}"
    )


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
        "selected_layer": "all",
        "pipeline": flags["pipeline"],
        "combination": flags["combination"],
        "sparsity_elimination": flags["sparsity"] == "on",
        "memory_coordination": flags["coordination"] == "on",
    }
    return all(manifest.get(key) == value for key, value in expected.items())


def run_variant(root, binary, output_dir, dataset, variant, force, profile_path):
    result_path = output_dir / result_name(dataset, variant)
    result = None
    if not force and result_path.is_file():
        try:
            with result_path.open(encoding="utf-8") as stream:
                candidate = json.load(stream)
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
            "--pipeline", flags["pipeline"],
            "--combination", flags["combination"],
            "--sparsity", flags["sparsity"],
            "--coordination", flags["coordination"],
            "--seed", "1",
            "--output-dir", str(output_dir),
            "--quiet",
        ]
        if profile_path:
            command.extend(["--profile-path", str(profile_path)])
        subprocess.run(command, cwd=root, check=True)
        with result_path.open(encoding="utf-8") as stream:
            result = json.load(stream)
        if not cached_result_valid(result, root, binary, dataset, variant, profile_path):
            raise ValueError(f"generated result manifest is invalid: {result_path}")
    return result


def ratio(numerator, denominator, name):
    if not math.isfinite(numerator) or not math.isfinite(denominator) or denominator == 0:
        raise ValueError(f"invalid ratio inputs for {name}: {numerator}/{denominator}")
    return numerator / denominator


def mean(values):
    if not values:
        raise ValueError("cannot average an empty metric list")
    return sum(values) / len(values)


def validate_pair(optimized, baseline, target, dataset):
    optimized_manifest = optimized["manifest"]
    baseline_manifest = baseline["manifest"]
    allowed = {
        "sparsity": {"sparsity_elimination"},
        "pipeline": {"pipeline"},
        "coordination": {"memory_coordination"},
    }[target]
    ignored = {"git_commit"}
    keys = set(optimized_manifest) | set(baseline_manifest)
    changed = {
        key for key in keys
        if key not in ignored and optimized_manifest.get(key) != baseline_manifest.get(key)
    }
    if changed != allowed:
        raise ValueError(
            f"{dataset} {target} ablation changed {sorted(changed)}, expected {sorted(allowed)}"
        )
    if optimized.get("architecture") != baseline.get("architecture"):
        raise ValueError(f"{dataset} {target} ablation changed architecture parameters")


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    binary = Path(args.binary)
    if not binary.is_absolute():
        binary = root / binary
    reference_path = Path(args.reference)
    if not reference_path.is_absolute():
        reference_path = root / reference_path
    output_dir = Path(args.output_dir)
    if not output_dir.is_absolute():
        output_dir = root / output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    profile_path = Path(args.profile_path) if args.profile_path else root / "configs/HYGCN_PAPER.ini"
    if not profile_path.is_absolute():
        profile_path = root / profile_path

    with reference_path.open(encoding="utf-8") as stream:
        reference = json.load(stream)
    datasets = args.datasets or reference["datasets"]
    per_dataset = {}
    metric_values = {name: [] for name in reference["metrics"]}

    for dataset in datasets:
        runs = {
            name: run_variant(root, binary, output_dir, dataset, name, args.force, profile_path)
            for name in VARIANTS
        }
        for baseline, target in PAIR_TARGETS.items():
            validate_pair(runs["optimized"], runs[baseline], target, dataset)
        optimized = runs["optimized"]["summary"]
        sparse_base = runs["sparsity_baseline"]["summary"]
        pipeline_base = runs["pipeline_baseline"]["summary"]
        coord_base = runs["coordination_baseline"]["summary"]
        metrics = {
            "sparsity_speedup": ratio(
                sparse_base["total_cycles"], optimized["total_cycles"], "sparsity_speedup"),
            "sparsity_input_dram_ratio": ratio(
                optimized["total_input_dram_bytes"],
                sparse_base["total_input_dram_bytes"],
                "sparsity_input_dram_ratio"),
            "pipeline_speedup": ratio(
                pipeline_base["total_cycles"], optimized["total_cycles"], "pipeline_speedup"),
            "pipeline_dram_ratio": ratio(
                optimized["total_dram_bytes"],
                pipeline_base["total_dram_bytes"],
                "pipeline_dram_ratio"),
            "coordination_speedup": ratio(
                coord_base["total_cycles"], optimized["total_cycles"], "coordination_speedup"),
            "coordination_bandwidth_gain": ratio(
                optimized["bandwidth_utilization"],
                coord_base["bandwidth_utilization"],
                "coordination_bandwidth_gain"),
        }
        per_dataset[dataset] = {
            "metrics": metrics,
            "runs": {name: result_name(dataset, name) for name in VARIANTS},
            "raw": {name: run["summary"] for name, run in runs.items()},
        }
        for name, value in metrics.items():
            metric_values[name].append(value)

    aggregate = {name: mean(values) for name, values in metric_values.items()}
    report = {
        "schema_version": 1,
        "reference_file": str(reference_path.relative_to(root)),
        "datasets": datasets,
        "aggregate": aggregate,
        "per_dataset": per_dataset,
        "scope": {
            "validated": "relative microarchitectural effects for GCN",
            "not_validated": [
                "absolute CPU speedup",
                "absolute GPU speedup",
                "DiffPool",
                "complete chip energy and area",
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
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
