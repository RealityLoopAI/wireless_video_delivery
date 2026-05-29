#!/usr/bin/env python3
"""Export one GWV3 receiver segment to an Orbbec-style delivery folder."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
from fractions import Fraction
from pathlib import Path
from typing import Any


DEPTH_CSV_FIELDS = [
    "frame_index",
    "timeline_frame_index",
    "timeline_offset_frames",
    "depth_file",
    "depth_video_frame_index",
    "video_pts_sec",
    "planned_host_epoch",
    "planned_host_utc",
    "emit_host_epoch",
    "emit_host_utc",
    "source_host_epoch",
    "source_host_utc",
    "source_monotonic_sec",
    "device_timestamp_us",
    "depth_source_seq",
    "duplicated",
    "placeholder",
    "depth_width",
    "depth_height",
    "depth_value_scale",
    "min_depth_mm",
    "max_depth_mm",
    "valid_pixel_ratio",
    "write_start_epoch",
    "write_end_epoch",
    "write_latency_ms",
    "write_ok",
    "error",
    "receiver_local_time_us",
    "source_system_timestamp_us",
    "payload_size",
]


def die(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def run(command: list[str]) -> None:
    try:
        subprocess.run(command, check=True)
    except FileNotFoundError:
        die(f"command not found: {command[0]}")
    except subprocess.CalledProcessError as exc:
        die(f"command failed with exit code {exc.returncode}: {' '.join(command)}")


def capture_json(command: list[str]) -> dict[str, Any]:
    try:
        result = subprocess.run(command, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except FileNotFoundError:
        die(f"command not found: {command[0]}")
    except subprocess.CalledProcessError as exc:
        die(exc.stderr.strip() or f"command failed: {' '.join(command)}")
    return json.loads(result.stdout)


def ffprobe_stream(ffprobe: str, path: Path) -> dict[str, Any]:
    data = capture_json(
        [
            ffprobe,
            "-hide_banner",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=codec_name,width,height,pix_fmt,avg_frame_rate,r_frame_rate,duration,nb_frames",
            "-of",
            "json",
            str(path),
        ]
    )
    streams = data.get("streams") or []
    if not streams:
        die(f"no video stream found: {path}")
    return streams[0]


def parse_rate(value: Any, fallback: float = 30.0) -> float:
    if not value or value == "0/0":
        return fallback
    try:
        return float(Fraction(str(value)))
    except (ValueError, ZeroDivisionError):
        return fallback


def fps_label(fps: float) -> str:
    rounded = round(fps)
    if abs(fps - rounded) < 0.001:
        return str(int(rounded))
    return f"{fps:.3f}".rstrip("0").rstrip(".").replace(".", "p")


def epoch_from_us(value: Any) -> str:
    parsed = parse_int(value)
    if parsed is None or parsed <= 0:
        return ""
    return f"{parsed / 1_000_000.0:.6f}"


def utc_from_epoch(value: str) -> str:
    if not value:
        return ""
    instant = dt.datetime.fromtimestamp(float(value), tz=dt.timezone.utc)
    return instant.isoformat().replace("+00:00", "Z")


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def parse_int(value: Any) -> int | None:
    if value is None or value == "":
        return None
    try:
        return int(float(str(value)))
    except ValueError:
        return None


def load_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle)
    except FileNotFoundError:
        die(f"missing required file: {path}")
    except json.JSONDecodeError as exc:
        die(f"invalid json {path}: {exc}")


def parse_camera_announce(meta: dict[str, Any]) -> dict[str, Any]:
    raw = meta.get("camera_announce_raw") or ""
    if not raw:
        return {}
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return {}


def load_calibration_doc(segment_dir: Path, meta: dict[str, Any]) -> tuple[dict[str, Any], Path | None]:
    name = str(meta.get("calibration_file") or "calibration.json")
    candidates = [segment_dir / name]
    if name == "calibration.json":
        candidates.extend(sorted(segment_dir.glob("*_calibration.json")))
    for path in candidates:
        if path.exists():
            return load_json(path), path
    return {}, None


def object_field(*values: Any) -> dict[str, Any]:
    for value in values:
        if isinstance(value, dict) and value:
            return value
    return {}


def sanitize_token(value: Any, fallback: str) -> str:
    text = str(value or "").strip() or fallback
    text = re.sub(r'[<>:"/\\|?*\s]+', "_", text)
    text = re.sub(r"_+", "_", text).strip("._")
    return (text or fallback)[:96]


def make_session_name(meta: dict[str, Any]) -> str:
    start_us = parse_int(meta.get("segment_start_us")) or 0
    if start_us > 0:
        local = dt.datetime.fromtimestamp(start_us / 1_000_000.0)
        return f"multi_orbbec_{local.strftime('%Y%m%d_%H%M%S_%f')}"
    return f"multi_orbbec_{dt.datetime.now().strftime('%Y%m%d_%H%M%S_%f')}"


def install_media(src: Path, dst: Path, mode: str) -> str:
    if not src.exists():
        return "missing"
    if dst.exists() or dst.is_symlink():
        dst.unlink()
    if mode == "copy":
        shutil.copy2(src, dst)
        return "copy"
    if mode == "symlink":
        os.symlink(src, dst)
        return "symlink"
    if mode in {"hardlink", "auto"}:
        try:
            os.link(src, dst)
            return "hardlink"
        except OSError:
            if mode == "hardlink":
                raise
            shutil.copy2(src, dst)
            return "copy"
    die(f"unsupported media mode: {mode}")
    return ""


def read_rows(path: Path) -> list[dict[str, str]]:
    try:
        with path.open("r", encoding="utf-8", newline="") as handle:
            return list(csv.DictReader(handle))
    except FileNotFoundError:
        die(f"missing required file: {path}")


def depth_rows(source_rows: list[dict[str, str]]) -> list[dict[str, str]]:
    return [row for row in source_rows if row.get("stream_type") == "depth"]


def write_depth_csv(
    path: Path,
    rows: list[dict[str, str]],
    depth_file_name: str,
    depth_width: int,
    depth_height: int,
    depth_fps: float,
    depth_scale: float,
    segment_start_us: int,
    png_dir_name: str | None,
) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=DEPTH_CSV_FIELDS)
        writer.writeheader()
        for index, row in enumerate(rows):
            local_epoch = epoch_from_us(row.get("local_time_us"))
            source_system_us = row.get("depth_system_timestamp_us") or row.get("packet_system_timestamp_us") or ""
            source_epoch = epoch_from_us(source_system_us)
            video_pts = index / depth_fps if depth_fps > 0 else 0.0
            planned_epoch = f"{(segment_start_us / 1_000_000.0) + video_pts:.6f}" if segment_start_us else ""
            frame_file = depth_file_name
            if png_dir_name:
                frame_file = f"{png_dir_name}/camera_00_depth_{index:08d}.png"
            writer.writerow(
                {
                    "frame_index": index,
                    "timeline_frame_index": index,
                    "timeline_offset_frames": 0,
                    "depth_file": frame_file,
                    "depth_video_frame_index": index,
                    "video_pts_sec": f"{video_pts:.6f}",
                    "planned_host_epoch": planned_epoch,
                    "planned_host_utc": utc_from_epoch(planned_epoch),
                    "emit_host_epoch": local_epoch,
                    "emit_host_utc": utc_from_epoch(local_epoch),
                    "source_host_epoch": source_epoch,
                    "source_host_utc": utc_from_epoch(source_epoch),
                    "source_monotonic_sec": "",
                    "device_timestamp_us": row.get("depth_timestamp_us") or "",
                    "depth_source_seq": row.get("depth_frame_id") or "",
                    "duplicated": "false",
                    "placeholder": "false",
                    "depth_width": row.get("width") or depth_width,
                    "depth_height": row.get("height") or depth_height,
                    "depth_value_scale": depth_scale,
                    "min_depth_mm": "",
                    "max_depth_mm": "",
                    "valid_pixel_ratio": "",
                    "write_start_epoch": "",
                    "write_end_epoch": local_epoch,
                    "write_latency_ms": "",
                    "write_ok": "true",
                    "error": "",
                    "receiver_local_time_us": row.get("local_time_us") or "",
                    "source_system_timestamp_us": source_system_us,
                    "payload_size": row.get("payload_size") or "",
                }
            )


def generate_depth_preview(ffmpeg: str, src: Path, dst: Path, fps: float) -> None:
    run(
        [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(src),
            "-vf",
            "pseudocolor=preset=turbo,format=yuv420p",
            "-c:v",
            "libx264",
            "-preset",
            "veryfast",
            "-crf",
            "23",
            "-r",
            f"{fps:.6f}",
            str(dst),
        ]
    )


def export_depth_png(ffmpeg: str, src: Path, dst_dir: Path) -> None:
    dst_dir.mkdir(parents=True, exist_ok=True)
    run(
        [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(src),
            "-start_number",
            "0",
            "-compression_level",
            "1",
            str(dst_dir / "camera_00_depth_%08d.png"),
        ]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("segment_dir", type=Path, help="GWV3 segment directory containing rgb.mp4/depth.mkv/frames.csv/meta.json")
    parser.add_argument("-o", "--output-dir", type=Path, help="Output root. The session directory is created inside this root.")
    parser.add_argument("--session-name", help="Override generated session directory name")
    parser.add_argument("--camera-index", type=int, default=0)
    parser.add_argument("--media-mode", choices=["auto", "copy", "hardlink", "symlink"], default="auto")
    parser.add_argument("--no-depth-preview", action="store_true")
    parser.add_argument("--export-depth-png", action="store_true", help="Also export uint16 depth PNG frames")
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()

    segment_dir = args.segment_dir.resolve()
    meta = load_json(segment_dir / "meta.json")
    announce = parse_camera_announce(meta)
    calibration_doc, calibration_path = load_calibration_doc(segment_dir, meta)
    device = object_field(calibration_doc.get("device"), announce.get("device"))

    rgb_src = segment_dir / str(meta.get("rgb_file") or "rgb.mp4")
    depth_src = segment_dir / str(meta.get("depth_file") or "depth.mkv")
    frames_src = segment_dir / str(meta.get("frames_file") or "frames.csv")
    if not rgb_src.exists():
        die(f"missing RGB file: {rgb_src}")
    if not depth_src.exists():
        die(f"missing depth file: {depth_src}")

    rgb_stream = ffprobe_stream(args.ffprobe, rgb_src)
    depth_stream = ffprobe_stream(args.ffprobe, depth_src)
    rgb_fps = parse_rate(rgb_stream.get("avg_frame_rate") or rgb_stream.get("r_frame_rate"))
    depth_fps = parse_rate(depth_stream.get("avg_frame_rate") or depth_stream.get("r_frame_rate"), rgb_fps)

    rows = read_rows(frames_src)
    drows = depth_rows(rows)
    segment_start_us = parse_int(meta.get("segment_start_us")) or 0
    session_name = sanitize_token(args.session_name or make_session_name(meta), "multi_orbbec_session")
    output_root = (args.output_dir.resolve() if args.output_dir else segment_dir / "orbbec_delivery")
    session_dir = output_root / session_name
    if session_dir.exists():
        if not args.overwrite:
            die(f"output session exists, pass --overwrite to replace it: {session_dir}")
        shutil.rmtree(session_dir)
    session_dir.mkdir(parents=True, exist_ok=True)

    sender_id = str(meta.get("sender_id") or "")
    camera_id = str(meta.get("camera_id") or "")
    serial = sanitize_token(device.get("serial_number") or f"{sender_id}_{camera_id}", "unknown_serial")
    camera_index = args.camera_index
    camera_prefix = f"camera_{camera_index:02d}_{serial}"

    rgb_width = int(rgb_stream.get("width") or 0)
    rgb_height = int(rgb_stream.get("height") or 0)
    depth_width = int(depth_stream.get("width") or meta.get("depth_width") or 0)
    depth_height = int(depth_stream.get("height") or meta.get("depth_height") or 0)
    rgb_name = f"{camera_prefix}_rgb_{rgb_width}x{rgb_height}_{fps_label(rgb_fps)}fps.mp4"
    camera_json_name = f"{camera_prefix}_rgb_{rgb_width}x{rgb_height}_{fps_label(rgb_fps)}fps.json"
    depth_name = f"{camera_prefix}_depth_{depth_width}x{depth_height}_{fps_label(depth_fps)}fps.mkv"
    depth_csv_name = f"{camera_prefix}_depth_frames.csv"
    depth_preview_name = f"{camera_prefix}_depth_preview_{fps_label(depth_fps)}fps.mp4"
    depth_png_dir_name = f"{camera_prefix}_depth_png" if args.export_depth_png else None

    rgb_install_mode = install_media(rgb_src, session_dir / rgb_name, args.media_mode)
    depth_install_mode = install_media(depth_src, session_dir / depth_name, args.media_mode)

    if not args.no_depth_preview:
        generate_depth_preview(args.ffmpeg, depth_src, session_dir / depth_preview_name, depth_fps)

    if args.export_depth_png and depth_png_dir_name:
        export_depth_png(args.ffmpeg, depth_src, session_dir / depth_png_dir_name)

    depth_profile = object_field(calibration_doc.get("depth_profile"), announce.get("depth_profile"))
    rgb_profile = object_field(calibration_doc.get("rgb_profile"), announce.get("rgb_profile"))
    depth_scale = float(depth_profile.get("depth_scale") or 1.0)
    write_depth_csv(
        session_dir / depth_csv_name,
        drows,
        depth_name,
        depth_width,
        depth_height,
        depth_fps,
        depth_scale,
        segment_start_us,
        depth_png_dir_name,
    )

    generated_at = utc_now()
    files = {
        "rgb_video": rgb_name,
        "depth_video": depth_name,
        "depth_frames_csv": depth_csv_name,
        "depth_preview_video": "" if args.no_depth_preview else depth_preview_name,
        "depth_png_dir": depth_png_dir_name or "",
    }
    camera_json = {
        "schema": "orbbec_multi_rgbd_camera_metadata_v1",
        "schema_version": 1,
        "final": True,
        "generated_by": "gwv3_export_orbbec_delivery",
        "generated_at_utc": generated_at,
        "camera_index": camera_index,
        "camera_id": camera_id,
        "sender_id": sender_id,
        "serial_number": serial,
        "device": device,
        "rgb_profile": {
            **rgb_profile,
            "width": rgb_width,
            "height": rgb_height,
            "fps": rgb_fps,
            "codec": rgb_stream.get("codec_name") or rgb_profile.get("codec") or "h264",
            "pix_fmt": rgb_stream.get("pix_fmt") or "",
        },
        "depth_profile": {
            **depth_profile,
            "width": depth_width,
            "height": depth_height,
            "fps": depth_fps,
            "pixel_format": "uint16",
            "storage_codec": depth_stream.get("codec_name") or "ffv1",
            "storage_pix_fmt": depth_stream.get("pix_fmt") or "gray16le",
            "depth_scale": depth_scale,
        },
        "calibration": object_field(calibration_doc.get("calibration"), announce.get("calibration"), {"available": False}),
        "files": files,
        "frame_counts": {
            "rgb_packets": sum(1 for row in rows if row.get("stream_type") == "rgb"),
            "depth_frames": len(drows),
        },
        "timestamps": {
            "segment_start_us": meta.get("segment_start_us"),
            "segment_end_us": meta.get("segment_end_us"),
        },
        "source": {
            "format": "gwv3_receiver_segment_v1",
            "segment_dir": str(segment_dir),
            "frames_csv": str(frames_src),
            "meta_json": str(segment_dir / "meta.json"),
            "calibration_json": str(calibration_path) if calibration_path else "",
            "rgb_install_mode": rgb_install_mode,
            "depth_install_mode": depth_install_mode,
        },
        "notes": [
            "RGB and depth are delivered as video files because downstream can consume both recordings.",
            "depth_frames_csv preserves the Orbbec-style timing/index fields and references the depth video frame index.",
        ],
    }
    (session_dir / camera_json_name).write_text(json.dumps(camera_json, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    session_json = {
        "schema": "orbbec_multi_rgbd_session_metadata_v1",
        "schema_version": 1,
        "generated_by": "gwv3_export_orbbec_delivery",
        "generated_at_utc": generated_at,
        "session_name": session_name,
        "source_format": "gwv3_receiver_segment_v1",
        "delivery_format": "gwv3_orbbec_compatible_video_v1",
        "session_dir": str(session_dir),
        "recording": {
            "segment_start_us": meta.get("segment_start_us"),
            "segment_end_us": meta.get("segment_end_us"),
            "closed": meta.get("closed"),
        },
        "cameras": [
            {
                "camera_index": camera_index,
                "sender_id": sender_id,
                "camera_id": camera_id,
                "serial_number": serial,
                "metadata_file": camera_json_name,
                "rgb_video": rgb_name,
                "depth_video": depth_name,
                "depth_frames_csv": depth_csv_name,
                "depth_preview_video": "" if args.no_depth_preview else depth_preview_name,
                "depth_png_dir": depth_png_dir_name or "",
            }
        ],
    }
    (session_dir / "session.json").write_text(json.dumps(session_json, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print(session_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
