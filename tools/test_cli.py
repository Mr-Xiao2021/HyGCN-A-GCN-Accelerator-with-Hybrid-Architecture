#!/usr/bin/env python3
import argparse
import subprocess
import sys
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="Check HyGCN CLI failures")
    parser.add_argument("--binary", default="build/hygcntest")
    return parser.parse_args()


def expect_failure(command, expected_text, root):
    completed = subprocess.run(
        command,
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if completed.returncode == 0 or expected_text not in completed.stderr:
        raise RuntimeError(
            f"expected non-zero status and {expected_text!r}: "
            f"rc={completed.returncode} stderr={completed.stderr!r}"
        )


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    binary = Path(args.binary)
    if not binary.is_absolute():
        binary = root / binary
    common = [str(binary), "--engine", "paper", "--profile", "smoke", "--model", "gcn"]
    expect_failure(common + ["--dataset", "missing", "--quiet"],
                   "missing graph metadata", root)
    expect_failure(common + ["--dataset", "test", "--layer", "invalid", "--quiet"],
                   "invalid layer: expected all, 0, or 1", root)
    expect_failure(common + ["--dataset", "test", "--profile-path", "missing.ini", "--quiet"],
                   "cannot parse architecture config", root)
    expect_failure(common + ["--dataset", "test", "--scope", "invalid", "--quiet"],
                   "invalid scope: expected full or aggregation", root)
    print("cli_failure_paths=PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
