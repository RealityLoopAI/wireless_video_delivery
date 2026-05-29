#!/usr/bin/env python3
"""Generate an offline depth_aligned_to_rgb.mkv for one GWV3 segment."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

import cv2
import numpy as np


def die(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def load_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle)
    except FileNotFoundError:
        die(f"missing required file: {path}")
    except json.JSONDecodeError as exc:
        die(f"invalid json {path}: {exc}")


def find_json_file(segment_dir: Path, preferred_name: str) -> Path:
    direct = segment_dir / preferred_name
    if direct.exists():
        return direct
    matches = sorted(segment_dir.glob(f"*_{preferred_name}"))
    if len(matches) == 1:
        return matches[0]
    if not matches:
        die(f"missing required file: {segment_dir / preferred_name}")
    die(f"multiple candidates for {preferred_name}: {', '.join(str(item.name) for item in matches)}")


def parse_float(value: Any, fallback: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return fallback


def parse_int(value: Any, fallback: int = 0) -> int:
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return fallback


def require_object(value: Any, name: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        die(f"{name} must be an object")
    return value


def intrinsic_matrix(intrinsic: dict[str, Any], name: str, expected_width: int, expected_height: int) -> np.ndarray:
    fx = parse_float(intrinsic.get("fx"))
    fy = parse_float(intrinsic.get("fy"))
    cx = parse_float(intrinsic.get("cx"))
    cy = parse_float(intrinsic.get("cy"))
    width = parse_int(intrinsic.get("width"))
    height = parse_int(intrinsic.get("height"))
    if fx <= 0.0 or fy <= 0.0:
        die(f"{name} has invalid fx/fy")
    if width <= 0 or height <= 0:
        die(f"{name} has invalid width/height")
    if width != expected_width or height != expected_height:
        die(f"{name} dimensions {width}x{height} do not match video/profile {expected_width}x{expected_height}")
    return np.array([[fx, 0.0, cx], [0.0, fy, cy], [0.0, 0.0, 1.0]], dtype=np.float64)


def distortion_coeffs(distortion: dict[str, Any]) -> np.ndarray:
    # Orbbec exposes k1..k6,p1,p2; OpenCV rational order is k1,k2,p1,p2,k3,k4,k5,k6.
    return np.array(
        [
            parse_float(distortion.get("k1")),
            parse_float(distortion.get("k2")),
            parse_float(distortion.get("p1")),
            parse_float(distortion.get("p2")),
            parse_float(distortion.get("k3")),
            parse_float(distortion.get("k4")),
            parse_float(distortion.get("k5")),
            parse_float(distortion.get("k6")),
        ],
        dtype=np.float64,
    )


def ffprobe_stream(ffprobe: str, path: Path) -> dict[str, Any]:
    command = [
        ffprobe,
        "-hide_banner",
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height,pix_fmt,avg_frame_rate,r_frame_rate",
        "-of",
        "json",
        str(path),
    ]
    try:
        result = subprocess.run(command, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except FileNotFoundError:
        die(f"command not found: {ffprobe}")
    except subprocess.CalledProcessError as exc:
        die(exc.stderr.strip() or f"ffprobe failed: {path}")
    streams = json.loads(result.stdout).get("streams") or []
    if not streams:
        die(f"no video stream found: {path}")
    return streams[0]


def fps_from_meta(meta: dict[str, Any], stream: dict[str, Any]) -> float:
    for key in ("depth_record_fps", "depth_actual_fps", "depth_fps"):
        value = parse_float(meta.get(key), 0.0)
        if value > 0.0:
            return value
    rate = str(stream.get("avg_frame_rate") or stream.get("r_frame_rate") or "")
    if "/" in rate:
        num, den = rate.split("/", 1)
        denominator = parse_float(den, 0.0)
        if denominator > 0.0:
            return parse_float(num, 0.0) / denominator
    return 30.0


def read_exact(stream: Any, size: int) -> bytes:
    chunks: list[bytes] = []
    remaining = size
    while remaining > 0:
        chunk = stream.read(remaining)
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


class DepthAligner:
    def __init__(
        self,
        depth_width: int,
        depth_height: int,
        rgb_width: int,
        rgb_height: int,
        depth_scale: float,
        calibration: dict[str, Any],
    ) -> None:
        data = require_object(calibration.get("data"), "calibration.data")
        depth_intrinsic = require_object(data.get("depth_intrinsic"), "calibration.data.depth_intrinsic")
        rgb_intrinsic = require_object(data.get("rgb_intrinsic"), "calibration.data.rgb_intrinsic")
        depth_distortion = require_object(data.get("depth_distortion"), "calibration.data.depth_distortion")
        rgb_distortion = require_object(data.get("rgb_distortion"), "calibration.data.rgb_distortion")
        d2c_transform = require_object(data.get("d2c_transform"), "calibration.data.d2c_transform")

        self.depth_width = depth_width
        self.depth_height = depth_height
        self.rgb_width = rgb_width
        self.rgb_height = rgb_height
        self.depth_scale = depth_scale
        self.rgb_k = intrinsic_matrix(rgb_intrinsic, "rgb_intrinsic", rgb_width, rgb_height)
        self.rgb_d = distortion_coeffs(rgb_distortion)

        depth_k = intrinsic_matrix(depth_intrinsic, "depth_intrinsic", depth_width, depth_height)
        depth_d = distortion_coeffs(depth_distortion)
        rot = d2c_transform.get("rot") or []
        trans = d2c_transform.get("trans_mm") or []
        if len(rot) != 9 or len(trans) != 3:
            die("calibration.data.d2c_transform must contain rot[9] and trans_mm[3]")
        self.rot = np.array(rot, dtype=np.float64).reshape(3, 3)
        self.trans = np.array(trans, dtype=np.float64).reshape(3, 1)
        if not np.any(np.abs(self.rot) > 0.0) or not np.any(np.abs(self.trans) > 0.0):
            die("calibration.data.d2c_transform is all zero; cannot align depth to RGB")

        ys, xs = np.indices((depth_height, depth_width), dtype=np.float32)
        pixels = np.stack([xs.reshape(-1), ys.reshape(-1)], axis=1).reshape(-1, 1, 2)
        undistorted = cv2.undistortPoints(pixels, depth_k, depth_d).reshape(-1, 2)
        self.depth_norm_x = undistorted[:, 0]
        self.depth_norm_y = undistorted[:, 1]

    def align(self, depth_frame: np.ndarray) -> np.ndarray:
        z_depth_mm = depth_frame.reshape(-1).astype(np.float64) * self.depth_scale
        valid = z_depth_mm > 0.0
        if not np.any(valid):
            return np.zeros((self.rgb_height, self.rgb_width), dtype=np.uint16)

        x_depth = self.depth_norm_x[valid] * z_depth_mm[valid]
        y_depth = self.depth_norm_y[valid] * z_depth_mm[valid]
        points_depth = np.stack([x_depth, y_depth, z_depth_mm[valid]], axis=0)
        points_rgb = self.rot @ points_depth + self.trans
        z_rgb = points_rgb[2, :]
        in_front = z_rgb > 0.0
        if not np.any(in_front):
            return np.zeros((self.rgb_height, self.rgb_width), dtype=np.uint16)

        object_points = points_rgb[:, in_front].T.reshape(-1, 1, 3).astype(np.float64)
        projected, _ = cv2.projectPoints(object_points, np.zeros(3), np.zeros(3), self.rgb_k, self.rgb_d)
        uv = projected.reshape(-1, 2)
        u = np.rint(uv[:, 0]).astype(np.int64)
        v = np.rint(uv[:, 1]).astype(np.int64)
        z_projected = z_rgb[in_front]

        inside = (u >= 0) & (u < self.rgb_width) & (v >= 0) & (v < self.rgb_height)
        if not np.any(inside):
            return np.zeros((self.rgb_height, self.rgb_width), dtype=np.uint16)

        flat_index = v[inside] * self.rgb_width + u[inside]
        z_inside = z_projected[inside]
        aligned_value = np.clip(np.rint(z_inside / self.depth_scale), 0, np.iinfo(np.uint16).max).astype(np.uint16)

        order = np.argsort(z_inside)
        sorted_index = flat_index[order]
        sorted_value = aligned_value[order]
        unique_index, first_pos = np.unique(sorted_index, return_index=True)

        aligned = np.zeros(self.rgb_width * self.rgb_height, dtype=np.uint16)
        aligned[unique_index] = sorted_value[first_pos]
        return aligned.reshape(self.rgb_height, self.rgb_width)


def load_calibration(segment_dir: Path, meta: dict[str, Any]) -> dict[str, Any]:
    calibration_name = str(meta.get("calibration_file") or "calibration.json")
    calibration_path = segment_dir / calibration_name
    if not calibration_path.exists():
        calibration_path = find_json_file(segment_dir, "calibration.json")
    return load_json(calibration_path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("segment_dir", type=Path, help="GWV3 segment directory")
    parser.add_argument("-o", "--output", type=Path, help="Output MKV path, default: <segment>/depth_aligned_to_rgb.mkv")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--max-frames", type=int, default=0, help="Debug limit; 0 means all frames")
    args = parser.parse_args()

    segment_dir = args.segment_dir.resolve()
    meta = load_json(find_json_file(segment_dir, "meta.json"))
    calibration_json = load_calibration(segment_dir, meta)
    calibration = require_object(calibration_json.get("calibration"), "calibration")
    if not calibration.get("available"):
        die("calibration.available is false; cannot align depth to RGB")

    depth_src = segment_dir / str(meta.get("depth_file") or "depth.mkv")
    if not depth_src.exists():
        die(f"missing depth file: {depth_src}")
    depth_stream = ffprobe_stream(args.ffprobe, depth_src)
    depth_width = parse_int(depth_stream.get("width") or meta.get("depth_width"))
    depth_height = parse_int(depth_stream.get("height") or meta.get("depth_height"))
    if depth_width <= 0 or depth_height <= 0:
        die("cannot determine depth dimensions")

    rgb_profile = require_object(calibration_json.get("rgb_profile"), "rgb_profile")
    depth_profile = require_object(calibration_json.get("depth_profile"), "depth_profile")
    rgb_width = parse_int(rgb_profile.get("width") or meta.get("rgb_width"))
    rgb_height = parse_int(rgb_profile.get("height") or meta.get("rgb_height"))
    if rgb_width <= 0 or rgb_height <= 0:
        die("cannot determine RGB dimensions")

    depth_scale = parse_float(depth_profile.get("depth_scale"), 1.0)
    if depth_scale <= 0.0:
        depth_scale = 1.0

    aligned_depth = calibration_json.get("aligned_depth")
    if not isinstance(aligned_depth, dict):
        aligned_depth = {}
    output = args.output.resolve() if args.output else segment_dir / str(aligned_depth.get("output_file") or "depth_aligned_to_rgb.mkv")
    output.parent.mkdir(parents=True, exist_ok=True)
    fps = fps_from_meta(meta, depth_stream)
    aligner = DepthAligner(depth_width, depth_height, rgb_width, rgb_height, depth_scale, calibration)

    frame_size = depth_width * depth_height * 2
    decoder = subprocess.Popen(
        [
            args.ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            str(depth_src),
            "-f",
            "rawvideo",
            "-pix_fmt",
            "gray16le",
            "pipe:1",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    encoder = subprocess.Popen(
        [
            args.ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "gray16le",
            "-s",
            f"{rgb_width}x{rgb_height}",
            "-r",
            f"{fps:.6f}",
            "-i",
            "pipe:0",
            "-c:v",
            "ffv1",
            "-level",
            "3",
            str(output),
        ],
        stdin=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if decoder.stdout is None or encoder.stdin is None:
        die("failed to open ffmpeg pipes")

    frames = 0
    try:
        while True:
            raw = read_exact(decoder.stdout, frame_size)
            if not raw:
                break
            if len(raw) != frame_size:
                die(f"short depth frame: got {len(raw)} bytes, expected {frame_size}")
            depth = np.frombuffer(raw, dtype="<u2").reshape(depth_height, depth_width)
            aligned = aligner.align(depth)
            encoder.stdin.write(aligned.astype("<u2", copy=False).tobytes())
            frames += 1
            if args.max_frames > 0 and frames >= args.max_frames:
                break
    finally:
        decoder.stdout.close()
        encoder.stdin.close()

    decoder_stderr = decoder.stderr.read().decode("utf-8", errors="replace") if decoder.stderr else ""
    encoder_stderr = encoder.stderr.read().decode("utf-8", errors="replace") if encoder.stderr else ""
    decoder_rc = decoder.wait()
    encoder_rc = encoder.wait()
    if decoder_rc != 0 and not (args.max_frames > 0 and frames >= args.max_frames):
        die(decoder_stderr.strip() or f"depth decoder failed with exit code {decoder_rc}")
    if encoder_rc != 0:
        die(encoder_stderr.strip() or f"aligned depth encoder failed with exit code {encoder_rc}")

    sidecar = output.with_suffix(".json")
    summary = {
        "schema": "gemini_aligned_depth_v1",
        "generated": True,
        "method": "offline_depth_to_rgb_projection",
        "source_depth_file": str(depth_src),
        "output_file": str(output),
        "frames": frames,
        "width": rgb_width,
        "height": rgb_height,
        "fps": fps,
        "depth_scale": depth_scale,
        "calibration_file": str(segment_dir / str(meta.get("calibration_file") or "calibration.json")),
        "z_buffer": "nearest_depth_in_rgb_camera",
    }
    sidecar.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
