#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def run_case(root, reference, aggregate, expected_returncode, name, directory):
    report = {
        "schema_version": 1,
        "datasets": reference["datasets"],
        "aggregate": aggregate,
        "per_dataset": {},
    }
    report_path = directory / f"{name}.json"
    output_path = directory / f"{name}.md"
    with report_path.open("w", encoding="utf-8") as stream:
        json.dump(report, stream, allow_nan=True)
    completed = subprocess.run(
        [
            sys.executable,
            str(root / "tools/validate_paper_metrics.py"),
            "--reference", str(root / "configs/paper_metrics.json"),
            "--report", str(report_path),
            "--output", str(output_path),
        ],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if completed.returncode != expected_returncode:
        raise RuntimeError(
            f"{name}: expected return code {expected_returncode}, got "
            f"{completed.returncode}: {completed.stdout}{completed.stderr}"
        )


def main():
    root = Path(__file__).resolve().parents[1]
    with (root / "configs/paper_metrics.json").open(encoding="utf-8") as stream:
        reference = json.load(stream)
    valid = {name: definition["reference"] for name, definition in reference["metrics"].items()}
    with tempfile.TemporaryDirectory(prefix="hygcn-validator-") as temp:
        directory = Path(temp)
        run_case(root, reference, valid, 0, "valid", directory)
        missing = dict(valid)
        missing.pop("pipeline_speedup")
        run_case(root, reference, missing, 1, "missing", directory)
        non_finite = dict(valid)
        non_finite["pipeline_speedup"] = float("nan")
        run_case(root, reference, non_finite, 1, "non-finite", directory)
        out_of_range = dict(valid)
        out_of_range["pipeline_speedup"] = 2.0
        run_case(root, reference, out_of_range, 1, "out-of-range", directory)
    print("metric_validator_selftest=PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
