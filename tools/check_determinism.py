#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="Check deterministic HyGCN paper output")
    parser.add_argument("--binary", default="build/hygcntest")
    return parser.parse_args()


def run(binary, root, output_dir):
    subprocess.run(
        [
            str(binary),
            "--engine", "paper",
            "--profile", "smoke",
            "--model", "gcn",
            "--dataset", "test",
            "--pipeline", "latency-aware",
            "--combination", "independent",
            "--sparsity", "on",
            "--coordination", "on",
            "--seed", "17",
            "--output-dir", str(output_dir),
            "--quiet",
        ],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=True,
    )
    stem = "paper_smoke_gcn_test_latency-aware_independent_sparse-on_coord-on_seed-17"
    return output_dir / f"{stem}.json", output_dir / f"{stem}.csv"


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    binary = Path(args.binary)
    if not binary.is_absolute():
        binary = root / binary
    with tempfile.TemporaryDirectory(prefix="hygcn-determinism-a-") as first_temp, \
            tempfile.TemporaryDirectory(prefix="hygcn-determinism-b-") as second_temp:
        first_json, first_csv = run(binary, root, Path(first_temp))
        second_json, second_csv = run(binary, root, Path(second_temp))
        if first_json.read_bytes() != second_json.read_bytes():
            raise RuntimeError("repeated JSON outputs differ")
        if first_csv.read_bytes() != second_csv.read_bytes():
            raise RuntimeError("repeated CSV outputs differ")
        with first_json.open(encoding="utf-8") as stream:
            result = json.load(stream)
        manifest = result["manifest"]
        for field in ("git_commit", "binary_digest", "graph_digest", "config_digest", "seed",
                      "pipeline", "combination", "sparsity_elimination",
                      "memory_coordination"):
            if field not in manifest:
                raise RuntimeError(f"manifest is missing {field}")
        for layer in result["layers"]:
            if set(layer["request_stats"]) != {"edge", "input", "weight", "output"}:
                raise RuntimeError("structured request statistics are incomplete")
    print("paper_output_determinism=PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, KeyError, json.JSONDecodeError,
            subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
