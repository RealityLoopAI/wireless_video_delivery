#!/usr/bin/env python3
"""Analyze real receiver segment FPS from frames.csv and media metadata."""

from __future__ import annotations

import argparse
import csv
import json
import statistics
import subprocess
import sys
from fractions import Fraction
from pathlib import Path
from typing import Any


def die(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def parse_int(value: Any) -> int | None:
    if value is None or value == "":
        return None
    try:
        return int(float(str(value)))
    except ValueError:
        return None


def parse_rate(value: Any) -> float | None:
    if not value or value == "0/0":
        return None
    try:
        return float(Fraction(str(value)))
    except (ValueError, ZeroDivisionError):
        return None


def capture_json(command: list[str]) -> dict[str, Any]:
    try:
        result = subprocess.run(command, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except FileNotFoundError:
        die(f"command not found: {command[0]}")
    except subprocess.CalledProcessError as exc:
        die(exc.stderr.strip() or f"command failed: {' '.join(command)}")
    return json.loads(result.stdout)


def ffprobe_stream(ffprobe: str, path: Path) -> dict[str, Any] | None:
    if not path.exists():
        return None
    data = capture_json(
        [
            ffprobe,
            "-hide_banner",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=codec_name,width,height,pix_fmt,avg_frame_rate,r_frame_rate,duration,nb_frames,bit_rate",
            "-of",
            "json",
            str(path),
        ]
    )
    streams = data.get("streams") or []
    return streams[0] if streams else None


def read_rows(path: Path) -> list[dict[str, str]]:
    try:
        with path.open("r", encoding="utf-8", newline="") as handle:
            return list(csv.DictReader(handle))
    except FileNotFoundError:
        die(f"missing frames.csv: {path}")


def summarize_rows(rows: list[dict[str, str]], stream_type: str) -> dict[str, Any]:
    stream_rows = [row for row in rows if row.get("stream_type") == stream_type]
    result: dict[str, Any] = {
        "rows": len(stream_rows),
        "duration_s": 0.0,
        "arrival_fps": 0.0,
        "avg_interval_ms": 0.0,
        "min_interval_ms": 0.0,
        "p50_interval_ms": 0.0,
        "p95_interval_ms": 0.0,
        "max_interval_ms": 0.0,
        "over_50ms": 0,
        "over_100ms": 0,
        "payload_mbps": 0.0,
        "width": 0,
        "height": 0,
    }
    if not stream_rows:
        return result
    times = [parse_int(row.get("local_time_us")) for row in stream_rows]
    times = [item for item in times if item is not None]
    payload_sizes = [parse_int(row.get("payload_size")) or 0 for row in stream_rows]
    if stream_rows:
        result["width"] = parse_int(stream_rows[-1].get("width")) or 0
        result["height"] = parse_int(stream_rows[-1].get("height")) or 0
    if len(times) < 2:
        return result
    intervals = [(b - a) / 1000.0 for a, b in zip(times, times[1:])]
    duration_s = (times[-1] - times[0]) / 1_000_000.0
    result["duration_s"] = duration_s
    if duration_s > 0:
        result["arrival_fps"] = (len(times) - 1) / duration_s
        result["payload_mbps"] = (sum(payload_sizes) * 8.0) / duration_s / 1_000_000.0
    result["avg_interval_ms"] = statistics.mean(intervals)
    result["min_interval_ms"] = min(intervals)
    result["p50_interval_ms"] = statistics.median(intervals)
    if len(intervals) >= 20:
        result["p95_interval_ms"] = statistics.quantiles(intervals, n=20)[18]
    else:
        result["p95_interval_ms"] = max(intervals)
    result["max_interval_ms"] = max(intervals)
    result["over_50ms"] = sum(1 for item in intervals if item > 50)
    result["over_100ms"] = sum(1 for item in intervals if item > 100)
    return result


def media_summary(stream: dict[str, Any] | None) -> dict[str, Any]:
    if not stream:
        return {}
    return {
        "codec": stream.get("codec_name") or "",
        "width": int(stream.get("width") or 0),
        "height": int(stream.get("height") or 0),
        "pix_fmt": stream.get("pix_fmt") or "",
        "avg_fps": parse_rate(stream.get("avg_frame_rate")),
        "r_fps": parse_rate(stream.get("r_frame_rate")),
        "duration_s": float(stream.get("duration") or 0),
        "nb_frames": int(stream.get("nb_frames") or 0),
        "bit_rate_mbps": (int(stream.get("bit_rate") or 0) / 1_000_000.0) if stream.get("bit_rate") else None,
    }


def print_stream(name: str, rows_summary: dict[str, Any], media: dict[str, Any]) -> None:
    print(f"{name}:")
    if media:
        rate = media.get("avg_fps")
        rate_text = f"{rate:.2f}fps" if isinstance(rate, float) else "unknown"
        bitrate = media.get("bit_rate_mbps")
        bitrate_text = f"{bitrate:.2f}Mbps" if isinstance(bitrate, float) else "unknown"
        frames = media.get("nb_frames") or "unknown"
        duration = media.get("duration_s") or 0
        print(
            f"  media: {media.get('codec')} {media.get('width')}x{media.get('height')} "
            f"{media.get('pix_fmt')} {rate_text}, duration={duration:.3f}s, frames={frames}, bitrate={bitrate_text}"
        )
    print(
        f"  arrival: rows={rows_summary['rows']}, fps={rows_summary['arrival_fps']:.2f}, "
        f"duration={rows_summary['duration_s']:.3f}s, payload={rows_summary['payload_mbps']:.2f}Mbps"
    )
    print(
        f"  intervals: avg={rows_summary['avg_interval_ms']:.2f}ms, p50={rows_summary['p50_interval_ms']:.2f}ms, "
        f"p95={rows_summary['p95_interval_ms']:.2f}ms, min={rows_summary['min_interval_ms']:.2f}ms, "
        f"max={rows_summary['max_interval_ms']:.2f}ms, >50ms={rows_summary['over_50ms']}, >100ms={rows_summary['over_100ms']}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("segment_dir", type=Path)
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON")
    args = parser.parse_args()

    segment_dir = args.segment_dir.resolve()
    rows = read_rows(segment_dir / "frames.csv")
    result = {
        "segment_dir": str(segment_dir),
        "rgb": summarize_rows(rows, "rgb"),
        "depth": summarize_rows(rows, "depth"),
        "rgb_media": media_summary(ffprobe_stream(args.ffprobe, segment_dir / "rgb.mp4")),
        "depth_media": media_summary(ffprobe_stream(args.ffprobe, segment_dir / "depth.mkv")),
    }
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    print(f"segment: {segment_dir}")
    print_stream("RGB", result["rgb"], result["rgb_media"])
    print_stream("Depth", result["depth"], result["depth_media"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
