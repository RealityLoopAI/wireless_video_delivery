#!/usr/bin/env python3
"""Read-only CSV gap audit. Does not rewrite timestamps, videos or metadata."""

import argparse
import csv
import datetime
import json
from pathlib import Path


def wall(us):
    return datetime.datetime.fromtimestamp(us / 1_000_000, datetime.timezone(
        datetime.timedelta(hours=8))).isoformat(timespec="milliseconds")


def audit(path, threshold_us=500_000):
    streams = {}
    previous = {}
    with path.open(newline="", encoding="utf-8-sig") as source:
        for row in csv.DictReader(source):
            stream = row.get("stream_type", "")
            if stream not in ("rgb", "depth"):
                continue
            if stream == "rgb" and row.get("rgb_recorded") not in (None, "", "1", "true"):
                continue
            stamp = int(row.get("global_timestamp_us") or 0)
            received = int(row.get("receiver_receive_timestamp_us") or 0)
            stats = streams.setdefault(stream, dict(
                frames=0, first_global_us=stamp, last_global_us=stamp,
                max_receive_minus_global_us=0, invalid_clock_rows=0,
                timestamp_regressions=0, gaps=[]))
            stats["frames"] += 1
            stats["last_global_us"] = stamp
            stats["invalid_clock_rows"] += row.get("clock_sync_valid") not in ("1", "true")
            if received and stamp:
                stats["max_receive_minus_global_us"] = max(
                    stats["max_receive_minus_global_us"], received - stamp)
            last = previous.get(stream)
            if last and stamp:
                delta = stamp - int(last.get("global_timestamp_us") or 0)
                stats["timestamp_regressions"] += delta < 0
                if delta > threshold_us:
                    stats["gaps"].append(dict(
                        from_time=wall(int(last["global_timestamp_us"])), to_time=wall(stamp),
                        delta_us=delta,
                        from_frame_id=last.get("frame_id"), to_frame_id=row.get("frame_id"),
                        from_video_index=last.get("rgb_video_frame_index"),
                        to_video_index=row.get("rgb_video_frame_index"),
                        before_receive_us=last.get("receiver_receive_timestamp_us"),
                        after_receive_us=row.get("receiver_receive_timestamp_us")))
            previous[stream] = row
    return dict(path=str(path), threshold_us=threshold_us, streams=streams)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_files", nargs="+", type=Path)
    parser.add_argument("--threshold-ms", type=float, default=500)
    args = parser.parse_args()
    if args.threshold_ms <= 0:
        parser.error("threshold must be positive")
    print(json.dumps([audit(path, int(args.threshold_ms * 1000)) for path in args.csv_files],
                     ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
