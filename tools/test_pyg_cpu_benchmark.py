#!/usr/bin/env python3
import sys
import tempfile
from pathlib import Path

from pyg_cpu_benchmark import (
    deterministic_sample,
    percentile,
    read_edges,
    read_metadata,
)


def main():
    sources = [0, 1, 2, 3, 4, 5]
    destinations = [0, 0, 0, 1, 1, 1]
    sampled_sources, sampled_destinations = deterministic_sample(
        sources, destinations, vertices=3, sample_size=5
    )
    if sampled_sources != [0, 0, 1, 1, 2, 3, 3, 4, 4, 5]:
        raise ValueError("deterministic sampling does not match the C++ fallback")
    if sampled_destinations != [0] * 5 + [1] * 5:
        raise ValueError("deterministic sampling produced incorrect destinations")
    if percentile([10.0, 20.0, 30.0], 0.5) != 20.0:
        raise ValueError("percentile interpolation is incorrect")

    with tempfile.TemporaryDirectory(prefix="hygcn-pyg-helper-") as temp:
        root = Path(temp)
        metadata_path = root / "test.txt"
        edges_path = root / "test_edge.csv"
        metadata_path.write_text(
            "edge,2\nfeature,16\nvertex,3\nclass,2\n", encoding="utf-8"
        )
        edges_path.write_text("0,1\n2,1\n", encoding="utf-8")
        metadata = read_metadata(metadata_path)
        loaded_sources, loaded_destinations = read_edges(edges_path, 3)
        if metadata["feature"] != 16 or loaded_sources != [0, 2] or \
                loaded_destinations != [1, 1]:
            raise ValueError("graph input parsing is incorrect")
    print("pyg_cpu_helpers=PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
