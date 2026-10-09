#!/usr/bin/env python3
import argparse
import hashlib
import json
import math
from pathlib import Path


FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
FNV_MASK = (1 << 64) - 1


def parse_args():
    parser = argparse.ArgumentParser(
        description="Generate a diagnostic Degree-Aware quantization manifest"
    )
    parser.add_argument("--graph-dir", default="gcn_dataset")
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--model", choices=("gcn", "gin", "gs"), default="gcn")
    parser.add_argument("--output", required=True)
    parser.add_argument("--layer0-density", type=float, default=0.10)
    parser.add_argument("--layer1-density", type=float, default=0.70)
    return parser.parse_args()


def read_metadata(path):
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, value = line.split(",", 1)
        values[key] = int(value)
    if set(values) != {"edge", "feature", "vertex", "class"}:
        raise ValueError(f"invalid graph metadata: {path}")
    return values


def read_degrees(path, vertices, model):
    incoming = [set() for _ in range(vertices)]
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            source_text, destination_text = line.rstrip().split(",", 1)
            source = int(source_text)
            destination = int(destination_text)
            if not (0 <= source < vertices and 0 <= destination < vertices):
                raise ValueError(f"edge outside graph at {path}:{line_number}")
            incoming[destination].add(source)
    if model in {"gcn", "gin"}:
        for vertex in range(vertices):
            incoming[vertex].add(vertex)
    return [len(neighbors) for neighbors in incoming]


def fnv_digest(path):
    value = FNV_OFFSET
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            for byte in chunk:
                value ^= byte
                value = (value * FNV_PRIME) & FNV_MASK
    return f"{value:016x}"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def percentile(values, fraction):
    ordered = sorted(values)
    index = min(len(ordered) - 1, math.floor((len(ordered) - 1) * fraction))
    return ordered[index]


def degree_rules(degrees):
    median = percentile(degrees, 0.50)
    p90 = max(median + 1, percentile(degrees, 0.90))
    return [
        {"min_degree": 0, "max_degree": median, "bitwidth": 2, "scale": 0.25},
        {
            "min_degree": median + 1,
            "max_degree": p90,
            "bitwidth": 3,
            "scale": 0.25,
        },
        {
            "min_degree": p90 + 1,
            "max_degree": "max",
            "bitwidth": 4,
            "scale": 0.25,
        },
    ]


def weighted_average_bits(degrees, rules):
    total = 0
    for degree in degrees:
        for rule in rules:
            maximum = math.inf if rule["max_degree"] == "max" else rule["max_degree"]
            if rule["min_degree"] <= degree <= maximum:
                total += rule["bitwidth"]
                break
    return total / len(degrees)


def layer(layer_id, features, outputs, density, rules):
    if not 0.0 <= density <= 1.0:
        raise ValueError("feature density must be in [0, 1]")
    return {
        "layer": layer_id,
        "feature_count": features,
        "output_features": outputs,
        "feature_density": density,
        "weight_bits": 4,
        "weight_scales": [0.125] * outputs,
        "degree_rules": rules,
    }


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    graph_dir = Path(args.graph_dir)
    if not graph_dir.is_absolute():
        graph_dir = root / graph_dir
    metadata_path = graph_dir / f"{args.dataset}.txt"
    edge_path = graph_dir / f"{args.dataset}_edge.csv"
    metadata = read_metadata(metadata_path)
    degrees = read_degrees(edge_path, metadata["vertex"], args.model)
    rules = degree_rules(degrees)
    hidden = 256 if args.model == "gs" else 128
    manifest = {
        "schema_version": 1,
        "manifest_version": "diagnostic-degree-quantiles-v1",
        "dataset": args.dataset,
        "model": args.model,
        "graph_digest": f"{fnv_digest(metadata_path)}-{fnv_digest(edge_path)}",
        "provenance": "diagnostic-heuristic",
        "source_sha256": {
            "metadata": sha256(metadata_path),
            "edges": sha256(edge_path),
        },
        "diagnostic": {
            "reason": "No author per-node bitwidth/scale artifact is available",
            "degree_p50": percentile(degrees, 0.50),
            "degree_p90": percentile(degrees, 0.90),
            "average_feature_bits": weighted_average_bits(degrees, rules),
        },
        "layers": [
            layer(0, metadata["feature"], hidden, args.layer0_density, rules),
            layer(1, hidden, metadata["class"], args.layer1_density, rules),
        ],
    }
    output = Path(args.output)
    if not output.is_absolute():
        output = root / output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"mega_quantization_manifest={output}")
    print("provenance=diagnostic-heuristic required_eligible=false")


if __name__ == "__main__":
    main()
