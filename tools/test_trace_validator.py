#!/usr/bin/env python3
import argparse
import collections
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


def run_validator(validator, path, expect_success, expected_error=None):
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
    if expected_error is not None and expected_error not in completed.stderr:
        raise RuntimeError(
            f"validator rejected {path.name} for the wrong reason: "
            f"{completed.stderr.strip()}"
        )


def write_case(directory, name, document):
    path = directory / f"{name}.json"
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(document, stream, separators=(",", ":"))
        stream.write("\n")
    return path


def apply_command_events(document, events, synchronize_counters=True):
    layer = document["layers"][0]
    layer["command_trace"] = test_paper_benchmark.command_trace_summary(events)
    if synchronize_counters:
        counts = collections.Counter(event["command"] for event in events)
        layer["precharge_commands"] = counts["PRE"]
        layer["activate_commands"] = counts["ACT"]
        layer["read_commands"] = counts["READ"]
        layer["write_commands"] = counts["WRITE"]


def first_data_index(events):
    return next(
        index for index, event in enumerate(events)
        if event["command"] in {"READ", "WRITE"}
    )


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
        deleted_index = first_data_index(events)
        deleted_events = events[:deleted_index] + events[deleted_index + 1:]
        apply_command_events(deleted, deleted_events)
        run_validator(
            validator, write_case(directory, "deleted-event", deleted), False
        )

        changed_cycle = copy.deepcopy(document)
        changed_events = copy.deepcopy(events)
        index = len(changed_events) // 2
        changed_events[index]["cycle"] = changed_events[index - 1]["cycle"]
        apply_command_events(changed_cycle, changed_events)
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

        data_index = first_data_index(events)
        invalid_sequence = copy.deepcopy(document)
        invalid_sequence_events = copy.deepcopy(events)
        invalid_sequence_events[data_index]["sequence"] = (
            max(request["sequence"] for request in layer["memory_requests"]) + 1
        )
        apply_command_events(invalid_sequence, invalid_sequence_events)
        run_validator(
            validator,
            write_case(directory, "unknown-request-sequence", invalid_sequence),
            False,
        )

        out_of_range = copy.deepcopy(document)
        out_of_range_events = copy.deepcopy(events)
        selected = out_of_range_events[data_index]
        request = next(
            item for item in layer["memory_requests"]
            if item["sequence"] == selected["sequence"]
        )
        block_size = document["architecture"]["block_size"]
        selected["block_offset"] = (
            request["bytes"] + block_size - 1
        ) // block_size
        apply_command_events(out_of_range, out_of_range_events)
        run_validator(
            validator,
            write_case(directory, "out-of-range-block-offset", out_of_range),
            False,
        )

        wrong_direction = copy.deepcopy(document)
        wrong_direction_events = copy.deepcopy(events)
        wrong_direction_events[data_index]["command"] = (
            "WRITE" if wrong_direction_events[data_index]["command"] == "READ"
            else "READ"
        )
        apply_command_events(wrong_direction, wrong_direction_events)
        run_validator(
            validator,
            write_case(directory, "wrong-data-direction", wrong_direction),
            False,
        )

        wrong_mapping = copy.deepcopy(document)
        wrong_mapping_events = copy.deepcopy(events)
        channels = document["architecture"]["hbm_channels"]
        wrong_mapping_events[data_index]["channel"] = (
            wrong_mapping_events[data_index]["channel"] + 1
        ) % channels
        apply_command_events(wrong_mapping, wrong_mapping_events)
        run_validator(
            validator,
            write_case(directory, "wrong-address-mapping", wrong_mapping),
            False,
        )

        groups = collections.defaultdict(list)
        for event_index, event in enumerate(events):
            if event["command"] in {"READ", "WRITE"}:
                groups[(event["command"], event["channel"], event["bank"],
                        event["row"])].append(event_index)
        duplicate_group = next(indices for indices in groups.values()
                               if len(indices) >= 2)
        duplicate_missing = copy.deepcopy(document)
        duplicate_events = copy.deepcopy(events)
        source_index, target_index = duplicate_group[:2]
        duplicate_events[target_index]["sequence"] = \
            duplicate_events[source_index]["sequence"]
        duplicate_events[target_index]["block_offset"] = \
            duplicate_events[source_index]["block_offset"]
        apply_command_events(duplicate_missing, duplicate_events)
        run_validator(
            validator,
            write_case(directory, "duplicate-and-missing-data-block", duplicate_missing),
            False,
        )

        requests = {
            request["sequence"]: request for request in layer["memory_requests"]
        }
        temporal_pair = None
        for indices in groups.values():
            for position, first_index in enumerate(indices):
                first = events[first_index]
                for second_index in indices[position + 1:]:
                    second = events[second_index]
                    if requests[second["sequence"]]["producer_ready_cycle"] > \
                            first["cycle"]:
                        temporal_pair = (first_index, second_index)
                        break
                if temporal_pair is not None:
                    break
            if temporal_pair is not None:
                break
        if temporal_pair is None:
            raise RuntimeError(
                "smoke command trace lacks a same-row producer-ready swap pair"
            )
        temporal_swap = copy.deepcopy(document)
        temporal_events = copy.deepcopy(events)
        first_index, second_index = temporal_pair
        first_identity = (
            temporal_events[first_index]["sequence"],
            temporal_events[first_index]["block_offset"],
        )
        second_identity = (
            temporal_events[second_index]["sequence"],
            temporal_events[second_index]["block_offset"],
        )
        temporal_events[first_index]["sequence"], \
            temporal_events[first_index]["block_offset"] = second_identity
        temporal_events[second_index]["sequence"], \
            temporal_events[second_index]["block_offset"] = first_identity
        apply_command_events(temporal_swap, temporal_events)
        run_validator(
            validator,
            write_case(directory, "same-row-producer-ready-identity-swap", temporal_swap),
            False,
            "command precedes request producer-ready cycle",
        )
        second_request = requests[second_identity[0]]
        print(
            "temporal_identity_swap="
            f"cycle-{temporal_events[first_index]['cycle']}:seq-{second_identity[0]}:"
            f"block-{second_identity[1]}:producer-ready-"
            f"{second_request['producer_ready_cycle']}:REJECTED"
        )

        dependent_requests = [
            request for request in layer["memory_requests"]
            if request["producer_sequence"] is not None
        ]
        if not dependent_requests:
            raise RuntimeError("smoke trace lacks producer-dependent requests")
        dependency = dependent_requests[0]

        unknown_producer = copy.deepcopy(document)
        unknown_request = next(
            request for request in unknown_producer["layers"][0]["memory_requests"]
            if request["sequence"] == dependency["sequence"]
        )
        unknown_request["producer_sequence"] = max(requests) + 1
        run_validator(
            validator,
            write_case(directory, "unknown-producer-sequence", unknown_producer),
            False,
            "producer sequence is unknown",
        )

        self_dependency = copy.deepcopy(document)
        self_request = next(
            request for request in self_dependency["layers"][0]["memory_requests"]
            if request["sequence"] == dependency["sequence"]
        )
        self_request["producer_sequence"] = self_request["sequence"]
        run_validator(
            validator,
            write_case(directory, "self-producer-dependency", self_dependency),
            False,
            "request has a self producer dependency",
        )

        future_pair = None
        for consumer in dependent_requests:
            producer_class = (
                "edge" if consumer["request_class"] == "input"
                else "intermediate_write"
            )
            for producer in layer["memory_requests"]:
                dependency_ready = (
                    producer["completion_cycle"] + consumer["producer_delay_cycles"]
                )
                if producer["request_class"] == producer_class and \
                        producer["sequence"] != consumer["producer_sequence"] and \
                        dependency_ready > consumer["first_issue_cycle"]:
                    future_pair = (consumer, producer, dependency_ready)
                    break
            if future_pair is not None:
                break
        if future_pair is None:
            raise RuntimeError("smoke trace lacks a future producer mutation pair")
        future_consumer, future_producer, future_ready = future_pair
        future_dependency = copy.deepcopy(document)
        future_request = next(
            request for request in future_dependency["layers"][0]["memory_requests"]
            if request["sequence"] == future_consumer["sequence"]
        )
        future_request["producer_sequence"] = future_producer["sequence"]
        run_validator(
            validator,
            write_case(directory, "future-late-producer", future_dependency),
            False,
            "consumer request precedes reconstructed producer completion",
        )
        print(
            "future_producer_dependency="
            f"consumer-seq-{future_consumer['sequence']}:issue-"
            f"{future_consumer['first_issue_cycle']}:producer-seq-"
            f"{future_producer['sequence']}:dependency-ready-{future_ready}:REJECTED"
        )
    print("compiled_trace_validator_mutations=13/13_REJECTED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError,
            json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
