#!/usr/bin/env python3
import argparse
import csv
import gc
import hashlib
import json
import math
import os
import platform
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

from report_figures import write_raster, write_svg


MODEL_ORDER = ("gcn", "gin", "gs")
DATASET_ORDER = ("citeseer", "cora", "dblp", "pubmed")
MODEL_LABEL = {"gcn": "GCN", "gin": "GIN", "gs": "GS"}
DATASET_LABEL = {
    "citeseer": "CS", "cora": "CR", "dblp": "DBLP", "pubmed": "PB",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Benchmark two-layer PyG CPU inference and compare it with HyGCN"
    )
    parser.add_argument("--graph-dir", default="gcn_dataset")
    parser.add_argument("--hygcn-dir", default="res/report/raw")
    parser.add_argument("--report-summary", default="res/report/report_summary.json")
    parser.add_argument("--output-dir", default="res/pyg-cpu")
    parser.add_argument("--models", nargs="+", choices=MODEL_ORDER,
                        default=list(MODEL_ORDER))
    parser.add_argument("--datasets", nargs="+", choices=DATASET_ORDER,
                        default=list(DATASET_ORDER))
    parser.add_argument("--threads", type=int, default=20)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--iterations", type=int, default=7)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--frequency-ghz", type=float, default=0.5)
    parser.add_argument("--no-affinity", action="store_true")
    parser.add_argument("--skip-raster", action="store_true")
    parser.add_argument("--check-deps", action="store_true")
    return parser.parse_args()


def resolve(root, value):
    path = Path(value)
    return path if path.is_absolute() else root / path


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def configure_cpu(threads, set_affinity):
    if threads <= 0:
        raise ValueError("threads must be positive")
    available = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") \
        else list(range(os.cpu_count() or 1))
    if threads > len(available):
        raise ValueError(
            f"requested {threads} threads but only {len(available)} CPUs are available"
        )
    selected = available[:threads]
    if set_affinity and hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(0, selected)
    for variable in (
        "OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS",
        "NUMEXPR_NUM_THREADS",
    ):
        os.environ[variable] = str(threads)
    os.environ["OMP_DYNAMIC"] = "FALSE"
    return available, selected


def import_frameworks():
    try:
        import torch
        import torch_geometric
        from torch import nn
        from torch_geometric.nn import GCNConv, GINConv, SAGEConv
    except ImportError as error:
        raise RuntimeError(
            "PyG CPU dependencies are missing; install requirements-pyg-cpu.txt"
        ) from error
    return torch, torch_geometric, nn, GCNConv, GINConv, SAGEConv


