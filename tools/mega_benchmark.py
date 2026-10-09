#!/usr/bin/env python3
"""Run the fixed MEGA M0-M3 ablation and build a recomputable report."""

import argparse
import csv
import hashlib
import json
import statistics
import subprocess
import sys
from pathlib import Path


VARIANTS = (
    ("m0", "m0-fp32-axw"),
    ("m1", "m1-degree-aware-bitmap"),
    ("m2", "m2-adaptive-package"),
    ("m3", "m3-condense-edge"),
)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--graph-dir", default="gcn_dataset")
    parser.add_argument("--datasets", nargs="+", default=("cora", "citeseer", "pubmed"))
    parser.add_argument("--model", choices=("gcn", "gin", "gs"), default="gcn")
    parser.add_argument("--profile", choices=("paper", "smoke"), default="paper")
    parser.add_argument("--output-dir", default="res/mega")
    parser.add_argument("--force", action="store_true")
    return parser.parse_args()


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command, cwd):
    subprocess.run(command, cwd=cwd, check=True)


def result_path(raw_dir, profile, model, dataset, variant_name):
    return raw_dir / f"mega_{profile}_{model}_{dataset}_{variant_name}.json"


def ratio(numerator, denominator):
    if denominator <= 0:
        raise ValueError("metric denominator must be positive")
    return numerator / denominator


def summarize_dataset(results):
    variants = {}
    for variant_id, _ in VARIANTS:
        result = results[variant_id]
        variants[variant_id] = {
            "total_cycles": result["summary"]["total_cycles"],
            "total_dram_bytes": result["summary"]["total_dram_bytes"],
            "total_dram_transactions": result["summary"]["total_dram_transactions"],
        }
    comparisons = {}
    for baseline, optimized in (("m0", "m1"), ("m1", "m2"), ("m2", "m3"), ("m0", "m3")):
        key = f"{optimized}_vs_{baseline}"
        comparisons[key] = {
            "speedup": ratio(
                variants[baseline]["total_cycles"], variants[optimized]["total_cycles"]
            ),
            "dram_reduction": ratio(
                variants[baseline]["total_dram_bytes"],
                variants[optimized]["total_dram_bytes"],
            ),
            "transaction_reduction": ratio(
                variants[baseline]["total_dram_transactions"],
                variants[optimized]["total_dram_transactions"],
            ),
        }
    gate = {
        "dram_bytes_reduced": variants["m3"]["total_dram_bytes"] < variants["m0"]["total_dram_bytes"],
        "total_cycles_reduced": variants["m3"]["total_cycles"] < variants["m0"]["total_cycles"],
    }
    gate["pass"] = all(gate.values())
    return {"variants": variants, "comparisons": comparisons, "local_mechanism_gate": gate}


def write_csv(report, path):
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(
            (
                "dataset",
                "variant",
                "total_cycles",
                "total_dram_bytes",
                "total_dram_transactions",
                "m3_vs_m0_speedup",
                "m3_vs_m0_dram_reduction",
                "local_gate",
            )
        )
        for dataset, item in report["datasets"].items():
            final = item["comparisons"]["m3_vs_m0"]
            for variant_id, metrics in item["variants"].items():
                writer.writerow(
                    (
                        dataset,
                        variant_id,
                        metrics["total_cycles"],
                        metrics["total_dram_bytes"],
                        metrics["total_dram_transactions"],
                        final["speedup"],
                        final["dram_reduction"],
                        item["local_mechanism_gate"]["pass"],
                    )
                )


