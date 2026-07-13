#!/usr/bin/env python3
"""Reject unfinished or ambiguously indexed recordings before multi-camera sync."""

from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable


REQUIRED_RGB_COLUMNS = {
    "stream_type",
    "frame_id",
    "frame_system_timestamp_us",
    "global_timestamp_us",
    "rgb_recorded",
    "rgb_video_frame_index",
}


class ValidationError(RuntimeError):
    pass


@dataclass(frozen=True)
class ValidationResult:
    root: str
    camera_key: str
    frames_path: str
    rgb_path: str
    rgb_frames: int
    first_global_timestamp_us: int
    last_global_timestamp_us: int
    ready_marker: str | None
    decoded_video_frames: int | None


def parse_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    return str(value).strip().lower() in {"1", "true", "yes", "on"}


def parse_int(value: Any, label: str) -> int:
    try:
        return int(value)
    except (TypeError, ValueError) as exc:
        raise ValidationError(f"invalid {label}: {value!r}") from exc


def read_json_object(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValidationError(f"cannot read {label} {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ValidationError(f"{label} must contain a JSON object: {path}")
    return value


def find_single(root: Path, direct_name: str, pattern: str, label: str) -> Path:
    direct = root / direct_name
    candidates = [direct] if direct.is_file() else sorted(root.glob(pattern))
    candidates = list(dict.fromkeys(path.resolve() for path in candidates if path.is_file()))
    if len(candidates) != 1:
        raise ValidationError(
            f"{root} must contain exactly one {label}; found {len(candidates)}"
        )
    return candidates[0]


def resolve_meta_file(root: Path, meta: dict[str, Any], field: str, pattern: str, label: str) -> Path:
    configured = meta.get(field)
    if isinstance(configured, str) and configured:
        path = (root / configured).resolve()
        if not path.is_file():
            raise ValidationError(f"{label} declared by {field} is missing: {path}")
        return path
    matches = sorted(path.resolve() for path in root.glob(pattern) if path.is_file())
    if len(matches) != 1:
        raise ValidationError(f"{root} must contain exactly one {label}; found {len(matches)}")
    return matches[0]


def ffprobe_frame_count(video_path: Path, ffprobe: str) -> int:
    executable = shutil.which(ffprobe)
    if executable is None:
        raise ValidationError(f"ffprobe executable is unavailable: {ffprobe}")
    command = [
        executable,
        "-v",
        "error",
        "-count_frames",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=nb_read_frames",
        "-of",
        "default=nk=1:nw=1",
        str(video_path),
    ]
    try:
        completed = subprocess.run(command, check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        detail = getattr(exc, "stderr", "") or str(exc)
        raise ValidationError(f"ffprobe failed for {video_path}: {detail.strip()}") from exc
    for line in completed.stdout.splitlines():
        if line.strip().isdigit():
            return int(line.strip())
    raise ValidationError(f"ffprobe did not report a decoded frame count for {video_path}")


def validate_recording(
    root: Path,
    *,
    verify_video_frames: bool = False,
    ffprobe: str = "ffprobe",
) -> ValidationResult:
    root = root.resolve()
    if not root.is_dir():
        raise ValidationError(f"recording directory does not exist: {root}")

    meta_path = find_single(root, "meta.json", "*_meta.json", "meta.json")
    meta = read_json_object(meta_path, "recording metadata")
    if meta.get("closed") is not True:
        raise ValidationError(f"recording is not finalized (meta.closed is not true): {root}")
    publish_state = meta.get("frames_publish_state")
    if publish_state is not None and publish_state != "finalized":
        raise ValidationError(f"frames.csv is not finalized ({publish_state!r}): {root}")

    ready_marker: Path | None = None
    ready_name = meta.get("recording_ready_file")
    if isinstance(ready_name, str) and ready_name:
        ready_marker = (root / ready_name).resolve()
        ready = read_json_object(ready_marker, "recording ready marker")
        if ready.get("ready") is not True:
            raise ValidationError(f"recording ready marker is not valid: {ready_marker}")

    frames_path = resolve_meta_file(root, meta, "frames_file", "*frames.csv", "frames.csv")
    rgb_path = resolve_meta_file(root, meta, "rgb_file", "*rgb.mp4", "RGB MP4")
    try:
        if rgb_path.stat().st_size <= 0:
            raise ValidationError(f"RGB MP4 is empty: {rgb_path}")
    except OSError as exc:
        raise ValidationError(f"cannot stat RGB MP4 {rgb_path}: {exc}") from exc

    rgb_count = 0
    first_global_us: int | None = None
    last_global_us: int | None = None
    with frames_path.open("r", newline="", encoding="utf-8-sig") as handle:
        reader = csv.DictReader(handle)
        fields = set(reader.fieldnames or [])
        missing = sorted(REQUIRED_RGB_COLUMNS - fields)
        if missing:
            raise ValidationError(
                f"final frames.csv lacks required columns {missing}: {frames_path}"
            )
        for row_number, row in enumerate(reader, start=2):
            if row.get("stream_type") != "rgb" or not parse_bool(row.get("rgb_recorded")):
                continue
            video_index = parse_int(row.get("rgb_video_frame_index"), f"rgb_video_frame_index at row {row_number}")
            if video_index != rgb_count:
                raise ValidationError(
                    f"non-contiguous RGB video index at {frames_path}:{row_number}: "
                    f"expected {rgb_count}, got {video_index}"
                )
            global_us = parse_int(row.get("global_timestamp_us"), f"global_timestamp_us at row {row_number}")
            if last_global_us is not None and global_us <= last_global_us:
                raise ValidationError(
                    f"non-monotonic global_timestamp_us at {frames_path}:{row_number}: "
                    f"{global_us} <= {last_global_us}"
                )
            if first_global_us is None:
                first_global_us = global_us
            last_global_us = global_us
            rgb_count += 1

    if rgb_count == 0 or first_global_us is None or last_global_us is None:
        raise ValidationError(f"no finalized RGB frames found in {frames_path}")
    declared_frames = meta.get("rgb_frames")
    if declared_frames is not None and parse_int(declared_frames, "meta.rgb_frames") != rgb_count:
        raise ValidationError(
            f"RGB frame count mismatch for {root}: meta={declared_frames}, frames.csv={rgb_count}"
        )

    decoded_frames = ffprobe_frame_count(rgb_path, ffprobe) if verify_video_frames else None
    if decoded_frames is not None and decoded_frames != rgb_count:
        raise ValidationError(
            f"RGB frame count mismatch for {root}: MP4={decoded_frames}, frames.csv={rgb_count}"
        )

    return ValidationResult(
        root=str(root),
        camera_key=str(meta.get("camera_key") or root.name),
        frames_path=str(frames_path),
        rgb_path=str(rgb_path),
        rgb_frames=rgb_count,
        first_global_timestamp_us=first_global_us,
        last_global_timestamp_us=last_global_us,
        ready_marker=str(ready_marker) if ready_marker is not None else None,
        decoded_video_frames=decoded_frames,
    )


def fallback_yaml_inputs(path: Path) -> list[Path]:
    inputs: list[Path] = []
    in_inputs = False
    for raw_line in path.read_text(encoding="utf-8-sig").splitlines():
        if re.match(r"^inputs\s*:\s*$", raw_line):
            in_inputs = True
            continue
        if not in_inputs:
            continue
        match = re.match(r"^\s+-\s+(.+?)\s*$", raw_line)
        if not match:
            if raw_line.strip() and not raw_line.startswith((" ", "\t")):
                break
            continue
        value = match.group(1)
        if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
            value = value[1:-1]
        inputs.append(Path(value))
    return inputs


def config_inputs(path: Path) -> list[Path]:
    try:
        import yaml  # type: ignore
    except ImportError:
        return fallback_yaml_inputs(path)
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8-sig"))
    except Exception as exc:
        raise ValidationError(f"cannot parse sync config {path}: {exc}") from exc
    raw_inputs = value.get("inputs") if isinstance(value, dict) else None
    if not isinstance(raw_inputs, list):
        raise ValidationError(f"sync config has no inputs list: {path}")
    return [Path(str(item)) for item in raw_inputs]


def unique_paths(paths: Iterable[Path]) -> list[Path]:
    result: list[Path] = []
    seen: set[str] = set()
    for path in paths:
        key = str(path.resolve())
        if key not in seen:
            seen.add(key)
            result.append(path)
    return result


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate that RGB recordings are finalized and indexed before synchronization."
    )
    parser.add_argument("--config", type=Path, help="sync_multi_segments YAML config")
    parser.add_argument("--inputs", type=Path, nargs="*", default=[], help="recording directories")
    parser.add_argument("--verify-video-frames", action="store_true", help="compare ffprobe frame count with frames.csv")
    parser.add_argument("--ffprobe", default="ffprobe", help="ffprobe executable")
    parser.add_argument("--json", action="store_true", help="print JSON output")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    inputs = list(args.inputs)
    if args.config is not None:
        inputs.extend(config_inputs(args.config))
    inputs = unique_paths(inputs)
    if not inputs:
        raise ValidationError("at least one recording directory is required")
    results = [
        validate_recording(path, verify_video_frames=args.verify_video_frames, ffprobe=args.ffprobe)
        for path in inputs
    ]
    if args.json:
        print(json.dumps({"valid": True, "tracks": [asdict(item) for item in results]}, indent=2))
    else:
        for item in results:
            decoded = "" if item.decoded_video_frames is None else f" decoded={item.decoded_video_frames}"
            print(f"OK {item.camera_key} frames={item.rgb_frames}{decoded} root={item.root}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValidationError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2)
