#!/usr/bin/env python3
import argparse
import json
import math
import sys
from pathlib import Path


REQUIRED_METRICS = {
    "sparsity_speedup",
    "sparsity_input_dram_ratio",
    "pipeline_speedup",
    "pipeline_dram_ratio",
    "coordination_speedup",
    "coordination_bandwidth_gain",
}
REQUIRED_DATASETS = {"cora", "citeseer", "pubmed"}


def parse_args():
    parser = argparse.ArgumentParser(description="Validate the HyGCN paper metric manifest")
    parser.add_argument("--reference", default="configs/paper_metrics.json")
    return parser.parse_args()


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    path = Path(args.reference)
    if not path.is_absolute():
        path = root / path
    with path.open(encoding="utf-8") as stream:
        manifest = json.load(stream)

    if manifest.get("schema_version") != 2 or not manifest.get("reference_version"):
        raise ValueError("reference manifest requires schema_version=2 and reference_version")
    tolerance = manifest.get("tolerance")
    if not isinstance(tolerance, (int, float)) or not math.isfinite(tolerance) or tolerance != 0.20:
        raise ValueError("reference manifest tolerance must be 0.20")
    if set(manifest.get("datasets", [])) != REQUIRED_DATASETS:
        raise ValueError("reference manifest must cover Cora, Citeseer, and PubMed")
    metrics = manifest.get("metrics", {})
    if set(metrics) != REQUIRED_METRICS:
        raise ValueError("reference manifest does not contain the required metric set")
    for name, definition in metrics.items():
        if not isinstance(definition.get("required"), bool):
            raise ValueError(f"{name} is missing required flag")
        for field in ("validation", "unit", "source", "aggregation"):
            if not isinstance(definition.get(field), str) or not definition[field].strip():
                raise ValueError(f"{name} is missing {field}")
        validation = definition["validation"]
        if validation == "aggregate_relative_error":
            value = definition.get("reference")
            if not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
                raise ValueError(f"{name} has an invalid aggregate reference")
            if definition["aggregation"] != "arithmetic_mean_over_datasets":
                raise ValueError(f"{name} must use arithmetic mean aggregation")
        elif validation == "per_dataset_range":
            lower = definition.get("reference_min")
            upper = definition.get("reference_max")
            if (not isinstance(lower, (int, float)) or
                    not isinstance(upper, (int, float)) or
                    not math.isfinite(lower) or not math.isfinite(upper) or
                    lower <= 0 or upper < lower):
                raise ValueError(f"{name} has an invalid reference range")
            if definition["aggregation"] != "per_dataset":
                raise ValueError(f"{name} range validation must be per-dataset")
        elif validation == "diagnostic_only":
            if definition["required"]:
                raise ValueError(f"{name} diagnostic-only metric cannot be required")
        else:
            raise ValueError(f"{name} uses unsupported validation {validation}")
    print(f"reference_manifest=PASS path={path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