def write_markdown(report, path):
    lines = [
        "# MEGA Local Mechanism Report",
        "",
        "All rows use diagnostic quantization inputs. The ratios are local M0-M3 mechanism results, not a full-paper reproduction claim.",
        "",
        "| Dataset | M3/M0 speedup | M3/M0 DRAM reduction | Transaction reduction | Gate |",
        "|---|---:|---:|---:|---|",
    ]
    for dataset, item in report["datasets"].items():
        final = item["comparisons"]["m3_vs_m0"]
        lines.append(
            f"| {dataset} | {final['speedup']:.6f}x | {final['dram_reduction']:.6f}x | "
            f"{final['transaction_reduction']:.6f}x | "
            f"{'PASS' if item['local_mechanism_gate']['pass'] else 'FAIL'} |"
        )
    aggregate = report["aggregate"]
    lines.extend(
        (
            "",
            f"Arithmetic-mean speedup: {aggregate['mean_speedup']:.6f}x",
            f"Arithmetic-mean DRAM reduction: {aggregate['mean_dram_reduction']:.6f}x",
            f"Conclusion: {report['conclusion_level']}",
            "",
            "Paper reference values are retained for comparison only because author per-node quantization artifacts and the full five-workload set are unavailable.",
        )
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    binary = Path(args.binary)
    if not binary.is_absolute():
        binary = (root / binary).resolve()
    graph_dir = Path(args.graph_dir)
    if not graph_dir.is_absolute():
        graph_dir = (root / graph_dir).resolve()
    output_dir = Path(args.output_dir)
    if not output_dir.is_absolute():
        output_dir = (root / output_dir).resolve()
    raw_dir = output_dir / "raw"
    manifest_dir = output_dir / "manifests"
    raw_dir.mkdir(parents=True, exist_ok=True)
    manifest_dir.mkdir(parents=True, exist_ok=True)

    report = {
        "schema_version": 1,
        "benchmark": "mega-m0-m3-local-mechanism-v1",
        "conclusion_level": "diagnostic-local-mechanism",
        "model": args.model,
        "profile": args.profile,
        "datasets": {},
        "artifacts": [],
        "paper_reference": {
            "speedup_vs_hygcn_arithmetic_mean": 38.3,
            "dram_reduction_vs_hygcn_arithmetic_mean": 108.1,
            "eligible_for_paper_numeric_claim": False,
            "reason": "diagnostic quantization and three exposed workloads",
        },
    }

    for dataset in args.datasets:
        manifest = manifest_dir / f"{args.model}_{dataset}_diagnostic_quantization.json"
        if args.force or not manifest.exists():
            run(
                (
                    sys.executable,
                    str(root / "tools" / "mega_quantization.py"),
                    "--graph-dir",
                    str(graph_dir),
                    "--dataset",
                    dataset,
                    "--model",
                    args.model,
                    "--output",
                    str(manifest),
                ),
                root,
            )
        results = {}
        run_paths = {}
        for variant_id, variant_name in VARIANTS:
            path = result_path(raw_dir, args.profile, args.model, dataset, variant_name)
            if args.force or not path.exists():
                run(
                    (
                        str(binary),
                        "--engine",
                        "mega",
                        "--profile",
                        args.profile,
                        "--model",
                        args.model,
                        "--dataset",
                        dataset,
                        "--graph-dir",
                        str(graph_dir),
                        "--quantization-manifest",
                        str(manifest),
                        "--mega-variant",
                        variant_id,
                        "--output-dir",
                        str(raw_dir),
                        "--quiet",
                    ),
                    root,
                )
            results[variant_id] = json.loads(path.read_text(encoding="utf-8"))
            run_paths[variant_id] = str(path.relative_to(output_dir))
            report["artifacts"].append(
                {"path": str(path.relative_to(output_dir)), "sha256": sha256(path)}
            )
            csv_path = path.with_suffix(".csv")
            report["artifacts"].append(
                {"path": str(csv_path.relative_to(output_dir)), "sha256": sha256(csv_path)}
            )
        dataset_summary = summarize_dataset(results)
        dataset_summary["runs"] = run_paths
        dataset_summary["quantization_manifest"] = str(manifest.relative_to(output_dir))
        dataset_summary["quantization_sha256"] = sha256(manifest)
        report["datasets"][dataset] = dataset_summary
        report["artifacts"].append(
            {"path": str(manifest.relative_to(output_dir)), "sha256": sha256(manifest)}
        )

    final_metrics = [
        item["comparisons"]["m3_vs_m0"] for item in report["datasets"].values()
    ]
    report["aggregate"] = {
        "mean_speedup": statistics.fmean(item["speedup"] for item in final_metrics),
        "mean_dram_reduction": statistics.fmean(
            item["dram_reduction"] for item in final_metrics
        ),
        "all_local_gates_pass": all(
            item["local_mechanism_gate"]["pass"]
            for item in report["datasets"].values()
        ),
    }

    report_path = output_dir / "benchmark_report.json"
    csv_path = output_dir / "benchmark_report.csv"
    markdown_path = output_dir / "benchmark_report.md"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    write_csv(report, csv_path)
    write_markdown(report, markdown_path)
    print(f"mega_report={report_path}")
    print(f"local_gates_pass={str(report['aggregate']['all_local_gates_pass']).lower()}")


if __name__ == "__main__":
    main()
