#!/usr/bin/env python3
import argparse
import csv
import hashlib
import html
import json
import math
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


COLORS = {
    "cpu": "#B7D9DF",
    "hygcn": "#315E8A",
    "axis": "#1F2933",
    "grid": "#D9E0E6",
    "muted": "#52606D",
    "background": "#FFFFFF",
}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Reproduce the final speed and energy figures in ResearchReport.pdf"
    )
    parser.add_argument(
        "--targets", default="configs/report_figure_targets.json",
        help="versioned PDF digitization manifest",
    )
    parser.add_argument(
        "--binary", help="hygcntest binary; enables supported legacy simulator runs"
    )
    parser.add_argument("--output-dir", default="res/report")
    parser.add_argument("--skip-raster", action="store_true")
    return parser.parse_args()


def resolve(root, value):
    path = Path(value)
    return path if path.is_absolute() else root / path


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def linear_fit(xs, ys):
    if len(xs) != len(ys) or len(xs) < 2:
        raise ValueError("axis fit requires matching tick arrays")
    mean_x = sum(xs) / len(xs)
    mean_y = sum(ys) / len(ys)
    denominator = sum((value - mean_x) ** 2 for value in xs)
    if denominator == 0:
        raise ValueError("axis tick coordinates are degenerate")
    slope = sum(
        (x_value - mean_x) * (y_value - mean_y)
        for x_value, y_value in zip(xs, ys)
    ) / denominator
    return slope, mean_y - slope * mean_x


def digitize_axis(axis, workload_count):
    tick_y = axis["tick_y"]
    tick_log10 = axis["tick_log10"]
    bar_top_y = axis["bar_top_y"]
    if len(bar_top_y) != workload_count:
        raise ValueError("bar count does not match workload count")
    slope, intercept = linear_fit(tick_y, tick_log10)
    raw_values = [10 ** (slope * y_value + intercept) for y_value in bar_top_y]
    raw_mean = sum(raw_values) / len(raw_values)
    reported_mean = float(axis["reported_arithmetic_mean"])
    scale = reported_mean / raw_mean
    return {
        "slope": slope,
        "intercept": intercept,
        "raw_values": raw_values,
        "raw_mean": raw_mean,
        "reported_mean": reported_mean,
        "scale": scale,
        "values": [value * scale for value in raw_values],
    }


def build_points(manifest):
    workloads = manifest["workloads"]
    speed = digitize_axis(manifest["axes"]["speedup"], len(workloads))
    energy = digitize_axis(
        manifest["axes"]["energy_reduction"], len(workloads)
    )
    points = []
    for index, workload in enumerate(workloads):
        point = dict(workload)
        point.update({
            "speedup_raw": speed["raw_values"][index],
            "speedup": speed["values"][index],
            "energy_reduction_raw": energy["raw_values"][index],
            "energy_reduction": energy["values"][index],
            "measurement_status": "not-run",
        })
        points.append(point)
    return points, {"speedup": speed, "energy_reduction": energy}


def run_legacy(binary, root, output_dir, manifest, points):
    raw_dir = output_dir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    supported = set(manifest["simulator"]["supported_datasets"])
    frequency_ghz = float(manifest["simulator"]["frequency_ghz"])
    log_lines = []
    for point in points:
        if point["dataset"] not in supported:
            point["measurement_status"] = "unavailable: dataset files absent"
            continue
        command = [
            str(binary), "--engine", "legacy", "--profile", "legacy",
            "--model", point["model"], "--dataset", point["dataset"],
            "--seed", "1", "--output-dir", str(raw_dir), "--quiet",
        ]
        completed = subprocess.run(
            command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, check=False,
        )
        log_lines.extend([
            f"$ {' '.join(command)}",
            completed.stdout.rstrip(),
            completed.stderr.rstrip(),
            f"returncode={completed.returncode}",
            "",
        ])
        if completed.returncode != 0:
            raise RuntimeError(
                f"legacy run failed for {point['label']}: {completed.stderr.strip()}"
            )
        result_path = raw_dir / (
            f"legacy_{point['model']}_{point['dataset']}_seed-1.json"
        )
        with result_path.open(encoding="utf-8") as stream:
            result = json.load(stream)
        summary = result["summary"]
        cycles = int(summary["total_cycles"])
        dram_energy_pj = float(summary["total_dram_energy_pj"])
        point.update({
            "measurement_status": "measured: legacy simulator",
            "legacy_result": str(result_path.relative_to(output_dir)),
            "hygcn_cycles": cycles,
            "hygcn_latency_ms": cycles / (frequency_ghz * 1_000_000.0),
            "hygcn_dram_energy_pj": dram_energy_pj,
            "calibrated_pyg_cpu_latency_ms": (
                cycles / (frequency_ghz * 1_000_000.0) * point["speedup"]
            ),
            "calibrated_pyg_cpu_dram_equivalent_pj": (
                dram_energy_pj * point["energy_reduction"]
            ),
            "graphsage_sample_source": result["manifest"].get(
                "graphsage_sample_source", "not-applicable"
            ),
        })
    (output_dir / "legacy_runs.log").write_text(
        "\n".join(log_lines).rstrip() + "\n", encoding="utf-8"
    )