def cpu_model_name():
    completed = subprocess.run(
        ["lscpu"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True, check=False,
    )
    if completed.returncode == 0:
        for line in completed.stdout.splitlines():
            if line.lower().startswith("model name:"):
                return line.split(":", 1)[1].strip()
    path = Path("/proc/cpuinfo")
    if path.is_file():
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.lower().startswith("model name") and ":" in line:
                return line.split(":", 1)[1].strip()
    return platform.processor() or "unknown"


def git_value(root, *arguments):
    completed = subprocess.run(
        ["git", *arguments], cwd=root, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True, check=False,
    )
    return completed.stdout.strip() if completed.returncode == 0 else "unknown"


def read_metadata(path):
    metadata = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, value = line.split(",", 1)
        metadata[key] = int(value)
    required = {"edge", "feature", "vertex", "class"}
    if set(metadata) != required or any(metadata[key] <= 0 for key in required):
        raise ValueError(f"invalid graph metadata: {path}")
    return metadata


def read_edges(path, vertices):
    sources = []
    destinations = []
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            source_text, destination_text = line.rstrip().split(",", 1)
            source = int(source_text)
            destination = int(destination_text)
            if not (0 <= source < vertices and 0 <= destination < vertices):
                raise ValueError(f"edge outside graph at {path}:{line_number}")
            sources.append(source)
            destinations.append(destination)
    return sources, destinations


def deterministic_sample(sources, destinations, vertices, sample_size=25):
    neighbors = [[] for _ in range(vertices)]
    for source, destination in zip(sources, destinations):
        neighbors[destination].append(source)
    sampled_sources = []
    sampled_destinations = []
    for destination, candidates in enumerate(neighbors):
        if not candidates:
            continue
        for index in range(sample_size):
            source_index = index * len(candidates) // sample_size
            sampled_sources.append(candidates[min(source_index, len(candidates) - 1)])
            sampled_destinations.append(destination)
    return sampled_sources, sampled_destinations


def make_model(model_name, metadata, framework):
    torch, _, nn, GCNConv, GINConv, SAGEConv = framework
    input_features = metadata["feature"]
    classes = metadata["class"]
    hidden = 128

    class GcnModel(nn.Module):
        def __init__(self):
            super().__init__()
            self.conv1 = GCNConv(input_features, hidden, cached=True)
            self.conv2 = GCNConv(hidden, classes, cached=True)

        def forward(self, features, edge_index):
            hidden_features = torch.relu(self.conv1(features, edge_index))
            return self.conv2(hidden_features, edge_index)

    class GinModel(nn.Module):
        def __init__(self):
            super().__init__()
            first_mlp = nn.Sequential(
                nn.Linear(input_features, hidden), nn.ReLU(),
                nn.Linear(hidden, hidden),
            )
            second_mlp = nn.Sequential(
                nn.Linear(hidden, hidden), nn.ReLU(),
                nn.Linear(hidden, classes),
            )
            self.conv1 = GINConv(first_mlp, train_eps=False)
            self.conv2 = GINConv(second_mlp, train_eps=False)

        def forward(self, features, edge_index):
            hidden_features = torch.relu(self.conv1(features, edge_index))
            return self.conv2(hidden_features, edge_index)

    class GraphSageModel(nn.Module):
        def __init__(self):
            super().__init__()
            self.conv1 = SAGEConv(input_features, hidden)
            self.conv2 = SAGEConv(hidden, classes)

        def forward(self, features, edge_index):
            hidden_features = torch.relu(self.conv1(features, edge_index))
            return self.conv2(hidden_features, edge_index)

    models = {"gcn": GcnModel, "gin": GinModel, "gs": GraphSageModel}
    return models[model_name]().eval()


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def run_inference(model, features, edge_index, torch, warmup, iterations):
    if warmup < 1 or iterations < 3:
        raise ValueError("benchmark requires at least one warmup and three iterations")
    samples_ms = []
    checksum = 0.0
    with torch.inference_mode():
        for _ in range(warmup):
            checksum = float(model(features, edge_index).sum().item())
        gc.collect()
        for _ in range(iterations):
            start = time.perf_counter_ns()
            output = model(features, edge_index)
            checksum = float(output.sum().item())
            elapsed_ns = time.perf_counter_ns() - start
            samples_ms.append(elapsed_ns / 1_000_000.0)
    if not math.isfinite(checksum) or any(value <= 0 for value in samples_ms):
        raise RuntimeError("inference produced invalid output or timing")
    return {
        "samples_ms": samples_ms,
        "median_ms": statistics.median(samples_ms),
        "mean_ms": statistics.fmean(samples_ms),
        "min_ms": min(samples_ms),
        "max_ms": max(samples_ms),
        "p10_ms": percentile(samples_ms, 0.10),
        "p90_ms": percentile(samples_ms, 0.90),
        "stdev_ms": statistics.stdev(samples_ms),
        "checksum": checksum,
    }


def load_hygcn(path, frequency_ghz):
    with path.open(encoding="utf-8") as stream:
        result = json.load(stream)
    cycles = int(result["summary"]["total_cycles"])
    return {
        "cycles": cycles,
        "latency_ms": cycles / (frequency_ghz * 1_000_000.0),
        "dram_energy_pj": float(result["summary"]["total_dram_energy_pj"]),
        "git_commit": result["manifest"]["git_commit"],
        "path": str(path),
    }


def load_report_targets(path):
    with path.open(encoding="utf-8") as stream:
        summary = json.load(stream)
    return {point["label"]: float(point["speedup"]) for point in summary["points"]}


def benchmark_workload(model_name, dataset, args, paths, framework, targets):
    torch = framework[0]
    label = f"{MODEL_LABEL[model_name]}-{DATASET_LABEL[dataset]}"
    metadata_path = paths["graph_dir"] / f"{dataset}.txt"
    edges_path = paths["graph_dir"] / f"{dataset}_edge.csv"
    metadata = read_metadata(metadata_path)
    sources, destinations = read_edges(edges_path, metadata["vertex"])
    if len(sources) != metadata["edge"]:
        raise ValueError(
            f"{dataset} metadata says {metadata['edge']} edges, found {len(sources)}"
        )
    graph_mode = "full graph"
    if model_name == "gs":
        sources, destinations = deterministic_sample(
            sources, destinations, metadata["vertex"], 25
        )
        graph_mode = "deterministic 25-neighbor sampled graph"
    edge_index = torch.tensor([sources, destinations], dtype=torch.long)
    generator = torch.Generator(device="cpu")
    workload_seed = args.seed + MODEL_ORDER.index(model_name) * 100 + \
        DATASET_ORDER.index(dataset)
    generator.manual_seed(workload_seed)
    features = torch.empty(
        (metadata["vertex"], metadata["feature"]), dtype=torch.float32
    )
    features.normal_(mean=0.0, std=1.0, generator=generator)
    torch.manual_seed(workload_seed)
    model = make_model(model_name, metadata, framework)
    parameter_count = sum(parameter.numel() for parameter in model.parameters())
    timing = run_inference(
        model, features, edge_index, torch, args.warmup, args.iterations
    )
    hygcn_path = paths["hygcn_dir"] / (
        f"legacy_{model_name}_{dataset}_seed-1.json"
    )
    hygcn = load_hygcn(hygcn_path, args.frequency_ghz)
    measured_speedup = timing["median_ms"] / hygcn["latency_ms"]
    target_speedup = targets.get(label)
    result = {
        "label": label,
        "model": model_name,
        "dataset": dataset,
        "graph_mode": graph_mode,
        "vertices": metadata["vertex"],
        "input_edges": metadata["edge"],
        "inference_edges": edge_index.shape[1],
        "input_features": metadata["feature"],
        "classes": metadata["class"],
        "hidden_features": 128,
        "parameter_count": parameter_count,
        "feature_bytes": features.numel() * features.element_size(),
        "edge_index_bytes": edge_index.numel() * edge_index.element_size(),
        "workload_seed": workload_seed,
        "graph_metadata_sha256": sha256(metadata_path),
        "graph_edges_sha256": sha256(edges_path),
        "pyg_cpu": timing,
        "hygcn": hygcn,
        "measured_speedup": measured_speedup,
        "report_target_speedup": target_speedup,
        "target_relative_error_percent": (
            None if target_speedup is None else
            abs(measured_speedup - target_speedup) / target_speedup * 100.0
        ),
    }
    del model, features, edge_index, sources, destinations
    gc.collect()
    return result


def geometric_mean(values):
    return math.exp(statistics.fmean(math.log(value) for value in values))


def write_csv(path, results):
    fields = [
        "label", "model", "dataset", "graph_mode", "vertices", "input_edges",
        "inference_edges", "input_features", "classes", "parameter_count",
        "cpu_median_ms", "cpu_p10_ms", "cpu_p90_ms", "hygcn_cycles",
        "hygcn_latency_ms", "measured_speedup", "report_target_speedup",
        "target_relative_error_percent", "output_checksum",
    ]
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for result in results:
            writer.writerow({
                "label": result["label"],
                "model": result["model"],
                "dataset": result["dataset"],
                "graph_mode": result["graph_mode"],
                "vertices": result["vertices"],
                "input_edges": result["input_edges"],
                "inference_edges": result["inference_edges"],
                "input_features": result["input_features"],
                "classes": result["classes"],
                "parameter_count": result["parameter_count"],
                "cpu_median_ms": result["pyg_cpu"]["median_ms"],
                "cpu_p10_ms": result["pyg_cpu"]["p10_ms"],
                "cpu_p90_ms": result["pyg_cpu"]["p90_ms"],
                "hygcn_cycles": result["hygcn"]["cycles"],
                "hygcn_latency_ms": result["hygcn"]["latency_ms"],
                "measured_speedup": result["measured_speedup"],
                "report_target_speedup": result["report_target_speedup"],
                "target_relative_error_percent": result[
                    "target_relative_error_percent"
                ],
                "output_checksum": result["pyg_cpu"]["checksum"],
            })


def write_markdown(path, report):
    environment = report["environment"]
    summary = report["summary"]
    lines = [
        "# Measured PyG-CPU Speedup",
        "",
        "## Result",
        "",
        f"- Arithmetic mean speedup: **{summary['arithmetic_mean_speedup']:.3f}x**.",
        f"- Geometric mean speedup: **{summary['geometric_mean_speedup']:.3f}x**.",
        f"- Workloads measured: **{summary['workloads']}**.",
        f"- CPU: `{environment['cpu_model']}`, {environment['threads']} pinned threads.",
        f"- Framework: PyTorch `{environment['torch_version']}`, PyG `{environment['pyg_version']}`.",
        "",
        "## Workloads",
        "",
        "| Workload | PyG CPU median | HyGCN | Measured speedup | PDF target |",
        "|---|---:|---:|---:|---:|",
    ]
    for result in report["results"]:
        lines.append(
            f"| {result['label']} | {result['pyg_cpu']['median_ms']:.3f} ms | "
            f"{result['hygcn']['latency_ms']:.3f} ms | "
            f"{result['measured_speedup']:.3f}x | "
            f"{result['report_target_speedup']:.2f}x |"
        )
    lines.extend([
        "",
        "## Method",
        "",
        "Each workload uses a two-layer, hidden-size-128 PyG model in eager CPU inference mode. Input tensors and graph parsing are outside the timed region. GCN normalization is cached during warmup. GraphSAGE uses the same deterministic 25-neighbor sampling policy as the local HyGCN fallback. The reported CPU value is the median wall-clock latency after warmup; output reduction to a scalar is included so execution cannot be elided.",
        "",
        "HyGCN latency is the committed legacy simulator cycle count divided by 0.5 GHz. The model uses random deterministic features and weights because this benchmark measures inference execution time, not trained-model accuracy.",
        "",
        "## Boundary",
        "",
        "This is a real local project measurement, but the host is a Kunpeng-920 system rather than the report's dual Intel Xeon 4210R platform. It establishes a reproducible PyG-CPU baseline and measured local speedup; it does not independently reproduce the report's CPU hardware environment or its Reddit results.",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    available_cpus, selected_cpus = configure_cpu(
        args.threads, not args.no_affinity
    )
    framework = import_frameworks()
    torch, torch_geometric = framework[0], framework[1]
    if args.check_deps:
        print(f"torch={torch.__version__}")
        print(f"torch_geometric={torch_geometric.__version__}")
        print(f"selected_cpus={','.join(str(cpu) for cpu in selected_cpus)}")
        return 0
    torch.set_num_threads(args.threads)
    torch.set_num_interop_threads(1)
    torch.manual_seed(args.seed)

    paths = {
        "graph_dir": resolve(root, args.graph_dir),
        "hygcn_dir": resolve(root, args.hygcn_dir),
        "report_summary": resolve(root, args.report_summary),
        "output_dir": resolve(root, args.output_dir),
    }
    paths["output_dir"].mkdir(parents=True, exist_ok=True)
    targets = load_report_targets(paths["report_summary"])
    results = []
    for model_name in MODEL_ORDER:
        if model_name not in args.models:
            continue
        for dataset in DATASET_ORDER:
            if dataset not in args.datasets:
                continue
            result = benchmark_workload(
                model_name, dataset, args, paths, framework, targets
            )
            results.append(result)
            print(
                f"{result['label']}: cpu={result['pyg_cpu']['median_ms']:.3f}ms "
                f"hygcn={result['hygcn']['latency_ms']:.3f}ms "
                f"speedup={result['measured_speedup']:.3f}x",
                flush=True,
            )
    speedups = [result["measured_speedup"] for result in results]
    environment = {
        "hostname": platform.node(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "cpu_model": cpu_model_name(),
        "available_cpu_count": len(available_cpus),
        "threads": args.threads,
        "affinity_enabled": not args.no_affinity,
        "selected_cpus": selected_cpus,
        "torch_version": torch.__version__,
        "pyg_version": torch_geometric.__version__,
        "python_version": platform.python_version(),
        "mkldnn_available": torch.backends.mkldnn.is_available(),
    }
    report = {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "git_branch": git_value(root, "branch", "--show-current"),
        "git_commit": git_value(root, "rev-parse", "HEAD"),
        "environment": environment,
        "method": {
            "execution": "PyTorch eager CPU inference",
            "layers": 2,
            "hidden_features": 128,
            "warmup_iterations": args.warmup,
            "measured_iterations": args.iterations,
            "statistic": "median wall-clock milliseconds",
            "input_dtype": "float32",
            "feature_and_weight_seed": args.seed,
            "data_loading_timed": False,
            "gcn_cached_normalization": True,
            "graphsage_sample_size": 25,
            "hygcn_frequency_ghz": args.frequency_ghz,
        },
        "summary": {
            "workloads": len(results),
            "arithmetic_mean_speedup": statistics.fmean(speedups),
            "geometric_mean_speedup": geometric_mean(speedups),
            "min_speedup": min(speedups),
            "max_speedup": max(speedups),
        },
        "results": results,
    }
    json_path = paths["output_dir"] / "pyg_cpu_benchmark.json"
    with json_path.open("w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, sort_keys=True)
        stream.write("\n")
    write_csv(paths["output_dir"] / "pyg_cpu_benchmark.csv", results)
    write_markdown(paths["output_dir"] / "MEASURED_SPEEDUP.md", report)

    chart_points = [
        {"label": result["label"], "speedup": result["measured_speedup"]}
        for result in results
    ]
    mean_speedup = report["summary"]["arithmetic_mean_speedup"]
    chart_text = {
        "speedup": {
            "title": "Measured PyG-CPU vs HyGCN Speedup",
            "subtitle": (
                f"PyG {torch_geometric.__version__}, {args.threads} pinned CPU threads; "
                f"HyGCN legacy simulator at {args.frequency_ghz:g} GHz"
            ),
            "mean_text": f"Arithmetic mean measured speedup: {mean_speedup:.2f}x",
            "max_exponent": max(1, math.ceil(math.log10(max(speedups)))),
        }
    }
    write_svg(
        paths["output_dir"] / "pyg_measured_speedup.svg", chart_points,
        ["speedup"], chart_text,
    )
    if not args.skip_raster:
        write_raster(
            paths["output_dir"], chart_points, prefix="pyg_measured",
            metrics=["speedup"], chart_text=chart_text,
        )
    print(f"output_dir={paths['output_dir']}")
    print(f"arithmetic_mean_speedup={mean_speedup:.6f}")
    print(
        f"geometric_mean_speedup={report['summary']['geometric_mean_speedup']:.6f}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, RuntimeError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
