#!/usr/bin/env python3
"""Compare recorded RGB global timestamps across receiver frames.csv files."""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Frame:
    timestamp_us: int
    frame_id: str
    row_number: int


def die(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(2)


def frames_csv_path(value: str) -> Path:
    path = Path(value)
    if path.is_dir():
        path = path / "frames.csv"
    if not path.exists():
        die(f"frames.csv not found: {path}")
    return path


def default_label(path: Path) -> str:
    if path.name == "frames.csv":
        parts = [p for p in path.parts[-4:-1] if p]
        return "/".join(parts) if parts else str(path.parent)
    return str(path)


def parse_int(value: str | None) -> int | None:
    if value is None:
        return None
    value = value.strip()
    if not value:
        return None
    try:
        parsed = int(value)
    except ValueError:
        return None
    return parsed if parsed > 0 else None


def read_rgb_frames(path: Path, timestamp_column: str) -> list[Frame]:
    frames: list[Frame] = []
    seen: set[tuple[str, int]] = set()
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames:
            die(f"empty csv: {path}")
        if "stream_type" not in reader.fieldnames:
            die(f"missing stream_type column: {path}")
        if timestamp_column != "auto" and timestamp_column not in reader.fieldnames:
            die(f"missing {timestamp_column} column: {path}")
        for row_number, row in enumerate(reader, start=2):
            if (row.get("stream_type") or "").strip().lower() != "rgb":
                continue
            if any(field in row and (row.get(field) or "").strip() == "0"
                   for field in ("rgb_recorded", "recording_window_valid", "segment_window_valid")):
                continue
            timestamp_us = parse_int(row.get(timestamp_column)) if timestamp_column != "auto" else (
                parse_int(row.get("global_timestamp_us"))
                or parse_int(row.get("frame_system_timestamp_us"))
                or parse_int(row.get("packet_system_timestamp_us"))
                or parse_int(row.get("rgb_system_timestamp_us"))
                or parse_int(row.get("timestamp_us"))
            )
            if timestamp_us is None:
                continue
            frame_id = (row.get("frame_id") or row.get("rgb_frame_id") or "").strip()
            key = (frame_id, timestamp_us)
            if key in seen:
                continue
            seen.add(key)
            frames.append(Frame(timestamp_us=timestamp_us, frame_id=frame_id, row_number=row_number))
    frames.sort(key=lambda item: item.timestamp_us)
    if not frames:
        die(f"no rgb frames with timestamp found: {path}")
    return frames


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    ordered = sorted(values)
    index = (len(ordered) - 1) * pct
    low = int(index)
    high = min(low + 1, len(ordered) - 1)
    weight = index - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def nearest_delta_us(timestamp_us: int, candidates: list[Frame], candidate_timestamps: list[int]) -> tuple[int, Frame]:
    index = bisect.bisect_left(candidate_timestamps, timestamp_us)
    best: tuple[int, Frame] | None = None
    for candidate_index in (index - 1, index):
        if 0 <= candidate_index < len(candidates):
            frame = candidates[candidate_index]
            delta = abs(frame.timestamp_us - timestamp_us)
            if best is None or delta < best[0]:
                best = (delta, frame)
    if best is None:
        raise RuntimeError("no candidate frame")
    return best


def summarize(reference: list[Frame], other: list[Frame], target_ms: float, max_ms: float) -> dict[str, object]:
    other_timestamps = [frame.timestamp_us for frame in other]
    deltas_ms: list[float] = []
    worst: dict[str, object] | None = None
    for ref in reference:
        delta_us, matched = nearest_delta_us(ref.timestamp_us, other, other_timestamps)
        delta_ms = delta_us / 1000.0
        deltas_ms.append(delta_ms)
        if worst is None or delta_ms > worst["delta_ms"]:
            worst = {
                "delta_ms": delta_ms,
                "reference_frame_id": ref.frame_id,
                "reference_timestamp_us": ref.timestamp_us,
                "matched_frame_id": matched.frame_id,
                "matched_timestamp_us": matched.timestamp_us,
            }
    target_count = sum(1 for value in deltas_ms if value <= target_ms)
    max_count = sum(1 for value in deltas_ms if value <= max_ms)
    return {
        "pairs": len(deltas_ms),
        "target_ms": target_ms,
        "max_ms": max_ms,
        "within_target": target_count,
        "within_target_ratio": target_count / len(deltas_ms),
        "within_max": max_count,
        "within_max_ratio": max_count / len(deltas_ms),
        "mean_ms": statistics.fmean(deltas_ms),
        "p50_ms": percentile(deltas_ms, 0.50),
        "p90_ms": percentile(deltas_ms, 0.90),
        "p95_ms": percentile(deltas_ms, 0.95),
        "p99_ms": percentile(deltas_ms, 0.99),
        "max_delta_ms": max(deltas_ms),
        "worst": worst,
    }


def fmt_ms(value: object) -> str:
    return f"{float(value):.3f}ms"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", help="reference segment directory or frames.csv")
    parser.add_argument("others", nargs="+", help="other segment directories or frames.csv files")
    parser.add_argument("--target-ms", type=float, default=10.0, help="preferred RGB sync threshold")
    parser.add_argument("--max-ms", type=float, default=33.333, help="hard RGB one-frame threshold")
    parser.add_argument("--timestamp-column", default="global_timestamp_us",
                        help="timestamp column to compare; use auto for legacy CSV fallback")
    parser.add_argument("--limit", type=int, default=0, help="limit reference frame count from the end")
    parser.add_argument("--json", action="store_true", help="print JSON instead of text")
    args = parser.parse_args()

    reference_path = frames_csv_path(args.reference)
    reference = read_rgb_frames(reference_path, args.timestamp_column)
    if args.limit > 0:
        reference = reference[-args.limit :]

    result = {
        "reference": {
            "label": default_label(reference_path),
            "path": str(reference_path),
            "rgb_frames": len(reference),
        },
        "comparisons": [],
    }

    for value in args.others:
        other_path = frames_csv_path(value)
        other = read_rgb_frames(other_path, args.timestamp_column)
        summary = summarize(reference, other, args.target_ms, args.max_ms)
        summary.update(
            {
                "label": default_label(other_path),
                "path": str(other_path),
                "rgb_frames": len(other),
            }
        )
        result["comparisons"].append(summary)

    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0

    print(f"Reference: {result['reference']['label']} ({result['reference']['rgb_frames']} RGB frames)")
    print(f"Target: <= {args.target_ms:.3f}ms, hard limit: <= {args.max_ms:.3f}ms")
    for item in result["comparisons"]:
        print()
        print(f"Compare: {item['label']} ({item['rgb_frames']} RGB frames)")
        print(
            "  pairs={pairs} target_pass={within_target}/{pairs} ({target_pct:.1f}%) "
            "limit_pass={within_max}/{pairs} ({max_pct:.1f}%)".format(
                pairs=item["pairs"],
                within_target=item["within_target"],
                target_pct=100.0 * float(item["within_target_ratio"]),
                within_max=item["within_max"],
                max_pct=100.0 * float(item["within_max_ratio"]),
            )
        )
        print(
            "  mean={mean} p50={p50} p90={p90} p95={p95} p99={p99} max={max_delta}".format(
                mean=fmt_ms(item["mean_ms"]),
                p50=fmt_ms(item["p50_ms"]),
                p90=fmt_ms(item["p90_ms"]),
                p95=fmt_ms(item["p95_ms"]),
                p99=fmt_ms(item["p99_ms"]),
                max_delta=fmt_ms(item["max_delta_ms"]),
            )
        )
        worst = item["worst"] or {}
        print(
            "  worst ref_frame={ref} other_frame={other} ref_ts={ref_ts} other_ts={other_ts}".format(
                ref=worst.get("reference_frame_id", ""),
                other=worst.get("matched_frame_id", ""),
                ref_ts=worst.get("reference_timestamp_us", ""),
                other_ts=worst.get("matched_timestamp_us", ""),
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
