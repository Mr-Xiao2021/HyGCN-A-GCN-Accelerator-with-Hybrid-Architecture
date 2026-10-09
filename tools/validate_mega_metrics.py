#!/usr/bin/env python3
"""Independently recompute MEGA M0-M3 totals, ratios, and fairness gates."""

import argparse
import hashlib
import json
import math
from pathlib import Path


VARIANTS = ("m0", "m1", "m2", "m3")
SHARED_MANIFEST_FIELDS = (
    "binary_digest",
    "model",
    "dataset",
    "profile",
    "graph_digest",
    "config_digest",
    "quantization_digest",
    "quantization_manifest_version",
    "quantization_provenance",
    "feature_value_source",
    "partition_source",
    "partition_manifest_version",
    "partition_parameters",
    "partition_digest",
)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True)
    parser.add_argument("--output")
    parser.add_argument("--require-local-gate", action="store_true")
    return parser.parse_args()


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def close(left, right):
    return math.isclose(left, right, rel_tol=1e-12, abs_tol=1e-12)


def require(condition, message, errors):
    if not condition:
        errors.append(message)


def load_and_validate_run(path, expected_variant, errors):
    data = json.loads(path.read_text(encoding="utf-8"))
    manifest = data["manifest"]
    require(manifest["engine"] == "mega", f"{path}: engine is not mega", errors)
    require(manifest["variant"].startswith(expected_variant),
            f"{path}: variant mismatch", errors)
    layer_cycles = 0
    layer_bytes = 0
    layer_transactions = 0
    for index, layer in enumerate(data["layers"]):
        logical = layer["logical_bytes"]
        dram = layer["dram_bytes"]
        transactions = layer["dram_transactions"]
        cycles = layer["cycles"]
        classes = ("input", "weight", "edge", "cross_partition", "output")
        require(sum(logical[name] for name in classes) == logical["total"],
                f"{path}: layer {index} logical byte sum mismatch", errors)
        require(sum(dram[name] for name in classes) == dram["total"],
                f"{path}: layer {index} DRAM byte sum mismatch", errors)
        require(sum(transactions[name] for name in classes) == transactions["total"],
                f"{path}: layer {index} transaction sum mismatch", errors)
        transaction_bytes = data["architecture"]["transaction_bytes"]
        require(dram["total"] == transactions["total"] * transaction_bytes,
                f"{path}: layer {index} bytes/transaction mismatch", errors)
        require(
            cycles["total"]
            == cycles["memory_service"]
            + cycles["decoder"]
            + cycles["combination"]
            + cycles["condense"]
            + cycles["aggregation"]
            + cycles["encoder"],
            f"{path}: layer {index} cycle sum mismatch",
            errors,
        )
        layer_cycles += cycles["total"]
        layer_bytes += dram["total"]
        layer_transactions += transactions["total"]
    summary = data["summary"]
    require(layer_cycles == summary["total_cycles"], f"{path}: cycle summary mismatch", errors)
    require(layer_bytes == summary["total_dram_bytes"], f"{path}: byte summary mismatch", errors)
    require(layer_transactions == summary["total_dram_transactions"],
            f"{path}: transaction summary mismatch", errors)
    return data


