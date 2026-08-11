#!/usr/bin/env python3
"""Build a monotonic multi-camera RGB frame manifest on global_timestamp_us."""

from __future__ import annotations

import argparse
import bisect
import csv
import re
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Frame:
    global_timestamp_us: int
    aligned_timestamp_us: int
    frame_id: str
    video_frame_index: str
    frames_csv: Path


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(2)


def parse_labeled_path(value: str) -> tuple[str, Path]:
    if "=" not in value:
        fail(f"stream must use LABEL=PATH: {value}")
    label, path_text = value.split("=", 1)
    label = label.strip()
    if not label or not re.fullmatch(r"[A-Za-z0-9_.-]+", label):
        fail(f"invalid stream label: {label}")
    path = Path(path_text).expanduser()
    if not path.exists():
        fail(f"stream path not found: {path}")
    return label, path


def frames_csv_files(path: Path) -> list[Path]:
    if path.is_file():
        return [path]
    direct = path / "frames.csv"
    if direct.is_file():
        return [direct]
    files = sorted(candidate for candidate in path.rglob("*frames.csv")
                   if not candidate.name.endswith((".inprogress", ".finalizing")))
    if not files:
        fail(f"no frames.csv found under: {path}")
    return files


def positive_int(value: str | None) -> int | None:
    try:
        parsed = int((value or "").strip())
    except ValueError:
        return None
    return parsed if parsed > 0 else None


def row_enabled(row: dict[str, str], field: str) -> bool:
    return field not in row or not (row.get(field) or "").strip() or (row.get(field) or "").strip() == "1"


def read_frames(path: Path, offset_us: int) -> list[Frame]:
    frames: list[Frame] = []
    seen: set[tuple[int, str, str]] = set()
    for csv_path in frames_csv_files(path):
        with csv_path.open("r", encoding="utf-8-sig", newline="") as handle:
            reader = csv.DictReader(handle)
            if not reader.fieldnames or "global_timestamp_us" not in reader.fieldnames:
                fail(f"global_timestamp_us is missing: {csv_path}")
            for row in reader:
                if (row.get("stream_type") or "").strip().lower() != "rgb":
                    continue
                if not all(row_enabled(row, field) for field in (
                    "rgb_recorded", "recording_window_valid", "segment_window_valid"
                )):
                    continue
                timestamp_us = positive_int(row.get("global_timestamp_us"))
                if timestamp_us is None:
                    continue
                frame_id = (row.get("frame_id") or row.get("rgb_frame_id") or "").strip()
                video_frame_index = (row.get("rgb_video_frame_index") or "").strip()
                key = (timestamp_us, frame_id, video_frame_index)
                if key in seen:
                    continue
                seen.add(key)
                frames.append(Frame(timestamp_us, timestamp_us + offset_us, frame_id,
                                    video_frame_index, csv_path))
    frames.sort(key=lambda frame: (frame.aligned_timestamp_us, frame.global_timestamp_us))
    if not frames:
        fail(f"no eligible recorded RGB frames: {path}")
    return frames


def nearest_after(frames: list[Frame], timestamps: list[int], target_us: int,
                  first_index: int) -> tuple[int, Frame, int] | None:
    insertion = max(first_index, bisect.bisect_left(timestamps, target_us, lo=first_index))
    candidates = {insertion - 1, insertion}
    best: tuple[int, Frame, int] | None = None
    for index in candidates:
        if index < first_index or index >= len(frames):
            continue
        frame = frames[index]
        delta_us = frame.aligned_timestamp_us - target_us
        if best is None or abs(delta_us) < abs(best[2]):
            best = (index, frame, delta_us)
    return best


def parse_offsets(values: list[str]) -> dict[str, int]:
    offsets: dict[str, int] = {}
    for value in values:
        if "=" not in value:
            fail(f"offset must use LABEL=MICROSECONDS: {value}")
        label, amount = value.split("=", 1)
        try:
            offsets[label] = int(amount)
        except ValueError:
            fail(f"invalid offset: {value}")
    return offsets


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("streams", nargs="+", help="ordered LABEL=PATH streams; first is the reference")
    parser.add_argument("--output", required=True, help="output rgb_sync_manifest.csv")
    parser.add_argument("--max-delta-ms", type=float, default=33.333)
    parser.add_argument("--content-offset-us", action="append", default=[], metavar="LABEL=US")
    args = parser.parse_args()
    if len(args.streams) < 2:
        fail("at least two streams are required")
    if args.max_delta_ms <= 0:
        fail("--max-delta-ms must be positive")

    labeled_paths = [parse_labeled_path(value) for value in args.streams]
    labels = [label for label, _ in labeled_paths]
    if len(set(labels)) != len(labels):
        fail("stream labels must be unique")
    offsets = parse_offsets(args.content_offset_us)
    unknown_offsets = set(offsets) - set(labels)
    if unknown_offsets:
        fail(f"offset labels are not streams: {', '.join(sorted(unknown_offsets))}")

    streams = {
        label: read_frames(path, offsets.get(label, 0))
        for label, path in labeled_paths
    }
    reference_label = labels[0]
    candidate_timestamps = {
        label: [frame.aligned_timestamp_us for frame in streams[label]]
        for label in labels[1:]
    }
    next_indexes = {label: 0 for label in labels[1:]}
    max_delta_us = int(round(args.max_delta_ms * 1000.0))
    matches: list[tuple[Frame, dict[str, tuple[int, Frame, int]]]] = []
    for reference in streams[reference_label]:
        row_matches: dict[str, tuple[int, Frame, int]] = {}
        for label in labels[1:]:
            nearest = nearest_after(streams[label], candidate_timestamps[label],
                                    reference.aligned_timestamp_us, next_indexes[label])
            if nearest is None or abs(nearest[2]) > max_delta_us:
                row_matches = {}
                break
            row_matches[label] = nearest
        if not row_matches:
            continue
        for label in labels[1:]:
            next_indexes[label] = row_matches[label][0] + 1
        matches.append((reference, row_matches))

    output = Path(args.output).expanduser()
    output.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = ["reference_label", "reference_aligned_timestamp_us", "max_abs_delta_us"]
    for label in labels:
        fieldnames.extend([
            f"{label}_frame_id", f"{label}_video_frame_index", f"{label}_global_timestamp_us",
            f"{label}_content_offset_us", f"{label}_aligned_timestamp_us", f"{label}_delta_us",
            f"{label}_frames_csv",
        ])
    with output.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for reference, row_matches in matches:
            matched = {
                reference_label: (reference, 0),
                **{label: (value[1], value[2]) for label, value in row_matches.items()},
            }
            row: dict[str, object] = {
                "reference_label": reference_label,
                "reference_aligned_timestamp_us": reference.aligned_timestamp_us,
                "max_abs_delta_us": max(abs(delta) for _, delta in matched.values()),
            }
            for label in labels:
                frame, delta_us = matched[label]
                row.update({
                    f"{label}_frame_id": frame.frame_id,
                    f"{label}_video_frame_index": frame.video_frame_index,
                    f"{label}_global_timestamp_us": frame.global_timestamp_us,
                    f"{label}_content_offset_us": offsets.get(label, 0),
                    f"{label}_aligned_timestamp_us": frame.aligned_timestamp_us,
                    f"{label}_delta_us": delta_us,
                    f"{label}_frames_csv": str(frame.frames_csv),
                })
            writer.writerow(row)

    print(f"wrote {len(matches)} synchronized frame groups to {output}")
    print(f"reference frames={len(streams[reference_label])} max_delta_us={max_delta_us}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