def svg_chart(points, metric, y_offset=0, height=720, chart_text=None):
    if metric == "speedup":
        title = "Processing Speedup"
        subtitle = "PyG-CPU normalized to 1; HyGCN uses digitized Figure 7 values"
        max_exponent = 4
        left_values = [1.0] * len(points)
        right_values = [point["speedup"] for point in points]
        mean_value = sum(right_values) / len(right_values)
        mean_text = f"Arithmetic mean: {mean_value:.0f}x"
    else:
        title = "Normalized Energy"
        subtitle = "HyGCN normalized to 1; PyG-CPU uses digitized Figure 8 values"
        max_exponent = 5
        left_values = [point["energy_reduction"] for point in points]
        right_values = [1.0] * len(points)
        mean_value = sum(left_values) / len(left_values)
        mean_text = f"Arithmetic mean reduction: {mean_value:.0f}x"

    text_override = (chart_text or {}).get(metric, {})
    title = text_override.get("title", title)
    subtitle = text_override.get("subtitle", subtitle)
    mean_text = text_override.get("mean_text", mean_text)
    max_exponent = int(text_override.get("max_exponent", max_exponent))

    width = 1600
    left = 126
    right = width - 60
    top = y_offset + 92
    bottom = y_offset + height - 155
    plot_height = bottom - top
    group_width = (right - left) / len(points)
    bar_width = min(28, group_width * 0.31)

    def y_position(value):
        exponent = min(max_exponent, max(0.0, math.log10(max(value, 1.0))))
        return bottom - exponent / max_exponent * plot_height

    output = [
        f'<rect x="0" y="{y_offset}" width="{width}" height="{height}" fill="{COLORS["background"]}"/>',
        f'<text x="{left}" y="{y_offset + 38}" font-size="27" font-family="DejaVu Sans, sans-serif" font-weight="700" fill="{COLORS["axis"]}">{title}</text>',
        f'<text x="{left}" y="{y_offset + 66}" font-size="15" font-family="DejaVu Sans, sans-serif" fill="{COLORS["muted"]}">{subtitle}</text>',
        f'<text x="{right}" y="{y_offset + 42}" text-anchor="end" font-size="16" font-family="DejaVu Sans, sans-serif" font-weight="700" fill="{COLORS["axis"]}">{mean_text}</text>',
    ]
    for exponent in range(max_exponent + 1):
        value = 10 ** exponent
        y_value = y_position(value)
        output.extend([
            f'<line x1="{left}" y1="{y_value:.2f}" x2="{right}" y2="{y_value:.2f}" stroke="{COLORS["grid"]}" stroke-width="1"/>',
            f'<text x="{left - 14}" y="{y_value + 5:.2f}" text-anchor="end" font-size="14" font-family="DejaVu Sans, sans-serif" fill="{COLORS["muted"]}">10^{exponent}</text>',
        ])
    output.extend([
        f'<line x1="{left}" y1="{top}" x2="{left}" y2="{bottom}" stroke="{COLORS["axis"]}" stroke-width="1.5"/>',
        f'<line x1="{left}" y1="{bottom}" x2="{right}" y2="{bottom}" stroke="{COLORS["axis"]}" stroke-width="1.5"/>',
    ])
    for index, point in enumerate(points):
        center = left + (index + 0.5) * group_width
        for value, color, x_value in (
            (left_values[index], COLORS["cpu"], center - bar_width - 2),
            (right_values[index], COLORS["hygcn"], center + 2),
        ):
            y_value = y_position(value)
            visible_top = min(y_value, bottom - 4)
            output.append(
                f'<rect x="{x_value:.2f}" y="{visible_top:.2f}" width="{bar_width:.2f}" height="{bottom - visible_top:.2f}" fill="{color}" stroke="#FFFFFF" stroke-width="0.6"/>'
            )
        label = html.escape(point["label"])
        output.append(
            f'<text x="{center + 5:.2f}" y="{bottom + 18}" transform="rotate(90 {center + 5:.2f} {bottom + 18})" font-size="13" font-family="DejaVu Sans, sans-serif" fill="{COLORS["axis"]}">{label}</text>'
        )
    legend_y = y_offset + height - 34
    output.extend([
        f'<rect x="{left}" y="{legend_y - 14}" width="19" height="14" fill="{COLORS["cpu"]}"/>',
        f'<text x="{left + 27}" y="{legend_y - 2}" font-size="15" font-family="DejaVu Sans, sans-serif" fill="{COLORS["axis"]}">PyG-CPU</text>',
        f'<rect x="{left + 122}" y="{legend_y - 14}" width="19" height="14" fill="{COLORS["hygcn"]}"/>',
        f'<text x="{left + 149}" y="{legend_y - 2}" font-size="15" font-family="DejaVu Sans, sans-serif" fill="{COLORS["axis"]}">HyGCN</text>',
    ])
    return output