def validate(report_path, require_local_gate=False):
    output_dir = report_path.parent
    report = json.loads(report_path.read_text(encoding="utf-8"))
    errors = []
    require(report.get("schema_version") == 1, "unsupported report schema", errors)
    artifact_hashes = {item["path"]: item["sha256"] for item in report["artifacts"]}
    for relative, expected in artifact_hashes.items():
        path = output_dir / relative
        require(path.is_file(), f"missing artifact: {relative}", errors)
        if path.is_file():
            require(sha256(path) == expected, f"artifact hash mismatch: {relative}", errors)

    recomputed_speedups = []
    recomputed_reductions = []
    all_gates = True
    for dataset, item in report["datasets"].items():
        runs = {}
        for variant in VARIANTS:
            path = output_dir / item["runs"][variant]
            runs[variant] = load_and_validate_run(path, variant, errors)
        reference_architecture = runs["m0"]["architecture"]
        reference_manifest = runs["m0"]["manifest"]
        for variant in VARIANTS[1:]:
            require(runs[variant]["architecture"] == reference_architecture,
                    f"{dataset}: baseline-only architecture difference in {variant}", errors)
            for field in SHARED_MANIFEST_FIELDS:
                require(runs[variant]["manifest"].get(field) == reference_manifest.get(field),
                        f"{dataset}: shared manifest field {field} differs in {variant}", errors)

        metrics = {
            variant: {
                "cycles": runs[variant]["summary"]["total_cycles"],
                "bytes": runs[variant]["summary"]["total_dram_bytes"],
                "transactions": runs[variant]["summary"]["total_dram_transactions"],
            }
            for variant in VARIANTS
        }
        for variant in VARIANTS:
            reported = item["variants"][variant]
            require(metrics[variant]["cycles"] == reported["total_cycles"],
                    f"{dataset}: {variant} cycle report mismatch", errors)
            require(metrics[variant]["bytes"] == reported["total_dram_bytes"],
                    f"{dataset}: {variant} byte report mismatch", errors)
            require(metrics[variant]["transactions"] == reported["total_dram_transactions"],
                    f"{dataset}: {variant} transaction report mismatch", errors)
        for baseline, optimized in (("m0", "m1"), ("m1", "m2"), ("m2", "m3"), ("m0", "m3")):
            name = f"{optimized}_vs_{baseline}"
            reported = item["comparisons"][name]
            computed = {
                "speedup": metrics[baseline]["cycles"] / metrics[optimized]["cycles"],
                "dram_reduction": metrics[baseline]["bytes"] / metrics[optimized]["bytes"],
                "transaction_reduction": metrics[baseline]["transactions"]
                / metrics[optimized]["transactions"],
            }
            for metric, value in computed.items():
                require(close(value, reported[metric]),
                        f"{dataset}: {name} {metric} mismatch", errors)
        gate = metrics["m3"]["bytes"] < metrics["m0"]["bytes"] and metrics["m3"]["cycles"] < metrics["m0"]["cycles"]
        require(gate == item["local_mechanism_gate"]["pass"],
                f"{dataset}: local gate mismatch", errors)
        all_gates = all_gates and gate
        recomputed_speedups.append(metrics["m0"]["cycles"] / metrics["m3"]["cycles"])
        recomputed_reductions.append(metrics["m0"]["bytes"] / metrics["m3"]["bytes"])

    aggregate = report["aggregate"]
    require(close(sum(recomputed_speedups) / len(recomputed_speedups), aggregate["mean_speedup"]),
            "aggregate speedup mismatch", errors)
    require(close(sum(recomputed_reductions) / len(recomputed_reductions), aggregate["mean_dram_reduction"]),
            "aggregate DRAM reduction mismatch", errors)
    require(all_gates == aggregate["all_local_gates_pass"], "aggregate gate mismatch", errors)
    if require_local_gate:
        require(all_gates, "one or more local mechanism gates failed", errors)
    return report, errors


def main():
    args = parse_args()
    report_path = Path(args.report).resolve()
    report, errors = validate(report_path, args.require_local_gate)
    lines = ["# MEGA Validation", ""]
    if errors:
        lines.append("Result: FAIL")
        lines.extend(f"- {error}" for error in errors)
    else:
        lines.extend(
            (
                "Result: PASS",
                f"Datasets: {', '.join(report['datasets'])}",
                f"Mean M3/M0 speedup: {report['aggregate']['mean_speedup']:.6f}x",
                f"Mean M3/M0 DRAM reduction: {report['aggregate']['mean_dram_reduction']:.6f}x",
                "Claim boundary: diagnostic local mechanism result",
            )
        )
    text = "\n".join(lines) + "\n"
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
    print(text, end="")
    raise SystemExit(1 if errors else 0)


if __name__ == "__main__":
    main()
