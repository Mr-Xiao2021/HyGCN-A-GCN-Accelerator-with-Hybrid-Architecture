#!/usr/bin/env python3
import argparse
import configparser
import io
import json
import subprocess
import sys
from pathlib import Path


PROFILE_PATH = "configs/HYGCN_PAPER.ini"
TARGET_FILES = (
    "configs/paper_metrics.json",
    "configs/paper_workloads.json",
)
TARGET_PARAMETER_PATHS = {
    "configs/paper_metrics.json": (
        "tolerance", "datasets", "digitization", "metrics",
    ),
    "configs/paper_workloads.json": (
        "figures", "graph_partition", "memory_ablation",
    ),
}
CALIBRATION_KEYS = (
    "model.aggregation_shard_capacity_bytes",
    "model.batch_launch_interval_cycles",
    "model.neighbor_index_ready_cycles",
    "model.coordinator_fifo_active_windows",
    "model.row_first_bank_interleave",
)
SOURCE_POLICY_FILES = (
    "hygcn/paper_sim.cpp",
    "hygcn/paper_sim.h",
)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Derive a review-to-review HyGCN configuration audit"
    )
    parser.add_argument("--base-ref", required=True)
    parser.add_argument("--implementation-ref", default="WORKTREE")
    parser.add_argument("--before-report")
    parser.add_argument("--after-report")
    parser.add_argument("--output", required=True)
    return parser.parse_args()


def git_text(root, revision, path):
    if revision == "WORKTREE":
        return (root / path).read_text(encoding="utf-8")
    return subprocess.check_output(
        ["git", "show", f"{revision}:{path}"], cwd=root, text=True
    )


def resolve_revision(root, revision):
    if revision == "WORKTREE":
        return "WORKTREE"
    return subprocess.check_output(
        ["git", "rev-parse", revision], cwd=root, text=True
    ).strip()


def profile_values(text):
    parser = configparser.ConfigParser()
    parser.read_file(io.StringIO(text))
    return {
        f"{section}.{key}": value
        for section in parser.sections()
        for key, value in parser.items(section)
    }


def file_digest(text):
    import hashlib

    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def source_diff(root, base_ref, implementation_ref, path):
    command = ["git", "diff", "--unified=0", base_ref]
    if implementation_ref != "WORKTREE":
        command.append(implementation_ref)
    command.extend(["--", path])
    return subprocess.check_output(command, cwd=root, text=True)


def json_path(document, path):
    value = document
    for key in path.split("."):
        if not isinstance(value, dict) or key not in value:
            return {"__missing__": True}
        value = value[key]
    return value


def target_parameter_digest(text, paths):
    document = json.loads(text)
    selected = {path: json_path(document, path) for path in paths}
    return file_digest(json.dumps(selected, sort_keys=True, separators=(",", ":")))


def json_leaf_differences(before_text, after_text):
    before = json.loads(before_text)
    after = json.loads(after_text)
    differences = []

    def walk(path, lhs, rhs):
        if isinstance(lhs, dict) and isinstance(rhs, dict):
            for key in sorted(set(lhs) | set(rhs)):
                walk(path + [key], lhs.get(key), rhs.get(key))
            return
        if lhs != rhs:
            differences.append({
                "path": ".".join(path),
                "before": lhs,
                "after": rhs,
            })

    walk([], before, after)
    return differences


def metric_snapshot(report):
    snapshot = {
        f"aggregate.{name}": value
        for name, value in report.get("aggregate", {}).items()
        if isinstance(value, (int, float))
    }
    for dataset, evidence in report.get("per_dataset", {}).items():
        for name, value in evidence.get("metrics", {}).items():
            if isinstance(value, (int, float)):
                snapshot[f"per_dataset.{dataset}.{name}"] = value
    return snapshot


def metric_differences(before_report, after_report):
    before = metric_snapshot(before_report)
    after = metric_snapshot(after_report)
    differences = []
    for key in sorted(set(before) | set(after)):
        lhs = before.get(key)
        rhs = after.get(key)
        if lhs == rhs:
            continue
        delta = None if lhs is None or rhs is None else rhs - lhs
        relative_delta = None
        if delta is not None and lhs != 0:
            relative_delta = delta / abs(lhs)
        differences.append({
            "metric": key,
            "before": lhs,
            "after": rhs,
            "delta": delta,
            "relative_delta": relative_delta,
        })
    return differences


