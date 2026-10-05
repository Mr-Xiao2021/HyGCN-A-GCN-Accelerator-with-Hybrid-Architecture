#!/usr/bin/env python3
import argparse
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="Test report figure reproduction")
    parser.add_argument("--script", default="tools/report_figures.py")
    return parser.parse_args()


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    script = Path(args.script)
    if not script.is_absolute():
        script = root / script
    with tempfile.TemporaryDirectory(prefix="hygcn-report-figure-") as temp:
        output_dir = Path(temp) / "output"
        completed = subprocess.run(
            [sys.executable, str(script), "--output-dir", str(output_dir),
             "--skip-raster"],
            cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(completed.stderr)
        with (output_dir / "report_summary.json").open(encoding="utf-8") as stream:
            summary = json.load(stream)
        if abs(summary["means"]["speedup"] - 275.0) > 1e-9:
            raise ValueError("speedup mean is not anchored to the report text")
        if abs(summary["means"]["energy_reduction"] - 4112.0) > 1e-9:
            raise ValueError("energy mean is not anchored to the report text")
        if summary["measurement_coverage"] != {
                "measured": 0, "missing": 15, "total": 15}:
            raise ValueError("unexpected no-binary measurement coverage")
        if len(summary["points"]) != 15:
            raise ValueError("report figure must contain 15 workloads")
        for axis in summary["axis_fits"].values():
            if abs(axis["uniform_adjustment_percent"]) >= 5.0:
                raise ValueError("PDF vector digitization needs excessive adjustment")
        with (output_dir / "report_metrics.csv").open(
                newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
        if [row["label"] for row in rows] != [
                point["label"] for point in summary["points"]]:
            raise ValueError("CSV workload order differs from the PDF")
        for filename in (
            "report_speedup.svg", "report_energy.svg", "report_comparison.svg",
            "REPRODUCTION.md",
        ):
            path = output_dir / filename
            if not path.is_file() or path.stat().st_size == 0:
                raise ValueError(f"missing generated artifact: {filename}")
        svg = (output_dir / "report_comparison.svg").read_text(encoding="utf-8")
        if "GCN-CS" not in svg or "GS-RD" not in svg or "HyGCN" not in svg:
            raise ValueError("combined SVG is missing expected labels")
    print("report_figure_pipeline=PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
