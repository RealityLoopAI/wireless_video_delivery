#!/usr/bin/env python3

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
from pathlib import Path


FIELDS = [
    "stream_type", "frame_id", "global_timestamp_us", "rgb_recorded",
    "recording_window_valid", "segment_window_valid", "rgb_video_frame_index",
    "timestamp_us", "pair_id", "pair_id_valid", "frame_system_timestamp_us",
    "clock_sync_valid",
]


def write_frames(path: Path, timestamps: list[int], invalid_index: int | None = None,
                 invalid_clock_index: int | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        for index, timestamp in enumerate(timestamps):
            writer.writerow({
                "stream_type": "rgb",
                "frame_id": index + 1,
                "global_timestamp_us": timestamp,
                "rgb_recorded": 1,
                "recording_window_valid": 1,
                "segment_window_valid": 0 if index == invalid_index else 1,
                "rgb_video_frame_index": index,
                # Native RGB has a host timestamp, but no SDK device time/pair.
                "timestamp_us": 0,
                "pair_id": 0,
                "pair_id_valid": 0,
                "frame_system_timestamp_us": timestamp - 100,
                "clock_sync_valid": 0 if index == invalid_clock_index else 1,
            })


def main() -> int:
    tool = Path(__file__).resolve().parents[1] / "05_tools" / "build_rgb_sync_manifest.py"
    with tempfile.TemporaryDirectory(prefix="gwv3_sync_manifest_") as temporary_text:
        temporary = Path(temporary_text)
        a = temporary / "a" / "test_Short_0001_frames.csv"
        b = temporary / "b" / "frames.csv"
        output = temporary / "manifest.csv"
        write_frames(a, [1_000_000, 1_033_333, 1_066_666, 1_099_999])
        write_frames(b, [1_004_000, 1_037_333, 1_070_666, 1_103_999], invalid_index=2)
        completed = subprocess.run([
            sys.executable, str(tool), "--output", str(output), "--max-delta-ms", "10",
            f"a={a}", f"b={b}",
        ], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        assert completed.returncode == 0, completed.stderr
        with output.open("r", encoding="utf-8", newline="") as handle:
            rows = list(csv.DictReader(handle))
        assert len(rows) == 3
        assert [int(row["b_delta_us"]) for row in rows] == [4000, 4000, 4000]
        assert [row["b_video_frame_index"] for row in rows] == ["0", "1", "3"]
        write_frames(a, [1_000_000, 1_033_333, 1_066_666, 1_099_999], invalid_clock_index=1)
        completed = subprocess.run([
            sys.executable, str(tool), "--output", str(output), "--max-delta-ms", "10",
            f"a={a}", f"b={b}",
        ], text=True, capture_output=True, check=False)
        assert completed.returncode == 0, completed.stderr
        with output.open("r", encoding="utf-8", newline="") as handle:
            rows = list(csv.DictReader(handle))
        assert [row["a_video_frame_index"] for row in rows] == ["0", "3"]
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