def write_svg(path, points, metrics, chart_text=None):
    chart_height = 720
    total_height = chart_height * len(metrics)
    body = []
    for index, metric in enumerate(metrics):
        body.extend(svg_chart(
            points, metric, index * chart_height, chart_height, chart_text
        ))
    document = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<svg xmlns="http://www.w3.org/2000/svg" width="1600" height="{total_height}" viewBox="0 0 1600 {total_height}">',
        *body,
        "</svg>",
    ]
    path.write_text("\n".join(document) + "\n", encoding="utf-8")


def load_font(size, bold=False):
    from PIL import ImageFont
    candidates = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf" if bold else
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    ]
    for candidate in candidates:
        if Path(candidate).is_file():
            return ImageFont.truetype(candidate, size)
    return ImageFont.load_default()


def draw_rotated_label(image, position, text, font, fill):
    from PIL import Image, ImageDraw
    scratch = Image.new("RGBA", (180, 28), (255, 255, 255, 0))
    scratch_draw = ImageDraw.Draw(scratch)
    scratch_draw.text((0, 4), text, font=font, fill=fill)
    rotated = scratch.rotate(-90, expand=True)
    image.alpha_composite(rotated, position)


def draw_raster_chart(image, points, metric, y_offset, height, chart_text=None):
    from PIL import ImageDraw
    draw = ImageDraw.Draw(image)
    axis_color = COLORS["axis"]
    muted = COLORS["muted"]
    grid = COLORS["grid"]
    title_font = load_font(28, bold=True)
    body_font = load_font(15)
    body_bold = load_font(16, bold=True)
    label_font = load_font(13)

    if metric == "speedup":
        title = "Processing Speedup"
        subtitle = "PyG-CPU normalized to 1; HyGCN uses digitized Figure 7 values"
        max_exponent = 4
        left_values = [1.0] * len(points)
        right_values = [point["speedup"] for point in points]
        mean_text = f"Arithmetic mean: {sum(right_values) / len(right_values):.0f}x"
    else:
        title = "Normalized Energy"
        subtitle = "HyGCN normalized to 1; PyG-CPU uses digitized Figure 8 values"
        max_exponent = 5
        left_values = [point["energy_reduction"] for point in points]
        right_values = [1.0] * len(points)
        mean_text = (
            f"Arithmetic mean reduction: {sum(left_values) / len(left_values):.0f}x"
        )

    text_override = (chart_text or {}).get(metric, {})
    title = text_override.get("title", title)
    subtitle = text_override.get("subtitle", subtitle)
    mean_text = text_override.get("mean_text", mean_text)
    max_exponent = int(text_override.get("max_exponent", max_exponent))

    left = 126
    right = 1540
    top = y_offset + 92
    bottom = y_offset + height - 155
    plot_height = bottom - top
    group_width = (right - left) / len(points)
    bar_width = min(28, int(group_width * 0.31))

    def y_position(value):
        exponent = min(max_exponent, max(0.0, math.log10(max(value, 1.0))))
        return int(round(bottom - exponent / max_exponent * plot_height))

    draw.text((left, y_offset + 22), title, font=title_font, fill=axis_color)
    draw.text((left, y_offset + 60), subtitle, font=body_font, fill=muted)
    mean_box = draw.textbbox((0, 0), mean_text, font=body_bold)
    draw.text((right - (mean_box[2] - mean_box[0]), y_offset + 27), mean_text,
              font=body_bold, fill=axis_color)
    for exponent in range(max_exponent + 1):
        y_value = y_position(10 ** exponent)
        draw.line((left, y_value, right, y_value), fill=grid, width=1)
        tick = f"10^{exponent}"
        tick_box = draw.textbbox((0, 0), tick, font=body_font)
        draw.text((left - 14 - (tick_box[2] - tick_box[0]), y_value - 8), tick,
                  font=body_font, fill=muted)
    draw.line((left, top, left, bottom), fill=axis_color, width=2)
    draw.line((left, bottom, right, bottom), fill=axis_color, width=2)
    for index, point in enumerate(points):
        center = int(round(left + (index + 0.5) * group_width))
        for value, color, x_value in (
            (left_values[index], COLORS["cpu"], center - bar_width - 2),
            (right_values[index], COLORS["hygcn"], center + 2),
        ):
            y_value = min(y_position(value), bottom - 4)
            draw.rectangle((x_value, y_value, x_value + bar_width, bottom), fill=color)
        draw_rotated_label(
            image, (center - 9, bottom + 13), point["label"], label_font, axis_color
        )
    legend_y = y_offset + height - 47
    draw.rectangle((left, legend_y, left + 19, legend_y + 14), fill=COLORS["cpu"])
    draw.text((left + 28, legend_y - 3), "PyG-CPU", font=body_font, fill=axis_color)
    draw.rectangle((left + 122, legend_y, left + 141, legend_y + 14),
                   fill=COLORS["hygcn"])
    draw.text((left + 150, legend_y - 3), "HyGCN", font=body_font, fill=axis_color)


