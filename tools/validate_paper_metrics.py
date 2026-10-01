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


def relative_error(measured, expected):
    if measured is None or not isinstance(measured, (int, float)):
        return math.inf
    if not math.isfinite(measured) or expected == 0:
        return math.inf
    return abs(measured - expected) / abs(expected)


def range_error(measured, lower, upper):
    if measured is None or not isinstance(measured, (int, float)) or not math.isfinite(measured):
        return math.inf
    if lower <= measured <= upper:
        return 0.0
    boundary = lower if measured < lower else upper
    return abs(measured - boundary) / abs(boundary)


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
    per_dataset = report.get("per_dataset", {})
    rows = []
    diagnostics = []
    failed = False

    for name, definition in reference["metrics"].items():
        validation = definition["validation"]
        required = definition["required"]
        if validation == "aggregate_relative_error":
            measured = aggregate.get(name)
            expected = float(definition["reference"])
            error = relative_error(measured, expected)
            passed = error <= tolerance
            failed = failed or (required and not passed)
            rows.append((name, "aggregate", f"{expected:.6f}", measured, error, passed,
                         definition["source"], definition["evidence_class"]))
        elif validation == "per_dataset_relative_error":
            references = definition["reference"]
            for dataset in reference["datasets"]:
                measured = per_dataset.get(dataset, {}).get("metrics", {}).get(name)
                expected = float(references[dataset])
                error = relative_error(measured, expected)
                passed = error <= tolerance
                failed = failed or (required and not passed)
                rows.append((name, dataset, f"{expected:.6f}", measured, error,
                             passed, definition["source"], definition["evidence_class"]))
        elif validation == "per_dataset_range":
            lower = float(definition["reference_min"])
            upper = float(definition["reference_max"])
            for dataset in reference["datasets"]:
                measured = per_dataset.get(dataset, {}).get("metrics", {}).get(name)
                error = range_error(measured, lower, upper)
                passed = error <= tolerance
                failed = failed or (required and not passed)
                rows.append((name, dataset, f"{lower:.6f}-{upper:.6f}", measured, error,
                             passed, definition["source"], definition["evidence_class"]))
        elif validation == "per_dataset_minimum":
            lower = float(definition["reference_min"])
            for dataset in reference["datasets"]:
                measured = per_dataset.get(dataset, {}).get("metrics", {}).get(name)
                error = range_error(measured, lower, math.inf)
                passed = error == 0.0
                failed = failed or (required and not passed)
                rows.append((name, dataset, f">={lower:.6f}", measured, error,
                             passed, definition["source"], definition["evidence_class"]))
        elif validation == "aggregate_minimum":
            lower = float(definition["reference_min"])
            measured = aggregate.get(name)
            error = range_error(measured, lower, math.inf)
            passed = error == 0.0
            failed = failed or (required and not passed)
            rows.append((name, "aggregate", f">={lower:.6f}", measured, error,
                         passed, definition["source"], definition["evidence_class"]))
        elif validation == "diagnostic_only":
            for dataset in reference["datasets"]:
                measured = per_dataset.get(dataset, {}).get("metrics", {}).get(name)
                diagnostics.append((name, dataset, measured, definition["source"]))
        else:
            raise ValueError(f"unsupported validation rule for {name}: {validation}")

    paper_rows = [row for row in rows if row[7] == "paper_metric"]
    internal_rows = [row for row in rows if row[7] == "internal_check"]
    if len(paper_rows) != 14 or len(internal_rows) != 12:
        raise ValueError("validation requires 14 paper metric rows and 12 internal checks")
    legacy_row_hit_name = "priority_incremental_row_hit_ratio_aggregate"
    legacy_row_hit_threshold = 1.05
    legacy_internal_passed = 0
    legacy_row_hit_measured = aggregate.get(legacy_row_hit_name)
    legacy_row_hit_passed = False
    for row in internal_rows:
        if row[0] == legacy_row_hit_name:
            legacy_row_hit_passed = range_error(
                legacy_row_hit_measured, legacy_row_hit_threshold, math.inf
            ) == 0.0
            legacy_internal_passed += legacy_row_hit_passed
        else:
            legacy_internal_passed += row[5]
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as stream:
        stream.write("# HyGCN Paper Metric And Internal Check Validation\n\n")
        stream.write(f"Tolerance: {tolerance:.0%}\n\n")
        stream.write(
            "Digitized bars are checked against their dataset-specific references. "
            "Paper-reported averages are checked only after applying the declared "
            "aggregation rule. All three datasets are historically exposed calibrated-fit "
            "workloads; this report does not claim an independent hold-out. Internal checks "
            "and diagnostic-only metrics are not paper evidence.\n\n"
        )
        stream.write("## Paper Metrics (14 rows)\n\n")
        stream.write("| Metric | Scope | Reference | Measured | Relative error | Status |\n")
        stream.write("|---|---|---:|---:|---:|---|\n")
        for name, scope, expected, measured, error, passed, _, _ in paper_rows:
            measured_text = "missing" if measured is None else f"{measured:.6f}"
            error_text = "invalid" if not math.isfinite(error) else f"{error:.2%}"
            stream.write(
                f"| {name} | {scope} | {expected} | {measured_text} | {error_text} | "
                f"{'PASS' if passed else 'FAIL'} |\n"
            )
        stream.write("\n## Internal Regression Checks (12 rows)\n\n")
        stream.write("| Check | Scope | Threshold | Measured | Deviation | Status |\n")
        stream.write("|---|---|---:|---:|---:|---|\n")
        for name, scope, expected, measured, error, passed, _, _ in internal_rows:
            measured_text = "missing" if measured is None else f"{measured:.6f}"
            error_text = "invalid" if not math.isfinite(error) else f"{error:.2%}"
            stream.write(
                f"| {name} | {scope} | {expected} | {measured_text} | {error_text} | "
                f"{'PASS' if passed else 'FAIL'} |\n"
            )
        legacy_measured_text = (
            "missing" if legacy_row_hit_measured is None
            else f"{legacy_row_hit_measured:.6f}"
        )
        stream.write("\n## Historical Schema v5 Comparison\n\n")
        stream.write(
            f"- Current schema v{reference['schema_version']}: "
            f"{sum(row[5] for row in internal_rows)}/12 internal checks PASS.\n"
        )
        stream.write(
            f"- Historical schema v5: {legacy_internal_passed}/12 internal checks PASS. "
            "It used the unsupported aggregate row-hit floor 1.05x; this is retained "
            "as an audit comparison, not as the current acceptance specification.\n\n"
        )
        stream.write("| Historical check | Threshold | Measured | Status |\n")
        stream.write("|---|---:|---:|---|\n")
        stream.write(
            f"| {legacy_row_hit_name} | >=1.050000 | {legacy_measured_text} | "
            f"{'PASS' if legacy_row_hit_passed else 'FAIL'} |\n"
        )
        stream.write("\n## Diagnostic-Only Metrics\n\n")
        stream.write("| Metric | Dataset | Measured | Reason |\n")
        stream.write("|---|---|---:|---|\n")
        for name, dataset, measured, source in diagnostics:
            measured_text = "missing" if measured is None else f"{measured:.6f}"
            stream.write(f"| {name} | {dataset} | {measured_text} | {source} |\n")
        stream.write("\n## Scope\n\n")
        stream.write(
            "This is a request-level GCN mechanism check. It is not a cycle-accurate "
            "Ramulator reproduction and does not validate CPU/GPU speedup, DiffPool, "
            "area, or complete chip energy.\n"
        )

    for name, scope, expected, measured, error, passed, source, evidence_class in rows:
        measured_text = "missing" if measured is None else f"{measured:.6f}"
        error_text = "invalid" if not math.isfinite(error) else f"{error:.2%}"
        print(
            f"{name}[{scope}]: reference={expected} measured={measured_text} "
            f"error={error_text} {'PASS' if passed else 'FAIL'} "
            f"class={evidence_class} ({source})"
        )
    paper_passed = sum(row[5] for row in paper_rows)
    internal_passed = sum(row[5] for row in internal_rows)
    print(f"paper_metrics={paper_passed}/14")
    print(f"internal_checks={internal_passed}/12")
    print(f"internal_checks_schema_v{reference['schema_version']}={internal_passed}/12")
    print(f"internal_checks_schema_v5={legacy_internal_passed}/12")
    print(f"validation_report={output_path}")
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
