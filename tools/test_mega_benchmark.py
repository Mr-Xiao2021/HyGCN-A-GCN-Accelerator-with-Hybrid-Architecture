#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    return parser.parse_args()


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="mega-benchmark-test-") as directory:
        output = Path(directory)
        subprocess.run(
            (
                sys.executable,
                str(root / "tools" / "mega_benchmark.py"),
                "--binary",
                args.binary,
                "--datasets",
                "test",
                "--profile",
                "smoke",
                "--output-dir",
                str(output),
                "--force",
            ),
            cwd=root,
            check=True,
        )
        subprocess.run(
            (
                sys.executable,
                str(root / "tools" / "validate_mega_metrics.py"),
                "--report",
                str(output / "benchmark_report.json"),
                "--require-local-gate",
            ),
            cwd=root,
            check=True,
        )
        report = json.loads((output / "benchmark_report.json").read_text(encoding="utf-8"))
        assert report["datasets"]["test"]["local_mechanism_gate"]["pass"]
        assert report["conclusion_level"] == "diagnostic-local-mechanism"
        tampered = output / report["datasets"]["test"]["runs"]["m3"]
        data = json.loads(tampered.read_text(encoding="utf-8"))
        data["summary"]["total_dram_bytes"] += 1
        tampered.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        failed = subprocess.run(
            (
                sys.executable,
                str(root / "tools" / "validate_mega_metrics.py"),
                "--report",
                str(output / "benchmark_report.json"),
            ),
            cwd=root,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        assert failed.returncode != 0
        assert "hash mismatch" in failed.stdout or "summary mismatch" in failed.stdout
    print("mega benchmark and validator self-test PASS")


if __name__ == "__main__":
    main()
