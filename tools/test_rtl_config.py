#!/usr/bin/env python3
import configparser
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def parameter(path: Path, name: str) -> int:
    text = path.read_text(encoding="utf-8")
    match = re.search(rf"parameter\s+int\s+{re.escape(name)}\s*=\s*([^,\n]+)", text)
    if not match:
        raise ValueError(f"missing parameter {name} in {path}")
    expression = match.group(1).strip()
    if not re.fullmatch(r"[0-9 *+()]+", expression):
        raise ValueError(f"non-constant parameter {name}: {expression}")
    return int(eval(expression, {"__builtins__": {}}, {}))


def main() -> None:
    config = configparser.ConfigParser()
    config.read(ROOT / "configs" / "HYGCN_PAPER.ini")

    ae = ROOT / "rtl" / "hygcn_aggregation_cluster.sv"
    ce = ROOT / "rtl" / "hygcn_combination_cluster.sv"
    buffer_ctrl = ROOT / "rtl" / "hygcn_aggregation_buffer_ctrl.sv"
    arbiter = ROOT / "rtl" / "hygcn_request_arbiter.sv"

    expected = {
        "AE cores": (parameter(ae, "CORES"), config.getint("architecture", "num_simd")),
        "AE SIMD lanes": (parameter(ae, "LANES"), config.getint("architecture", "simd_width")),
        "CE modules": (
            parameter(ce, "MODULES"),
            config.getint("architecture", "combination_modules"),
        ),
        "CE arrays/module": (
            parameter(ce, "ARRAYS_PER_MODULE"),
            config.getint("architecture", "arrays_per_module"),
        ),
        "CE array width": (
            parameter(ce, "LANES"),
            config.getint("architecture", "array_width"),
        ),
        "request classes": (parameter(arbiter, "REQUEST_CLASSES"), 4),
    }
    for label, (actual, wanted) in expected.items():
        if actual != wanted:
            raise ValueError(f"{label}: RTL={actual}, paper config={wanted}")

    rtl_buffer_bytes = parameter(buffer_ctrl, "BANKS") * parameter(buffer_ctrl, "BANK_BYTES")
    paper_buffer_bytes = config.getint("buffer", "aggregation_bytes")
    if rtl_buffer_bytes != paper_buffer_bytes:
        raise ValueError(
            f"aggregation buffer: RTL={rtl_buffer_bytes}, paper config={paper_buffer_bytes}"
        )

    print("rtl_parameter_alignment=PASS")


if __name__ == "__main__":
    main()

