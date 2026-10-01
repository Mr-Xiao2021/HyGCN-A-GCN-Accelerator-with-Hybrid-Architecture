#!/usr/bin/env python3
import argparse
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import paper_benchmark
import test_paper_benchmark


def parse_args():
    parser = argparse.ArgumentParser(
        description="Mutation tests for the compiled full-trace validator"
    )
    parser.add_argument("--binary", required=True)
    parser.add_argument("--validator", required=True)
    return parser.parse_args()


def run_validator(validator, path, expect_success):
    completed = subprocess.run(
        [str(validator), str(path)],
        capture_output=True,
        text=True,
    )
    if (completed.returncode == 0) != expect_success:
        raise RuntimeError(
            f"validator {'accepted' if completed.returncode == 0 else 'rejected'} "
            f"{path.name}: {completed.stderr.strip()}"
        )


def write_case(directory, name, document):
    path = directory / f"{name}.json"
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(document, stream, separators=(",", ":"))
        stream.write("\n")
    return path


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    binary = Path(args.binary).resolve()
    validator = Path(args.validator).resolve()
    with tempfile.TemporaryDirectory(prefix="hygcn-trace-validator-") as temporary:
        directory = Path(temporary)
        subprocess.run(
            [
                str(binary),
                "--engine", "paper",
                "--profile", "smoke",
                "--model", "gcn",
                "--dataset", "test",
                "--scope", "full",
                "--layer", "0",
                "--pipeline", "latency-aware",
                "--combination", "independent",
                "--sparsity", "on",
                "--priority", "batch-class",
                "--mapping", "low-bits",
                "--seed", "1",
                "--output-dir", str(directory),
                "--quiet",
            ],
            cwd=root,
            check=True,
        )
        raw_paths = sorted(directory.glob("*.json"))
        if len(raw_paths) != 1:
            raise RuntimeError("smoke run did not produce exactly one JSON result")
        raw_path = raw_paths[0]
        document = paper_benchmark.load_json(raw_path)
        run_validator(validator, raw_path, True)
        layer = document["layers"][0]
        events = list(paper_benchmark.iter_command_trace(layer["command_trace"]))
        events = [
            {key: value for key, value in event.items() if key != "command_id"}
            for event in events
        ]
        if len(events) < 6:
            raise RuntimeError("smoke command trace is too short for mutation tests")

        deleted = copy.deepcopy(document)
        deleted_events = events[:len(events) // 2] + events[len(events) // 2 + 1:]
        deleted["layers"][0]["command_trace"] = \
            test_paper_benchmark.command_trace_summary(deleted_events)
        run_validator(
            validator, write_case(directory, "deleted-event", deleted), False
        )

        changed_cycle = copy.deepcopy(document)
        changed_events = copy.deepcopy(events)
        index = len(changed_events) // 2
        changed_events[index]["cycle"] = changed_events[index - 1]["cycle"]
        changed_cycle["layers"][0]["command_trace"] = \
            test_paper_benchmark.command_trace_summary(changed_events)
        run_validator(
            validator, write_case(directory, "changed-middle-cycle", changed_cycle),
            False,
        )

        empty_samples = copy.deepcopy(document)
        empty_samples["layers"][0]["command_trace"]["samples"] = []
        run_validator(
            validator, write_case(directory, "empty-samples", empty_samples), False
        )

        fake_checksum = copy.deepcopy(document)
        fake_checksum["layers"][0]["command_trace"]["checksum_fnv1a64"] = "0" * 16
        run_validator(
            validator, write_case(directory, "fake-checksum", fake_checksum), False
        )
    print("compiled_trace_validator_mutations=4/4_REJECTED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError,
            json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
