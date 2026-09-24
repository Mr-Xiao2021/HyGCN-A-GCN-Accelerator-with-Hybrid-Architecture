#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


LAYER_FIELDS = (
    "layer",
    "cycles",
    "dram_edge_read",
    "dram_input_read",
    "dram_weight_read",
    "dram_output_write",
)


def parse_args():
    parser = argparse.ArgumentParser(description="Check legacy HyGCN snapshots")
    parser.add_argument("--binary", default="build/hygcntest")
    parser.add_argument("--fixture", default="tests/data/legacy_baseline.json")
    parser.add_argument("--datasets", nargs="*")
    parser.add_argument("--output-dir")
    parser.add_argument("--fixture-only", action="store_true")
    return parser.parse_args()


def normalize(result):
    return {
        "summary": {"total_cycles": result["summary"]["total_cycles"]},
        "layers": [
            {field: layer[field] for field in LAYER_FIELDS}
            for layer in result["layers"]
        ],
    }


def validate_fixture(fixture):
    if fixture.get("schema_version") != 1:
        raise ValueError("legacy fixture schema_version must be 1")
    if fixture.get("engine") != "legacy" or fixture.get("model") != "gcn":
        raise ValueError("legacy fixture must describe the legacy GCN engine")
    if fixture.get("seed") != 1:
        raise ValueError("legacy fixture seed must be 1")
    if set(fixture.get("datasets", {})) != {"cora", "citeseer"}:
        raise ValueError("legacy fixture must include Cora and Citeseer")
    for dataset, expected in fixture["datasets"].items():
        if expected.get("summary", {}).get("total_cycles", 0) <= 0:
            raise ValueError(f"{dataset} has an invalid cycle baseline")
        if len(expected.get("layers", [])) != 2:
            raise ValueError(f"{dataset} must contain two layer snapshots")
        for layer in expected["layers"]:
            for field in LAYER_FIELDS:
                if field not in layer or layer[field] < 0:
                    raise ValueError(f"{dataset} layer is missing a valid {field}")


def run(binary, root, output_dir, dataset):
    subprocess.run(
        [
            str(binary),
            "--engine", "legacy",
            "--profile", "legacy",
            "--model", "gcn",
            "--dataset", dataset,
            "--seed", "1",
            "--output-dir", str(output_dir),
            "--quiet",
        ],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=True,
    )
    result_path = output_dir / f"legacy_gcn_{dataset}_seed-1.json"
    with result_path.open(encoding="utf-8") as stream:
        return json.load(stream)


def check_datasets(args, root, binary, fixture, output_dir):
    datasets = args.datasets or list(fixture["datasets"])
    unknown = set(datasets) - set(fixture["datasets"])
    if unknown:
        raise ValueError(f"datasets absent from fixture: {sorted(unknown)}")
    for dataset in datasets:
        actual = normalize(run(binary, root, output_dir, dataset))
        expected = fixture["datasets"][dataset]
        if actual != expected:
            raise ValueError(
                f"legacy snapshot mismatch for {dataset}:\n"
                f"expected={json.dumps(expected, sort_keys=True)}\n"
                f"actual={json.dumps(actual, sort_keys=True)}"
            )
        print(f"legacy_snapshot={dataset}:PASS")


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    fixture_path = Path(args.fixture)
    if not fixture_path.is_absolute():
        fixture_path = root / fixture_path
    with fixture_path.open(encoding="utf-8") as stream:
        fixture = json.load(stream)
    validate_fixture(fixture)
    if args.fixture_only:
        print(f"legacy_fixture=PASS path={fixture_path}")
        return 0

    binary = Path(args.binary)
    if not binary.is_absolute():
        binary = root / binary
    if args.output_dir:
        output_dir = Path(args.output_dir)
        if not output_dir.is_absolute():
            output_dir = root / output_dir
        output_dir.mkdir(parents=True, exist_ok=True)
        check_datasets(args, root, binary, fixture, output_dir)
    else:
        with tempfile.TemporaryDirectory(prefix="hygcn-legacy-") as temp:
            check_datasets(args, root, binary, fixture, Path(temp))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, json.JSONDecodeError,
            subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
