#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="Test report legacy evidence fields")
    parser.add_argument("--binary", default="build/hygcntest")
    return parser.parse_args()


def run(binary, root, graph_dir, output_dir):
    completed = subprocess.run(
        [
            str(binary), "--engine", "legacy", "--profile", "legacy",
            "--model", "gs", "--dataset", "test", "--graph-dir", str(graph_dir),
            "--seed", "1", "--output-dir", str(output_dir), "--quiet",
        ],
        cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(completed.stderr)
    with (output_dir / "legacy_gs_test_seed-1.json").open(encoding="utf-8") as stream:
        return json.load(stream)


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    binary = Path(args.binary)
    if not binary.is_absolute():
        binary = root / binary
    with tempfile.TemporaryDirectory(prefix="hygcn-report-legacy-") as temp:
        temp_path = Path(temp)
        graph_dir = temp_path / "graph"
        graph_dir.mkdir()
        (graph_dir / "test.txt").write_text(
            "edge,6\nfeature,16\nvertex,4\nclass,2\n", encoding="utf-8"
        )
        (graph_dir / "test_edge.csv").write_text(
            "0,1\n1,0\n1,2\n2,1\n2,3\n3,2\n", encoding="utf-8"
        )
        first = run(binary, root, graph_dir, temp_path / "first")
        second = run(binary, root, graph_dir, temp_path / "second")
        if first["manifest"]["graphsage_sample_source"] != \
                "deterministic-neighbor-sample-v1":
            raise ValueError("GraphSAGE did not use the deterministic fallback")
        if first["summary"] != second["summary"] or first["layers"] != second["layers"]:
            raise ValueError("deterministic GraphSAGE fallback changed across runs")
        summary = first["summary"]
        if summary["total_cycles"] <= 0 or summary["total_dram_energy_pj"] <= 0:
            raise ValueError("legacy summary lacks positive cycle or energy evidence")
        layer_energy = sum(layer["dram_energy_pj"] for layer in first["layers"])
        if abs(layer_energy - summary["total_dram_energy_pj"]) > 1e-6 * layer_energy:
            raise ValueError("layer DRAM energy does not reconcile with the summary")
    print("report_legacy_evidence=PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