def write_raster(output_dir, points, prefix="report", metrics=None, chart_text=None):
    try:
        from PIL import Image
    except ImportError:
        return []
    metrics = metrics or ["speedup", "energy_reduction"]
    artifacts = []
    metric_names = {"speedup": "speedup", "energy_reduction": "energy"}
    for metric in metrics:
        image = Image.new("RGBA", (1600, 720), COLORS["background"])
        draw_raster_chart(image, points, metric, 0, 720, chart_text)
        path = output_dir / f"{prefix}_{metric_names[metric]}.png"
        image.convert("RGB").save(path, "PNG", optimize=True)
        artifacts.append(path)
    combined = Image.new("RGBA", (1600, 720 * len(metrics)), COLORS["background"])
    for index, metric in enumerate(metrics):
        draw_raster_chart(
            combined, points, metric, index * 720, 720, chart_text
        )
    suffix = "comparison" if len(metrics) > 1 else metric_names[metrics[0]]
    png_path = output_dir / f"{prefix}_{suffix}.png"
    pdf_path = output_dir / f"{prefix}_{suffix}.pdf"
    combined_rgb = combined.convert("RGB")
    if png_path not in artifacts:
        combined_rgb.save(png_path, "PNG", optimize=True)
        artifacts.append(png_path)
    combined_rgb.save(pdf_path, "PDF", resolution=144.0)
    artifacts.append(pdf_path)
    return artifacts


def write_csv(path, points):
    fields = [
        "label", "model", "dataset", "speedup", "speedup_raw",
        "energy_reduction", "energy_reduction_raw", "measurement_status",
        "hygcn_cycles", "hygcn_latency_ms", "hygcn_dram_energy_pj",
        "calibrated_pyg_cpu_latency_ms",
        "calibrated_pyg_cpu_dram_equivalent_pj", "graphsage_sample_source",
        "legacy_result",
    ]
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for point in points:
            writer.writerow(point)


