#!/usr/bin/env python3
import argparse
import json
import math
import sys
from pathlib import Path


EXPECTED_DATASETS = {"cora", "citeseer", "pubmed", "nell", "reddit"}
EXPECTED_MODELS = {"gcn", "gin", "graphsage"}
EXPECTED_BUFFERS_KIB = {
    "input": 64,
    "edge": 24,
    "weight": 48,
    "combination": 96,
    "aggregation": 128,
    "sparse": 32,
}
EXPECTED_ABLATION_ORDER = [
    "m0_fp32_axw",
    "m1_degree_aware_bitmap",
    "m2_adaptive_package",
    "m3_condense_edge",
]


def parse_args():
    parser = argparse.ArgumentParser(description="Validate the MEGA paper reference manifest")
    parser.add_argument("--reference", default="configs/mega_paper_reference.json")
    return parser.parse_args()


def positive_number(value, name):
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise ValueError(f"{name} must be numeric")
    if not math.isfinite(value) or value <= 0:
        raise ValueError(f"{name} must be finite and positive")


def validate_positive_tree(node, prefix):
    if not isinstance(node, dict) or not node:
        raise ValueError(f"{prefix} must be a non-empty object")
    for key, value in node.items():
        name = f"{prefix}.{key}"
        if isinstance(value, dict):
            validate_positive_tree(value, name)
        else:
            positive_number(value, name)


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    path = Path(args.reference)
    if not path.is_absolute():
        path = root / path
    with path.open(encoding="utf-8") as stream:
        reference = json.load(stream)

    if reference.get("schema_version") != 1:
        raise ValueError("MEGA reference requires schema_version=1")
    if not reference.get("reference_version"):
        raise ValueError("MEGA reference is missing reference_version")

    paper = reference.get("paper", {})
    if paper.get("arxiv_id") != "2311.09775v1":
        raise ValueError("MEGA reference must identify arXiv 2311.09775v1")
    if paper.get("source_url") != "https://arxiv.org/html/2311.09775v1":
        raise ValueError("MEGA reference source_url is not the versioned paper URL")

    datasets = reference.get("datasets", {})
    if set(datasets) != EXPECTED_DATASETS:
        raise ValueError("MEGA reference must contain all five paper datasets")
    for dataset, values in datasets.items():
        validate_positive_tree(values, f"datasets.{dataset}")

    models = reference.get("models", {})
    if set(models) != EXPECTED_MODELS:
        raise ValueError("MEGA reference must contain GCN, GIN, and GraphSage")

    architecture = reference.get("architecture", {})
    if architecture.get("technology_nm") != 28:
        raise ValueError("MEGA paper technology must be 28nm")
    if architecture.get("frequency_ghz") != 1.0:
        raise ValueError("MEGA paper frequency must be 1GHz")
    if architecture.get("hbm_bandwidth_gbps") != 256:
        raise ValueError("MEGA paper HBM bandwidth must be 256GB/s")

    buffers = architecture.get("buffers_kib", {})
    for name, expected in EXPECTED_BUFFERS_KIB.items():
        if buffers.get(name) != expected:
            raise ValueError(f"MEGA {name} buffer must be {expected}KiB")
    total = sum(buffers[name] for name in EXPECTED_BUFFERS_KIB)
    if total != 392 or buffers.get("total") != total:
        raise ValueError("MEGA buffers must sum to 392KiB")

    processing = architecture.get("processing", {})
    expected_processing = {
        "combination_tiles": 4,
        "cpes_per_tile": 8,
        "bses_per_cpe": 32,
        "aggregation_units": 256,
        "weight_bits": 4,
        "partial_sum_bits": 16,
        "eid_fifos": 16,
        "eid_fifo_entries": 8,
    }
    for name, expected in expected_processing.items():
        if processing.get(name) != expected:
            raise ValueError(f"MEGA processing.{name} must be {expected}")

    package = architecture.get("adaptive_package_bits", {})
    if [package.get(name) for name in ("short", "medium", "long")] != [64, 128, 192]:
        raise ValueError("MEGA Adaptive-Package lengths must be 64/128/192 bits")
    if package.get("mode_bits") != 2 or package.get("bitwidth_bits") != 3:
        raise ValueError("MEGA package header must contain 2-bit mode and 3-bit bitwidth")
    if package.get("supported_value_bits") != list(range(1, 9)):
        raise ValueError("MEGA package must support value bitwidths 1 through 8")

    validate_positive_tree(
        reference.get("paper_reference_metrics", {}), "paper_reference_metrics"
    )

    policy = reference.get("evaluation_policy", {})
    if policy.get("ablation_order") != EXPECTED_ABLATION_ORDER:
        raise ValueError("MEGA ablation order must be M0 -> M1 -> M2 -> M3")
    if set(policy.get("required_local_workloads", [])) != {"cora", "citeseer", "pubmed"}:
        raise ValueError("MEGA local gate must cover Cora, Citeseer, and PubMed")
    if policy.get("paper_metrics_are_reference_only_until_full_workload_and_quantization_provenance") is not True:
        raise ValueError("MEGA paper metrics must remain reference-only during initial development")
    if policy.get("forbid_target_fitted_baseline_throttle") is not True:
        raise ValueError("MEGA evaluation must forbid target-fitted baseline throttles")

    print(f"mega_reference_manifest=PASS path={path} buffers_kib={total}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