def build_audit(root, base_ref, implementation_ref,
                before_report=None, after_report=None):
    base_profile = profile_values(git_text(root, base_ref, PROFILE_PATH))
    implementation_profile = profile_values(
        git_text(root, implementation_ref, PROFILE_PATH)
    )
    profile_differences = [
        {
            "parameter": key,
            "before": base_profile.get(key),
            "after": implementation_profile.get(key),
        }
        for key in sorted(set(base_profile) | set(implementation_profile))
        if base_profile.get(key) != implementation_profile.get(key)
    ]
    target_files = []
    for path in TARGET_FILES:
        before = git_text(root, base_ref, path)
        after = git_text(root, implementation_ref, path)
        target_before = target_parameter_digest(
            before, TARGET_PARAMETER_PATHS[path])
        target_after = target_parameter_digest(
            after, TARGET_PARAMETER_PATHS[path])
        target_files.append({
            "path": path,
            "before_sha256": file_digest(before),
            "after_sha256": file_digest(after),
            "changed": before != after,
            "changed_paths": json_leaf_differences(before, after),
            "target_parameters_before_sha256": target_before,
            "target_parameters_after_sha256": target_after,
            "target_parameters_changed": target_before != target_after,
        })
    calibration_differences = [
        difference for difference in profile_differences
        if difference["parameter"] in CALIBRATION_KEYS
    ]
    source_policy_files = []
    for path in SOURCE_POLICY_FILES:
        before = git_text(root, base_ref, path)
        after = git_text(root, implementation_ref, path)
        diff = source_diff(root, base_ref, implementation_ref, path)
        source_policy_files.append({
            "path": path,
            "before_sha256": file_digest(before),
            "after_sha256": file_digest(after),
            "changed": before != after,
            "changed_diff_lines": sum(
                1 for line in diff.splitlines()
                if line.startswith(("+", "-")) and
                not line.startswith(("+++", "---"))
            ),
            "unified_diff": diff,
        })
    no_target_retuning = (
        not calibration_differences
        and not any(item["target_parameters_changed"] for item in target_files)
        and not any(item["changed"] for item in source_policy_files)
    )
    return {
        "schema_version": 1,
        "base_ref": resolve_revision(root, base_ref),
        "implementation_ref": resolve_revision(root, implementation_ref),
        "profile_path": PROFILE_PATH,
        "profile_differences": profile_differences,
        "target_files": target_files,
        "calibration_keys": list(CALIBRATION_KEYS),
        "calibration_differences": calibration_differences,
        "source_policy_files": source_policy_files,
        "metric_delta": (
            metric_differences(before_report, after_report)
            if before_report is not None and after_report is not None else None
        ),
        "no_target_parameter_retuning": no_target_retuning,
        "no_target_parameter_retuning_derivation": (
            "Computed from the complete versioned figures, graph_partition, and "
            "memory_ablation policy trees plus declared profile calibration keys. "
            "Source policy file hashes are part of the boolean. Any scheduler "
            "policy or source implementation change, including FIFO window "
            "count/size/release, makes this boolean false instead of being hidden "
            "by an allowlist."
        ),
    }


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    output = Path(args.output)
    if not output.is_absolute():
        output = root / output
    if bool(args.before_report) != bool(args.after_report):
        raise ValueError("--before-report and --after-report must be supplied together")
    before_report = None
    after_report = None
    if args.before_report:
        before_path = Path(args.before_report)
        after_path = Path(args.after_report)
        if not before_path.is_absolute():
            before_path = root / before_path
        if not after_path.is_absolute():
            after_path = root / after_path
        with before_path.open(encoding="utf-8") as stream:
            before_report = json.load(stream)
        with after_path.open(encoding="utf-8") as stream:
            after_report = json.load(stream)
    audit = build_audit(
        root, args.base_ref, args.implementation_ref,
        before_report, after_report,
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8") as stream:
        json.dump(audit, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(output)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError,
            configparser.Error, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
