#!/usr/bin/env python3
import argparse
import json
import math
import sys
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="Validate HyGCN paper metrics")
    parser.add_argument("--reference", default="configs/paper_metrics.json")
    parser.add_argument("--report", default="res/paper/benchmark_report.json")
    parser.add_argument("--output", default="res/paper/validation_report.md")
    return parser.parse_args()


def load_json(path):
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    reference_path = Path(args.reference)
    report_path = Path(args.report)
    output_path = Path(args.output)
    if not reference_path.is_absolute():
        reference_path = root / reference_path
    if not report_path.is_absolute():
        report_path = root / report_path
    if not output_path.is_absolute():
        output_path = root / output_path

    reference = load_json(reference_path)
    report = load_json(report_path)
    tolerance = float(reference["tolerance"])
    aggregate = report.get("aggregate", {})
    rows = []
    failed = False
    for name, definition in reference["metrics"].items():
        measured = aggregate.get(name)
        expected = float(definition["reference"])
        if measured is None or not math.isfinite(measured) or expected == 0:
            error = math.inf
            passed = False
        else:
            error = abs(measured - expected) / abs(expected)
            passed = error <= tolerance
        failed = failed or not passed
        rows.append((name, expected, measured, error, passed, definition["source"]))

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as stream:
        stream.write("# HyGCN Paper Metric Validation\n\n")
        stream.write(f"Tolerance: {tolerance:.0%}\n\n")
        stream.write(
            "Acceptance rule: arithmetic mean across Cora, Citeseer, and PubMed. "
            "Per-dataset values below are diagnostics because the cited paper values are averages.\n\n"
        )
        stream.write("| Metric | Reference | Measured | Relative error | Status |\n")
        stream.write("|---|---:|---:|---:|---|\n")
        for name, expected, measured, error, passed, _ in rows:
            measured_text = "missing" if measured is None else f"{measured:.6f}"
            error_text = "invalid" if not math.isfinite(error) else f"{error:.2%}"
            stream.write(
                f"| {name} | {expected:.6f} | {measured_text} | {error_text} | "
                f"{'PASS' if passed else 'FAIL'} |\n"
            )
        stream.write("\n## Per-Dataset Diagnostics\n\n")
        stream.write("| Dataset | Metric | Measured | Relative error vs average | Diagnostic |\n")
        stream.write("|---|---|---:|---:|---|\n")
        for dataset in report.get("datasets", []):
            dataset_metrics = report.get("per_dataset", {}).get(dataset, {}).get("metrics", {})
            for name, definition in reference["metrics"].items():
                measured = dataset_metrics.get(name)
                expected = float(definition["reference"])
                if measured is None or not math.isfinite(measured):
                    error = math.inf
                else:
                    error = abs(measured - expected) / abs(expected)
                measured_text = "missing" if measured is None else f"{measured:.6f}"
                error_text = "invalid" if not math.isfinite(error) else f"{error:.2%}"
                diagnostic = "IN-RANGE" if error <= tolerance else "OUTLIER"
                stream.write(
                    f"| {dataset} | {name} | {measured_text} | {error_text} | {diagnostic} |\n"
                )
        stream.write("\n## Scope\n\n")
        stream.write("Validated: relative microarchitectural effects for GCN.\n\n")
        stream.write(
            "Not validated: absolute CPU/GPU speedup, DiffPool, or complete chip energy/area.\n"
        )

    for name, expected, measured, error, passed, source in rows:
        measured_text = "missing" if measured is None else f"{measured:.6f}"
        error_text = "invalid" if not math.isfinite(error) else f"{error:.2%}"
        print(
            f"{name}: reference={expected:.6f} measured={measured_text} "
            f"error={error_text} {'PASS' if passed else 'FAIL'} ({source})"
        )
    print(f"validation_report={output_path}")
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