def write_markdown(path, summary, points):
    lines = [
        "# ResearchReport.pdf Final Figure Reproduction",
        "",
        "## Result",
        "",
        f"- Figure 7 speedup arithmetic mean: **{summary['means']['speedup']:.0f}x**.",
        f"- Figure 8 energy-reduction arithmetic mean: **{summary['means']['energy_reduction']:.0f}x**.",
        f"- Legacy simulator coverage: **{summary['measurement_coverage']['measured']}/15** workloads.",
        "- The plotted normalized ratios are digitized from the PDF and uniformly anchored to the averages stated in its text.",
        "",
        "## Workloads",
        "",
        "| Workload | Speedup | Energy reduction | Local evidence |",
        "|---|---:|---:|---|",
    ]
    for point in points:
        lines.append(
            f"| {point['label']} | {point['speedup']:.2f}x | "
            f"{point['energy_reduction']:.2f}x | {point['measurement_status']} |"
        )
    lines.extend([
        "",
        "## Evidence Boundary",
        "",
        "The local legacy simulator supplies HyGCN cycle counts and DRAMSim3 DRAM energy for Cora, Citeseer, DBLP, and PubMed across GCN, GIN, and GraphSAGE. GraphSAGE uses the checked deterministic 25-neighbor fallback when no sample file exists. Reddit is not simulated because its graph files are absent.",
        "",
        "The repository does not include a local PyG-CPU benchmark capture, RTL synthesis results, or CACTI SRAM/compute energy. CPU latency and energy equivalents in the CSV are therefore report-calibrated reconstructions, and the local energy value covers DRAM only. These figures reproduce the report presentation; they are not an independent validation of the reported 275x/4112x claims.",
        "",
        "## Artifacts",
        "",
        "- `report_comparison.pdf`, `report_comparison.png`, and `report_comparison.svg`",
        "- `report_speedup.png`/`.svg` and `report_energy.png`/`.svg`",
        "- `report_metrics.csv` and `report_summary.json`",
        "- `raw/` legacy JSON/CSV plus `legacy_runs.log` when a simulator binary is supplied",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def git_value(root, *arguments):
    completed = subprocess.run(
        ["git", *arguments], cwd=root, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True, check=False,
    )
    return completed.stdout.strip() if completed.returncode == 0 else "unknown"


def main():
    args = parse_args()
    root = Path(__file__).resolve().parents[1]
    target_path = resolve(root, args.targets)
    output_dir = resolve(root, args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    with target_path.open(encoding="utf-8") as stream:
        manifest = json.load(stream)
    if manifest.get("schema_version") != 1:
        raise ValueError("unsupported report target schema")
    points, axes = build_points(manifest)

    binary = resolve(root, args.binary) if args.binary else None
    if binary:
        if not binary.is_file():
            raise FileNotFoundError(f"missing simulator binary: {binary}")
        run_legacy(binary.resolve(), root, output_dir, manifest, points)

    write_csv(output_dir / "report_metrics.csv", points)
    write_svg(output_dir / "report_speedup.svg", points, ["speedup"])
    write_svg(output_dir / "report_energy.svg", points, ["energy_reduction"])
    write_svg(
        output_dir / "report_comparison.svg", points,
        ["speedup", "energy_reduction"],
    )
    raster_artifacts = [] if args.skip_raster else write_raster(output_dir, points)

    measured = sum(
        point["measurement_status"].startswith("measured") for point in points
    )
    source_pdf = root / manifest["source"]["file"]
    summary = {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "git_branch": git_value(root, "branch", "--show-current"),
        "git_commit": git_value(root, "rev-parse", "HEAD"),
        "source": {
            **manifest["source"],
            "sha256": sha256(source_pdf),
        },
        "means": {
            "speedup": sum(point["speedup"] for point in points) / len(points),
            "energy_reduction": (
                sum(point["energy_reduction"] for point in points) / len(points)
            ),
            "speedup_raw_vector_digitization": axes["speedup"]["raw_mean"],
            "energy_raw_vector_digitization": axes["energy_reduction"]["raw_mean"],
        },
        "axis_fits": {
            name: {
                "log10_slope": axis["slope"],
                "log10_intercept": axis["intercept"],
                "text_anchor_scale": axis["scale"],
                "uniform_adjustment_percent": (axis["scale"] - 1.0) * 100.0,
            }
            for name, axis in axes.items()
        },
        "measurement_coverage": {
            "measured": measured,
            "total": len(points),
            "missing": len(points) - measured,
        },
        "scope": manifest["simulator"],
        "raster_artifacts": [path.name for path in raster_artifacts],
        "points": points,
    }
    with (output_dir / "report_summary.json").open("w", encoding="utf-8") as stream:
        json.dump(summary, stream, indent=2, sort_keys=True)
        stream.write("\n")
    write_markdown(output_dir / "REPRODUCTION.md", summary, points)
    print(f"output_dir={output_dir}")
    print(f"speedup_mean={summary['means']['speedup']:.6f}")
    print(f"energy_reduction_mean={summary['means']['energy_reduction']:.6f}")
    print(f"legacy_coverage={measured}/{len(points)}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, json.JSONDecodeError,
            RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
