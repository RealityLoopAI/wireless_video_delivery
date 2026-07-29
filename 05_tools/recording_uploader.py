#!/usr/bin/env python3
"""Capture staged recordings to NAS, then finalize and publish them atomically."""

from __future__ import annotations

import argparse
from concurrent.futures import Future, ThreadPoolExecutor, as_completed
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import threading
import time
import traceback
from typing import Any, Callable, Iterable
import urllib.request


STOP_REQUESTED = False
TRANSIENT_NAS_ERRNOS = frozenset(
    value
    for value in (
        errno.EAGAIN,
        errno.EBUSY,
        errno.ECONNABORTED,
        errno.ECONNRESET,
        errno.EFAULT,
        errno.EHOSTUNREACH,
        errno.EIO,
        errno.ENETRESET,
        errno.ENETUNREACH,
        errno.ESTALE,
        errno.ETIMEDOUT,
    )
    if value is not None
)
TRANSIENT_NAMES = {
    ".gwv3_uploader.lock",
    ".gwv3_publish_incomplete.json",
    "recording_staged.json",
    "recording_staged.json.tmp",
    "recording_capture_ready.json",
    "recording_capture_ready.json.tmp",
    "recording_capture_cached.json",
    "recording_capture_cached.json.tmp",
    "recording_nas_finalized.json",
    "recording_nas_finalized.json.tmp",
    "recording_ready.json.tmp",
    "recording_ready.json.pending",
    "recording_uploaded.json",
}
TRANSIENT_SUFFIXES = (
    "recording_staged.json",
    "recording_staged.json.tmp",
    "recording_capture_ready.json",
    "recording_capture_ready.json.tmp",
    "recording_capture_cached.json",
    "recording_capture_cached.json.tmp",
    "recording_nas_finalized.json",
    "recording_nas_finalized.json.tmp",
    "recording_ready.json",
    "recording_ready.json.tmp",
    "recording_ready.json.pending",
    "recording_uploaded.json",
)
CAPTURE_QUEUE_DIRECTORY = ".gwv3_capture_queue"
PUBLISH_JOURNAL_PREFIX = ".gwv3-publish-"
PUBLISH_JOURNAL_SUFFIX = ".json"
RGB_OUTPUT_CONVENTIONAL_MP4 = "conventional_mp4"
RGB_OUTPUT_FRAGMENTED_MP4 = "fragmented_mp4"
RGB_OUTPUT_MODES = frozenset(
    {
        RGB_OUTPUT_CONVENTIONAL_MP4,
        RGB_OUTPUT_FRAGMENTED_MP4,
    }
)


def request_stop(_signum: int, _frame: object) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True


def now_us() -> int:
    return time.time_ns() // 1000


def receiver_pause_phase(
    status: dict[str, Any],
    current_us: int,
    segment_seconds: int,
    quiet_before_segment_finalize_ms: int,
    max_record_queue_bytes: int = 0,
    max_record_queue_oldest_age_ms: int = 0,
) -> str:
    if int(status.get("record_finalize_outstanding_segments") or 0) > 0:
        return "receiver_finalize_wait"
    if max_record_queue_bytes > 0 and int(status.get("record_queue_total_bytes") or 0) >= max_record_queue_bytes:
        return "receiver_record_pressure_wait"
    if max_record_queue_oldest_age_ms > 0:
        for camera in status.get("cameras") or []:
            if (
                isinstance(camera, dict)
                and int(camera.get("record_queue_oldest_age_ms") or 0) >= max_record_queue_oldest_age_ms
            ):
                return "receiver_record_pressure_wait"
    quiet_us = max(0, quiet_before_segment_finalize_ms) * 1000
    segment_us = max(0, segment_seconds) * 1_000_000
    if quiet_us == 0 or segment_us == 0:
        return ""
    for camera in status.get("cameras") or []:
        if not isinstance(camera, dict):
            continue
        if not camera.get("recording") or not camera.get("segment_active"):
            continue
        segment_start_us = int(camera.get("segment_start_us") or 0)
        if segment_start_us <= 0:
            continue
        if camera.get("segment_rotation_requested") or segment_start_us + segment_us <= current_us + quiet_us:
            return "receiver_segment_boundary_wait"
    return ""


def transient_nas_errno(error: BaseException) -> int | None:
    current: BaseException | None = error
    seen: set[int] = set()
    while current is not None and id(current) not in seen:
        seen.add(id(current))
        if isinstance(current, OSError) and current.errno in TRANSIENT_NAS_ERRNOS:
            return current.errno
        current = current.__cause__ or current.__context__
    return None


def run_with_transient_nas_retries(
    operation: Callable[[], Any],
    on_retry: Callable[[int, int, BaseException, float], None] | None = None,
    max_attempts: int = 3,
    initial_delay_seconds: float = 1.0,
) -> Any:
    attempts = max(1, max_attempts)
    for attempt in range(1, attempts + 1):
        try:
            return operation()
        except Exception as error:
            if transient_nas_errno(error) is None or attempt >= attempts:
                raise
            delay = max(0.0, initial_delay_seconds) * (2 ** (attempt - 1))
            if on_retry is not None:
                on_retry(attempt, attempts, error, delay)
            deadline = time.monotonic() + delay
            while time.monotonic() < deadline:
                if STOP_REQUESTED:
                    raise InterruptedError("recording upload interrupted during retry wait") from error
                time.sleep(min(0.25, deadline - time.monotonic()))
    raise AssertionError("unreachable retry loop")


def wait_while_paused(
    pause: Callable[[], bool] | None,
    heartbeat: Callable[[], None] | None = None,
) -> float:
    if pause is None or not pause():
        return 0.0
    started = time.monotonic()
    while pause():
        if STOP_REQUESTED:
            raise InterruptedError("recording upload interrupted while waiting for receiver I/O")
        if heartbeat is not None:
            heartbeat()
        time.sleep(0.25)
    return time.monotonic() - started


def atomic_json_write(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=True, indent=2, sort_keys=True)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)
    fsync_directory(path.parent)


def fsync_directory(path: Path) -> None:
    try:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | os.O_CLOEXEC)
    except OSError:
        return
    try:
        os.fsync(descriptor)
    except OSError:
        pass
    finally:
        os.close(descriptor)


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise ValueError(f"JSON object required: {path}")
    return value


def marker_filename(value: Any, fallback: str, field: str) -> str:
    name = str(value or fallback)
    if not name or name in {".", ".."} or Path(name).name != name:
        raise ValueError(f"unsafe {field} in recording marker: {name!r}")
    return name


def run_checked(
    command: list[str],
    log_path: Path,
    timeout: float,
    heartbeat: Callable[[], None] | None = None,
    pause: Callable[[], bool] | None = None,
) -> subprocess.CompletedProcess[bytes]:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("ab", buffering=0) as log:
        process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
            close_fds=True,
        )
        deadline = time.monotonic() + timeout
        paused = False
        pause_started = 0.0
        while True:
            returncode = process.poll()
            if returncode is not None:
                return subprocess.CompletedProcess(command, returncode)
            if STOP_REQUESTED:
                if paused:
                    process.send_signal(signal.SIGCONT)
                    paused = False
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                raise InterruptedError("recording finalization interrupted")
            should_pause = pause is not None and pause()
            if should_pause and not paused:
                process.send_signal(signal.SIGSTOP)
                paused = True
                pause_started = time.monotonic()
            elif not should_pause and paused:
                process.send_signal(signal.SIGCONT)
                paused = False
                deadline += time.monotonic() - pause_started
            if not paused and time.monotonic() >= deadline:
                process.kill()
                process.wait()
                raise subprocess.TimeoutExpired(command, timeout)
            if heartbeat is not None:
                heartbeat()
            time.sleep(0.25)


def ffprobe_path(ffmpeg_path: str) -> str:
    executable = Path(ffmpeg_path)
    if executable.name == "ffmpeg":
        candidate = executable.with_name("ffprobe")
        if candidate.exists():
            return str(candidate)
    return "ffprobe"


def media_duration(
    path: Path,
    ffmpeg_path: str,
    pause: Callable[[], bool] | None = None,
) -> float | None:
    command = [
        ffprobe_path(ffmpeg_path),
        "-v",
        "error",
        "-show_entries",
        "format=duration",
        "-of",
        "default=nk=1:nw=1",
        str(path),
    ]
    process = subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        close_fds=True,
    )
    deadline = time.monotonic() + 60
    paused = False
    pause_started = 0.0
    while process.poll() is None:
        if STOP_REQUESTED:
            if paused:
                process.send_signal(signal.SIGCONT)
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            raise InterruptedError("recording media validation interrupted")
        should_pause = pause is not None and pause()
        if should_pause and not paused:
            process.send_signal(signal.SIGSTOP)
            paused = True
            pause_started = time.monotonic()
        elif not should_pause and paused:
            process.send_signal(signal.SIGCONT)
            paused = False
            deadline += time.monotonic() - pause_started
        if not paused and time.monotonic() >= deadline:
            process.kill()
            process.wait()
            raise subprocess.TimeoutExpired(command, 60)
        time.sleep(0.25)
    stdout, _stderr = process.communicate()
    result = subprocess.CompletedProcess(
        [
            *command,
        ],
        process.returncode,
        stdout=stdout,
    )
    if result.returncode != 0:
        return None
    try:
        duration = float(result.stdout.decode("ascii", errors="strict").strip())
    except (UnicodeDecodeError, ValueError):
        return None
    return duration if duration > 0 else None


def fragmented_mp4_has_mfra_footer(handle: Any, file_size: int) -> bool:
    if file_size < 24:
        return False
    handle.seek(file_size - 16)
    footer = handle.read(16)
    if (
        len(footer) != 16
        or int.from_bytes(footer[:4], "big") != 16
        or footer[4:8] != b"mfro"
    ):
        return False
    mfra_size = int.from_bytes(footer[12:16], "big")
    if mfra_size < 24 or mfra_size > file_size:
        return False
    handle.seek(file_size - mfra_size)
    header = handle.read(16)
    if len(header) < 8 or header[4:8] != b"mfra":
        return False
    atom_size = int.from_bytes(header[:4], "big")
    header_size = 8
    if atom_size == 1:
        if len(header) != 16:
            return False
        atom_size = int.from_bytes(header[8:16], "big")
        header_size = 16
    return atom_size >= header_size and atom_size == mfra_size


def mp4_atoms(path: Path) -> set[bytes] | None:
    try:
        file_size = path.stat().st_size
        atoms: set[bytes] = set()
        offset = 0
        with path.open("rb") as handle:
            while offset + 8 <= file_size:
                handle.seek(offset)
                header = handle.read(8)
                if len(header) != 8:
                    return None
                atom_size = int.from_bytes(header[:4], "big")
                atom_type = header[4:8]
                header_size = 8
                if atom_size == 1:
                    extended = handle.read(8)
                    if len(extended) != 8:
                        return None
                    atom_size = int.from_bytes(extended, "big")
                    header_size = 16
                elif atom_size == 0:
                    atom_size = file_size - offset
                if atom_size < header_size or atom_size > file_size - offset:
                    return None
                atoms.add(atom_type)
                offset += atom_size
                if atom_type == b"moof":
                    if fragmented_mp4_has_mfra_footer(handle, file_size):
                        atoms.add(b"mfra")
                    return atoms
        return atoms if offset == file_size else None
    except OSError:
        return None


def finalize_rgb(
    segment: Path,
    rgb_name: str,
    ffmpeg_path: str,
    expected_duration: float | None = None,
    heartbeat: Callable[[], None] | None = None,
    pause: Callable[[], bool] | None = None,
    source_rgb_path: Path | None = None,
    rgb_output_mode: str = RGB_OUTPUT_CONVENTIONAL_MP4,
) -> str:
    if rgb_output_mode not in RGB_OUTPUT_MODES:
        raise ValueError(f"unsupported RGB output mode: {rgb_output_mode}")
    rgb_path = segment / rgb_name
    if not rgb_path.is_file() or rgb_path.stat().st_size == 0:
        return "missing"
    atoms = mp4_atoms(rgb_path)
    if not atoms or b"moov" not in atoms:
        raise RuntimeError(f"RGB MP4 has no valid moov atom: {rgb_path}")
    if b"moof" not in atoms:
        source_duration = media_duration(rgb_path, ffmpeg_path, pause)
        if source_duration is None:
            raise RuntimeError(f"RGB MP4 is not readable: {rgb_path}")
        if (
            expected_duration is not None
            and expected_duration > 0
            and abs(source_duration - expected_duration) > max(1.0, expected_duration * 0.005)
        ):
            raise RuntimeError(f"RGB MP4 duration does not match recorded frame timing: {rgb_path}")
        return "already_seekable"
    if b"mfra" not in atoms:
        raise RuntimeError(f"RGB fragmented MP4 has no closing mfra atom: {rgb_path}")
    if rgb_output_mode == RGB_OUTPUT_FRAGMENTED_MP4:
        return "fragmented_passthrough"

    remux_source = rgb_path
    source_kind = "nas"
    if source_rgb_path is not None and source_rgb_path != rgb_path:
        try:
            source_atoms = mp4_atoms(source_rgb_path)
            source_valid = (
                source_rgb_path.is_file()
                and source_rgb_path.stat().st_size == rgb_path.stat().st_size
                and source_atoms is not None
                and b"moov" in source_atoms
                and (b"moof" not in source_atoms or b"mfra" in source_atoms)
            )
        except OSError:
            source_valid = False
        if source_valid:
            remux_source = source_rgb_path
            source_kind = "local"

    temporary = segment / ("." + rgb_name + ".seekable.tmp.mp4")
    temporary.unlink(missing_ok=True)
    result = run_checked(
        [
            ffmpeg_path,
            "-hide_banner",
            "-loglevel",
            "warning",
            "-y",
            "-i",
            str(remux_source),
            "-map",
            "0:v:0",
            "-c:v",
            "copy",
            str(temporary),
        ],
        segment / "ffmpeg.log",
        timeout=max(300.0, (expected_duration or 0.0) * 0.5),
        heartbeat=heartbeat,
        pause=pause,
    )
    if result.returncode != 0:
        temporary.unlink(missing_ok=True)
        raise RuntimeError(f"RGB player-compatible remux failed with exit={result.returncode}: {rgb_path}")
    wait_while_paused(pause, heartbeat)
    output_atoms = mp4_atoms(temporary)
    output_duration = media_duration(temporary, ffmpeg_path, pause)
    duration_valid = output_duration is not None
    if duration_valid and expected_duration is not None and expected_duration > 0:
        duration_valid = abs(output_duration - expected_duration) <= max(1.0, expected_duration * 0.005)
    if (
        not output_atoms
        or b"moov" not in output_atoms
        or b"moof" in output_atoms
        or not duration_valid
    ):
        temporary.unlink(missing_ok=True)
        raise RuntimeError(f"RGB remux validation failed: {rgb_path}")
    wait_while_paused(pause, heartbeat)
    with temporary.open("rb") as handle:
        os.fsync(handle.fileno())
    os.replace(temporary, rgb_path)
    fsync_directory(segment)
    return source_kind


def finalize_staged_segment(
    segment: Path,
    staged: dict[str, Any],
    staged_path: Path,
    ffmpeg_path: str,
    heartbeat: Callable[[], None] | None = None,
    pause: Callable[[], bool] | None = None,
    rgb_output_mode: str = RGB_OUTPUT_CONVENTIONAL_MP4,
) -> tuple[dict[str, Any], Path]:
    frames_name = marker_filename(staged.get("frames_file"), "frames.csv", "frames_file")
    rgb_name = marker_filename(staged.get("rgb_file"), "rgb.mp4", "rgb_file")
    depth_name = marker_filename(staged.get("depth_file"), "depth.mkv", "depth_file")
    meta_name = marker_filename(staged.get("meta_file"), "meta.json", "meta_file")
    if not (segment / frames_name).is_file():
        raise RuntimeError(f"final frames CSV missing: {segment / frames_name}")

    expected_rgb_duration: float | None = None
    try:
        meta = load_json(segment / meta_name)
        value = float(meta.get("rgb_container_expected_duration_sec") or 0)
        if value <= 0:
            frames = int(meta.get("rgb_frames") or 0)
            fps = float(meta.get("rgb_record_fps") or 0)
            value = frames / fps if frames > 0 and fps > 0 else 0
        if value <= 0:
            value = float(meta.get("rgb_target_duration_sec") or 0)
        if value > 0:
            expected_rgb_duration = value
    except (OSError, TypeError, ValueError, json.JSONDecodeError):
        pass
    finalize_rgb(
        segment,
        rgb_name,
        ffmpeg_path,
        expected_rgb_duration,
        heartbeat,
        pause,
        rgb_output_mode=rgb_output_mode,
    )
    rgb_exists = (segment / rgb_name).is_file() and (segment / rgb_name).stat().st_size > 0
    depth_exists = (segment / depth_name).is_file() and (segment / depth_name).stat().st_size > 0
    if not rgb_exists and not depth_exists:
        raise RuntimeError(f"no finalized media file found under {segment}")
    if depth_exists and media_duration(segment / depth_name, ffmpeg_path, pause) is None:
        raise RuntimeError(f"depth recording is not readable: {segment / depth_name}")
    rgb_atoms = mp4_atoms(segment / rgb_name) if rgb_exists else None
    rgb_container_format = (
        RGB_OUTPUT_FRAGMENTED_MP4
        if rgb_atoms is not None and b"moof" in rgb_atoms
        else RGB_OUTPUT_CONVENTIONAL_MP4 if rgb_exists else "missing"
    )

    ready_name = str(staged.get("ready_file") or "")
    if not ready_name:
        try:
            ready_name = str(load_json(segment / meta_name).get("recording_ready_file") or "")
        except (OSError, ValueError, json.JSONDecodeError):
            pass
    if not ready_name:
        ready_name = staged_path.name.replace("recording_staged.json", "recording_ready.json")
    ready_name = marker_filename(ready_name, "recording_ready.json", "ready_file")
    ready_path = segment / ready_name
    ready = {
        "schema": "gwv3_recording_ready_v1",
        "ready": True,
        "finalized_at_us": now_us(),
        "segment_start_us": int(staged.get("segment_start_us") or 0),
        "segment_end_us": int(staged.get("segment_end_us") or 0),
        "recording_session_id": int(staged.get("recording_session_id") or 0),
        "recording_window_start_global_us": int(staged.get("recording_window_start_global_us") or 0),
        "recording_window_end_global_us": int(staged.get("recording_window_end_global_us") or 0),
        "sender_id": str(staged.get("sender_id") or ""),
        "camera_id": str(staged.get("camera_id") or ""),
        "relative_path": str(staged.get("relative_path") or ""),
        "frames_file": frames_name,
        "meta_file": meta_name,
        "rgb_file": rgb_name,
        "depth_file": depth_name,
        "rgb_container_format": rgb_container_format,
        "rgb_fragmented": rgb_container_format == RGB_OUTPUT_FRAGMENTED_MP4,
        "rgb_frame_index_mode": "frames_csv_rgb_recorded_columns",
        "finalized_by": "gwv3_recording_uploader",
    }
    atomic_json_write(ready_path, ready)
    staged_path.unlink(missing_ok=True)
    fsync_directory(segment)
    return ready, ready_path


def capture_marker_name(ready_name: str) -> str:
    suffix = "recording_ready.json"
    if ready_name.endswith(suffix):
        return ready_name[: -len(suffix)] + "recording_capture_ready.json"
    return "recording_capture_ready.json"


def local_cache_marker_name(ready_name: str) -> str:
    suffix = "recording_ready.json"
    if ready_name.endswith(suffix):
        return ready_name[: -len(suffix)] + "recording_capture_cached.json"
    return "recording_capture_cached.json"


def nas_finalized_marker_name(ready_name: str) -> str:
    suffix = "recording_ready.json"
    if ready_name.endswith(suffix):
        return ready_name[: -len(suffix)] + "recording_nas_finalized.json"
    return "recording_nas_finalized.json"


def safe_relative_path(value: Any, fallback: Path) -> Path:
    relative = Path(str(value or fallback.as_posix()))
    if relative.is_absolute() or not relative.parts or any(part in {"", ".", ".."} for part in relative.parts):
        raise ValueError(f"unsafe staged relative path: {relative}")
    return relative


def has_matroska_header(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            return handle.read(4) == b"\x1a\x45\xdf\xa3"
    except OSError:
        return False


def expected_rgb_duration(segment: Path, meta_name: str) -> float | None:
    try:
        meta = load_json(segment / meta_name)
        value = float(meta.get("rgb_container_expected_duration_sec") or 0)
        if value <= 0:
            frames = int(meta.get("rgb_frames") or 0)
            fps = float(meta.get("rgb_record_fps") or 0)
            value = frames / fps if frames > 0 and fps > 0 else 0
        if value <= 0:
            value = float(meta.get("rgb_target_duration_sec") or 0)
        return value if value > 0 else None
    except (OSError, TypeError, ValueError, json.JSONDecodeError):
        return None


def build_ready_marker(
    source: dict[str, Any],
    rgb_container_format: str,
) -> dict[str, Any]:
    ready_name = marker_filename(source.get("ready_file"), "recording_ready.json", "ready_file")
    return {
        "schema": "gwv3_recording_ready_v1",
        "ready": True,
        "finalized_at_us": now_us(),
        "segment_start_us": int(source.get("segment_start_us") or 0),
        "segment_end_us": int(source.get("segment_end_us") or 0),
        "recording_session_id": int(source.get("recording_session_id") or 0),
        "recording_window_start_global_us": int(source.get("recording_window_start_global_us") or 0),
        "recording_window_end_global_us": int(source.get("recording_window_end_global_us") or 0),
        "sender_id": str(source.get("sender_id") or ""),
        "camera_id": str(source.get("camera_id") or ""),
        "relative_path": str(source.get("relative_path") or ""),
        "frames_file": marker_filename(source.get("frames_file"), "frames.csv", "frames_file"),
        "meta_file": marker_filename(source.get("meta_file"), "meta.json", "meta_file"),
        "rgb_file": marker_filename(source.get("rgb_file"), "rgb.mp4", "rgb_file"),
        "depth_file": marker_filename(source.get("depth_file"), "depth.mkv", "depth_file"),
        "ready_file": ready_name,
        "rgb_container_format": rgb_container_format,
        "rgb_fragmented": rgb_container_format == RGB_OUTPUT_FRAGMENTED_MP4,
        "rgb_frame_index_mode": "frames_csv_rgb_recorded_columns",
        "finalized_by": "gwv3_recording_uploader_nas_first",
    }


def prepare_local_capture(
    segment: Path,
    staging_root: Path,
) -> tuple[dict[str, Any], str]:
    staged_markers = sorted(segment.glob("*recording_staged.json"))
    ready_markers = sorted(segment.glob("*recording_ready.json"))
    if len(staged_markers) > 1 or len(ready_markers) > 1:
        raise RuntimeError(f"ambiguous recording marker set: {segment}")
    if ready_markers:
        source_path = ready_markers[0]
        source = load_json(source_path)
        if source.get("ready") is not True:
            raise RuntimeError(f"invalid ready marker: {source_path}")
        ready_name = source_path.name
    elif staged_markers:
        source_path = staged_markers[0]
        source = load_json(source_path)
        if source.get("staged") is not True:
            raise RuntimeError(f"invalid staged marker: {source_path}")
        ready_name = str(source.get("ready_file") or "")
        if not ready_name:
            meta_name = marker_filename(source.get("meta_file"), "meta.json", "meta_file")
            try:
                ready_name = str(load_json(segment / meta_name).get("recording_ready_file") or "")
            except (OSError, ValueError, json.JSONDecodeError):
                pass
        if not ready_name:
            ready_name = source_path.name.replace("recording_staged.json", "recording_ready.json")
    else:
        raise RuntimeError(f"recording control marker disappeared: {segment}")

    ready_name = marker_filename(ready_name, "recording_ready.json", "ready_file")
    relative = safe_relative_path(source.get("relative_path"), segment.relative_to(staging_root))
    frames_name = marker_filename(source.get("frames_file"), "frames.csv", "frames_file")
    rgb_name = marker_filename(source.get("rgb_file"), "rgb.mp4", "rgb_file")
    depth_name = marker_filename(source.get("depth_file"), "depth.mkv", "depth_file")
    meta_name = marker_filename(source.get("meta_file"), "meta.json", "meta_file")

    if not (segment / frames_name).is_file():
        raise RuntimeError(f"final frames CSV missing: {segment / frames_name}")
    if not (segment / meta_name).is_file():
        raise RuntimeError(f"recording metadata missing: {segment / meta_name}")
    try:
        if load_json(segment / meta_name).get("closed") is not True:
            raise RuntimeError(f"recording metadata is not closed: {segment / meta_name}")
    except (OSError, ValueError, json.JSONDecodeError) as error:
        raise RuntimeError(f"recording metadata is unreadable: {segment / meta_name}") from error

    rgb_path = segment / rgb_name
    depth_path = segment / depth_name
    rgb_exists = rgb_path.is_file() and rgb_path.stat().st_size > 0
    depth_exists = depth_path.is_file() and depth_path.stat().st_size > 0
    if not rgb_exists and not depth_exists:
        raise RuntimeError(f"no finalized media file found under {segment}")
    if rgb_exists:
        rgb_atoms = mp4_atoms(rgb_path)
        if not rgb_atoms or b"moov" not in rgb_atoms:
            raise RuntimeError(f"RGB MP4 has no valid moov atom: {rgb_path}")
        if b"moof" in rgb_atoms and b"mfra" not in rgb_atoms:
            raise RuntimeError(f"RGB fragmented MP4 has no closing mfra atom: {rgb_path}")
    if depth_exists and not has_matroska_header(depth_path):
        raise RuntimeError(f"depth recording has no Matroska header: {depth_path}")

    capture = {
        "schema": "gwv3_recording_capture_ready_v1",
        "capture_ready": True,
        "captured_at_us": 0,
        "segment_start_us": int(source.get("segment_start_us") or 0),
        "segment_end_us": int(source.get("segment_end_us") or 0),
        "recording_session_id": int(source.get("recording_session_id") or 0),
        "recording_window_start_global_us": int(source.get("recording_window_start_global_us") or 0),
        "recording_window_end_global_us": int(source.get("recording_window_end_global_us") or 0),
        "sender_id": str(source.get("sender_id") or ""),
        "camera_id": str(source.get("camera_id") or ""),
        "relative_path": relative.as_posix(),
        "frames_file": frames_name,
        "meta_file": meta_name,
        "rgb_file": rgb_name,
        "depth_file": depth_name,
        "ready_file": ready_name,
        "capture_file": capture_marker_name(ready_name),
        "rgb_frame_index_mode": "frames_csv_rgb_recorded_columns",
    }
    return capture, capture["capture_file"]


def finalize_captured_segment(
    segment: Path,
    capture: dict[str, Any],
    ffmpeg_path: str,
    heartbeat: Callable[[], None] | None = None,
    pause: Callable[[], bool] | None = None,
    local_segment: Path | None = None,
    rgb_output_mode: str = RGB_OUTPUT_CONVENTIONAL_MP4,
) -> tuple[dict[str, Any], Path, str]:
    frames_name = marker_filename(capture.get("frames_file"), "frames.csv", "frames_file")
    rgb_name = marker_filename(capture.get("rgb_file"), "rgb.mp4", "rgb_file")
    depth_name = marker_filename(capture.get("depth_file"), "depth.mkv", "depth_file")
    meta_name = marker_filename(capture.get("meta_file"), "meta.json", "meta_file")
    ready_name = marker_filename(capture.get("ready_file"), "recording_ready.json", "ready_file")
    if not (segment / frames_name).is_file():
        raise RuntimeError(f"final frames CSV missing from NAS capture: {segment / frames_name}")

    remux_source = finalize_rgb(
        segment,
        rgb_name,
        ffmpeg_path,
        expected_rgb_duration(segment, meta_name),
        heartbeat,
        pause,
        source_rgb_path=(local_segment / rgb_name) if local_segment is not None else None,
        rgb_output_mode=rgb_output_mode,
    )
    rgb_exists = (segment / rgb_name).is_file() and (segment / rgb_name).stat().st_size > 0
    depth_exists = (segment / depth_name).is_file() and (segment / depth_name).stat().st_size > 0
    if not rgb_exists and not depth_exists:
        raise RuntimeError(f"no finalized media file found under NAS capture {segment}")
    if depth_exists and media_duration(segment / depth_name, ffmpeg_path, pause) is None:
        raise RuntimeError(f"depth recording is not readable: {segment / depth_name}")
    rgb_atoms = mp4_atoms(segment / rgb_name) if rgb_exists else None
    rgb_container_format = (
        RGB_OUTPUT_FRAGMENTED_MP4
        if rgb_atoms is not None and b"moof" in rgb_atoms
        else RGB_OUTPUT_CONVENTIONAL_MP4 if rgb_exists else "missing"
    )

    ready = build_ready_marker(capture, rgb_container_format)
    finalized_path = segment / nas_finalized_marker_name(ready_name)
    atomic_json_write(
        finalized_path,
        {
            "schema": "gwv3_recording_nas_finalized_v1",
            "nas_finalized": True,
            "finalized_at_us": now_us(),
            "capture_file": marker_filename(
                capture.get("capture_file"),
                capture_marker_name(ready_name),
                "capture_file",
            ),
            "ready_file": ready_name,
            "ready_marker": ready,
        },
    )
    return ready, finalized_path, remux_source


def is_transient_recording_name(name: str) -> bool:
    return (
        name in TRANSIENT_NAMES
        or name.startswith(".rgb.mp4.seekable.tmp")
        or name.endswith(TRANSIENT_SUFFIXES)
    )


def iter_copy_files(source: Path) -> Iterable[tuple[Path, Path]]:
    for root, directories, files in os.walk(source):
        directories[:] = sorted(name for name in directories if not name.startswith(".gwv3-uploading-"))
        root_path = Path(root)
        relative_root = root_path.relative_to(source)
        for name in sorted(files):
            if is_transient_recording_name(name):
                continue
            yield root_path / name, relative_root / name


def copy_file_limited(
    source: Path,
    destination: Path,
    bandwidth_mbps: float,
    progress: Callable[[int, int], None] | None = None,
    pause: Callable[[], bool] | None = None,
) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    copied = 0
    total = source.stat().st_size
    if progress is not None:
        progress(0, total)
    started = time.monotonic()

    def wait_for_receiver_io() -> None:
        nonlocal started
        started += wait_while_paused(
            pause,
            (lambda: progress(copied, total)) if progress is not None else None,
        )

    chunk_size = 4 * 1024 * 1024
    with source.open("rb") as input_handle, destination.open("wb") as output_handle:
        while True:
            wait_for_receiver_io()
            chunk = input_handle.read(chunk_size)
            if not chunk:
                break
            output_handle.write(chunk)
            copied += len(chunk)
            if progress is not None:
                progress(copied, total)
            if bandwidth_mbps > 0:
                while True:
                    wait_for_receiver_io()
                    if STOP_REQUESTED:
                        raise InterruptedError("recording upload interrupted")
                    expected = copied * 8.0 / (bandwidth_mbps * 1_000_000.0)
                    delay = expected - (time.monotonic() - started)
                    if delay <= 0:
                        break
                    if progress is not None:
                        progress(copied, total)
                    time.sleep(min(delay, 0.25))
        wait_for_receiver_io()
        output_handle.flush()
        os.fsync(output_handle.fileno())
    try:
        shutil.copystat(source, destination, follow_symlinks=False)
    except OSError as error:
        # CIFS mounts may reject chmod/utime metadata updates even after the
        # file contents have been written and fsynced successfully.
        print(
            f"recording upload metadata preservation skipped source={source} "
            f"destination={destination} error={error}",
            file=sys.stderr,
            flush=True,
        )
    if progress is not None:
        progress(total, total)


def manifest(root: Path) -> dict[str, int]:
    result: dict[str, int] = {}
    for source, relative in iter_copy_files(root):
        if source.is_symlink() or not source.is_file():
            raise RuntimeError(f"unsupported staged path type: {source}")
        result[relative.as_posix()] = source.stat().st_size
    return result


def ready_identity(marker: dict[str, Any]) -> tuple[str, str, int]:
    return (
        str(marker.get("sender_id") or ""),
        str(marker.get("camera_id") or ""),
        int(marker.get("segment_start_us") or 0),
    )


def capture_queue_key(capture: dict[str, Any]) -> str:
    identity = (
        str(capture.get("sender_id") or ""),
        str(capture.get("camera_id") or ""),
        str(int(capture.get("segment_start_us") or 0)),
        str(capture.get("relative_path") or ""),
    )
    return hashlib.sha256("\0".join(identity).encode("utf-8")).hexdigest()[:24]


def publish_capture_segment(
    source: Path,
    capture_queue_root: Path,
    capture: dict[str, Any],
    capture_name: str,
    bandwidth_mbps: float,
    progress: Callable[[int, int], None] | None = None,
    pause: Callable[[], bool] | None = None,
) -> tuple[Path, bool]:
    capture_name = marker_filename(capture_name, "recording_capture_ready.json", "capture_file")
    key = capture_queue_key(capture)
    destination = capture_queue_root / key
    source_manifest = manifest(source)
    total_bytes = sum(source_manifest.values())
    if destination.exists():
        existing_marker = destination / capture_name
        try:
            existing = load_json(existing_marker)
            if (
                existing.get("capture_ready") is True
                and ready_identity(existing) == ready_identity(capture)
                and str(existing.get("relative_path") or "") == str(capture.get("relative_path") or "")
                and manifest(destination) == source_manifest
            ):
                if progress is not None:
                    progress(total_bytes, total_bytes)
                return destination, True
        except (OSError, ValueError, RuntimeError, json.JSONDecodeError):
            pass
        raise RuntimeError(f"NAS capture queue identity collision: {destination}")

    capture_queue_root.mkdir(parents=True, exist_ok=True)
    temporary_prefix = f".gwv3-uploading-{key}-"
    for stale_temporary in capture_queue_root.glob(temporary_prefix + "*"):
        if stale_temporary.is_dir():
            shutil.rmtree(stale_temporary, ignore_errors=True)
    temporary = capture_queue_root / f"{temporary_prefix}{os.getpid()}"
    temporary.mkdir(parents=False)
    try:
        copied_before_file = 0
        if progress is not None:
            progress(0, total_bytes)
        for relative_text in sorted(source_manifest):
            relative = Path(relative_text)
            file_size = source_manifest[relative_text]
            copy_file_limited(
                source / relative,
                temporary / relative,
                bandwidth_mbps,
                (
                    lambda copied, _total, base=copied_before_file: progress(base + copied, total_bytes)
                    if progress is not None
                    else None
                ),
                pause,
            )
            copied_before_file += file_size
        for root, directories, _files in os.walk(temporary, topdown=False):
            for directory in directories:
                fsync_directory(Path(root) / directory)
            fsync_directory(Path(root))
        if manifest(temporary) != source_manifest:
            raise RuntimeError(f"NAS capture copy verification failed: {temporary}")
        published_capture = dict(capture)
        published_capture["captured_at_us"] = now_us()
        atomic_json_write(temporary / capture_name, published_capture)
        os.replace(temporary, destination)
        fsync_directory(capture_queue_root)
        if (
            load_json(destination / capture_name).get("capture_ready") is not True
            or manifest(destination) != source_manifest
        ):
            raise RuntimeError(f"published NAS capture verification failed: {destination}")
        if progress is not None:
            progress(total_bytes, total_bytes)
        return destination, False
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def write_local_capture_cache_marker(
    local_segment: Path,
    capture_segment: Path,
    capture: dict[str, Any],
) -> Path:
    ready_name = marker_filename(capture.get("ready_file"), "recording_ready.json", "ready_file")
    marker_path = local_segment / local_cache_marker_name(ready_name)
    atomic_json_write(
        marker_path,
        {
            "schema": "gwv3_recording_local_capture_cache_v1",
            "cached": True,
            "cached_at_us": now_us(),
            "capture_directory": capture_segment.name,
            "capture_key": capture_queue_key(capture),
            "segment_start_us": int(capture.get("segment_start_us") or 0),
            "sender_id": str(capture.get("sender_id") or ""),
            "camera_id": str(capture.get("camera_id") or ""),
            "relative_path": str(capture.get("relative_path") or ""),
            "ready_file": ready_name,
            "rgb_file": marker_filename(capture.get("rgb_file"), "rgb.mp4", "rgb_file"),
        },
    )
    return marker_path


def local_capture_cache_for(
    capture: dict[str, Any],
    staging_root: Path,
    capture_segment: Path | None = None,
) -> Path | None:
    try:
        relative = safe_relative_path(capture.get("relative_path"), Path("capture"))
        local_segment = staging_root / relative
        ready_name = marker_filename(capture.get("ready_file"), "recording_ready.json", "ready_file")
        marker_path = local_segment / local_cache_marker_name(ready_name)
        marker = load_json(marker_path)
        if (
            marker.get("schema") != "gwv3_recording_local_capture_cache_v1"
            or marker.get("cached") is not True
            or ready_identity(marker) != ready_identity(capture)
            or str(marker.get("relative_path") or "") != relative.as_posix()
            or str(marker.get("capture_key") or "") != capture_queue_key(capture)
        ):
            return None
        if capture_segment is not None and str(marker.get("capture_directory") or "") != capture_segment.name:
            return None
        rgb_name = marker_filename(capture.get("rgb_file"), "rgb.mp4", "rgb_file")
        rgb_path = local_segment / rgb_name
        return local_segment if rgb_path.is_file() and rgb_path.stat().st_size > 0 else None
    except (OSError, TypeError, ValueError, json.JSONDecodeError):
        return None


def final_destination_for(
    capture_segment: Path,
    nas_root: Path,
    capture: dict[str, Any],
    ready: dict[str, Any],
    ready_name: str,
) -> tuple[Path, bool]:
    relative = safe_relative_path(capture.get("relative_path"), Path(capture_segment.name))
    if relative.parts[0] == capture_segment.parent.name:
        raise RuntimeError(f"capture destination cannot point into capture queue: {relative}")
    destination = nas_root / relative
    if not destination.exists():
        return destination, False
    existing_marker = destination / ready_name
    if existing_marker.is_file():
        try:
            if (
                ready_identity(load_json(existing_marker)) == ready_identity(ready)
                and manifest(destination) == manifest(capture_segment)
            ):
                return destination, True
        except (OSError, ValueError, RuntimeError, json.JSONDecodeError):
            pass
    for suffix in range(1, 1000):
        candidate = destination.with_name(f"{destination.name}_recovered_{suffix:03d}")
        if not candidate.exists():
            return candidate, False
    raise RuntimeError(f"cannot allocate non-colliding NAS destination for {destination}")


def publish_journal_path(capture_segment: Path) -> Path:
    capture_directory = marker_filename(
        capture_segment.name,
        "capture",
        "capture_directory",
    )
    return capture_segment.parent / (
        PUBLISH_JOURNAL_PREFIX + capture_directory + PUBLISH_JOURNAL_SUFFIX
    )


def discover_publish_journals(capture_queue_root: Path) -> list[Path]:
    try:
        if not capture_queue_root.is_dir():
            return []
        return sorted(
            path
            for path in capture_queue_root.glob(
                PUBLISH_JOURNAL_PREFIX + "*" + PUBLISH_JOURNAL_SUFFIX
            )
            if path.is_file()
        )
    except OSError:
        return []


def recover_publish_journal(
    journal_path: Path,
    nas_root: Path,
    capture_queue_root: Path,
) -> Path | None:
    if journal_path.parent != capture_queue_root:
        raise RuntimeError(f"publish journal is outside capture queue: {journal_path}")
    journal = load_json(journal_path)
    if journal.get("schema") != "gwv3_recording_publish_journal_v1":
        raise RuntimeError(f"unsupported recording publish journal: {journal_path}")

    capture_directory = marker_filename(
        journal.get("capture_directory"),
        "capture",
        "capture_directory",
    )
    destination_relative = safe_relative_path(
        journal.get("destination_relative_path"),
        Path(capture_directory),
    )
    if destination_relative.parts[0] == capture_queue_root.name:
        raise RuntimeError(
            f"publish journal destination points into capture queue: {destination_relative}"
        )
    ready_name = marker_filename(
        journal.get("ready_file"),
        "recording_ready.json",
        "ready_file",
    )
    pending_name = marker_filename(
        journal.get("pending_ready_file"),
        ready_name + ".pending",
        "pending_ready_file",
    )
    capture_name = marker_filename(
        journal.get("capture_file"),
        capture_marker_name(ready_name),
        "capture_file",
    )
    finalized_name = marker_filename(
        journal.get("nas_finalized_file"),
        nas_finalized_marker_name(ready_name),
        "nas_finalized_file",
    )
    ready = journal.get("ready_marker")
    if not isinstance(ready, dict) or ready.get("ready") is not True:
        raise RuntimeError(f"publish journal has no valid ready marker: {journal_path}")

    capture_segment = capture_queue_root / capture_directory
    destination = nas_root / destination_relative
    if capture_segment.exists():
        return None
    if not destination.is_dir():
        raise RuntimeError(
            f"publish journal references neither capture nor destination: {journal_path}"
        )

    ready_path = destination / ready_name
    pending_path = destination / pending_name
    incomplete_path = destination / ".gwv3_publish_incomplete.json"
    if ready_path.is_file():
        published_ready = load_json(ready_path)
        if (
            published_ready.get("ready") is not True
            or ready_identity(published_ready) != ready_identity(ready)
        ):
            raise RuntimeError(f"published ready marker identity mismatch: {ready_path}")
    else:
        if not pending_path.is_file():
            raise RuntimeError(f"pending ready marker missing during publish recovery: {pending_path}")
        pending_ready = load_json(pending_path)
        if (
            pending_ready.get("ready") is not True
            or ready_identity(pending_ready) != ready_identity(ready)
        ):
            raise RuntimeError(f"pending ready marker identity mismatch: {pending_path}")
        if not incomplete_path.is_file():
            raise RuntimeError(f"incomplete publish marker missing: {incomplete_path}")
        incomplete = load_json(incomplete_path)
        if ready_identity(incomplete) != ready_identity(ready):
            raise RuntimeError(f"incomplete publish marker identity mismatch: {incomplete_path}")

    for internal_marker in (
        destination / capture_name,
        destination / finalized_name,
        destination / ".gwv3_uploader.lock",
        incomplete_path,
    ):
        internal_marker.unlink(missing_ok=True)
    fsync_directory(destination)
    if not ready_path.is_file():
        os.replace(pending_path, ready_path)
        fsync_directory(destination)
    else:
        pending_path.unlink(missing_ok=True)
        fsync_directory(destination)
    journal_path.unlink(missing_ok=True)
    fsync_directory(capture_queue_root)
    return destination


def publish_finalized_capture(
    capture_segment: Path,
    nas_root: Path,
    capture: dict[str, Any],
    ready: dict[str, Any],
    finalized_path: Path,
) -> Path:
    ready_name = marker_filename(
        ready.get("ready_file") or capture.get("ready_file"),
        "recording_ready.json",
        "ready_file",
    )
    finalized_name = nas_finalized_marker_name(ready_name)
    if (
        finalized_path.parent != capture_segment
        or finalized_path.name != finalized_name
        or not finalized_path.is_file()
        or load_json(finalized_path).get("nas_finalized") is not True
    ):
        raise RuntimeError(
            f"NAS-finalized marker missing before final publication: {finalized_path}"
        )
    journal_path = publish_journal_path(capture_segment)
    if journal_path.is_file():
        recovered = recover_publish_journal(
            journal_path,
            nas_root,
            capture_segment.parent,
        )
        if recovered is not None:
            return recovered

    destination, already_published = final_destination_for(
        capture_segment,
        nas_root,
        capture,
        ready,
        ready_name,
    )
    if already_published:
        shutil.rmtree(capture_segment)
        fsync_directory(capture_segment.parent)
        journal_path.unlink(missing_ok=True)
        fsync_directory(journal_path.parent)
        return destination

    destination.parent.mkdir(parents=True, exist_ok=True)
    capture_name = marker_filename(
        capture.get("capture_file"),
        capture_marker_name(ready_name),
        "capture_file",
    )
    pending_name = ready_name + ".pending"
    journal = {
        "schema": "gwv3_recording_publish_journal_v1",
        "created_at_us": now_us(),
        "capture_directory": capture_segment.name,
        "destination_relative_path": destination.relative_to(nas_root).as_posix(),
        "capture_file": capture_name,
        "nas_finalized_file": finalized_name,
        "ready_file": ready_name,
        "pending_ready_file": pending_name,
        "ready_marker": ready,
    }
    atomic_json_write(journal_path, journal)
    atomic_json_write(capture_segment / pending_name, ready)
    incomplete = dict(ready)
    incomplete.update(
        {
            "schema": "gwv3_recording_publish_incomplete_v1",
            "publish_incomplete": True,
            "destination_relative_path": destination.relative_to(nas_root).as_posix(),
        }
    )
    atomic_json_write(capture_segment / ".gwv3_publish_incomplete.json", incomplete)
    # The capture lock lives in the queue root. Remove markers left by older
    # versions before rename because CIFS rejects moving a directory that
    # contains an open file.
    (capture_segment / ".gwv3_uploader.lock").unlink(missing_ok=True)
    fsync_directory(capture_segment)
    os.replace(capture_segment, destination)
    fsync_directory(destination.parent)
    recovered = recover_publish_journal(
        journal_path,
        nas_root,
        capture_segment.parent,
    )
    if recovered is None:
        raise RuntimeError(f"final NAS recording publication did not complete: {destination}")
    return recovered


def acquire_file_lock(lock_path: Path) -> int | None:
    flags = os.O_RDWR | os.O_CREAT | os.O_CLOEXEC
    try:
        descriptor = os.open(lock_path, flags, 0o600)
    except OSError:
        return None
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(descriptor)
        return None
    except OSError:
        os.close(descriptor)
        return None
    os.ftruncate(descriptor, 0)
    os.write(descriptor, f"pid={os.getpid()} started_us={now_us()}\n".encode("ascii"))
    os.fsync(descriptor)
    return descriptor


def release_file_lock(lock_path: Path, descriptor: int | None) -> None:
    if descriptor is not None:
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)
        try:
            lock_path.unlink(missing_ok=True)
        except OSError:
            pass


def local_segment_lock_path(segment: Path) -> Path:
    return segment / ".gwv3_uploader.lock"


def capture_segment_lock_path(segment: Path) -> Path:
    name = marker_filename(segment.name, "capture", "capture_directory")
    return segment.parent / f".gwv3-lock-{name}"


def discover_local_segments(staging_root: Path) -> list[Path]:
    candidates: dict[str, Path] = {}
    if not staging_root.exists():
        return []
    for marker_pattern in ("*recording_staged.json", "*recording_ready.json"):
        for marker in staging_root.rglob(marker_pattern):
            if marker.parent == staging_root or ".gwv3-uploading-" in marker.as_posix():
                continue
            if (
                (marker.parent / "recording_uploaded.json").is_file()
                or list(marker.parent.glob("*recording_capture_cached.json"))
            ):
                continue
            candidates[str(marker.parent)] = marker.parent
    return sorted(candidates.values(), key=lambda path: path.stat().st_mtime)


def discover_local_capture_caches(staging_root: Path) -> list[Path]:
    candidates: dict[str, Path] = {}
    if not staging_root.exists():
        return []
    for marker in staging_root.rglob("*recording_capture_cached.json"):
        if marker.parent == staging_root or ".gwv3-uploading-" in marker.as_posix():
            continue
        candidates[str(marker.parent)] = marker.parent
    return sorted(candidates.values(), key=lambda path: path.stat().st_mtime)


def discover_capture_segments(capture_queue_root: Path) -> list[Path]:
    try:
        if not capture_queue_root.is_dir():
            return []
        candidates = []
        for segment in capture_queue_root.iterdir():
            if not segment.is_dir() or segment.name.startswith(".gwv3-uploading-"):
                continue
            if (
                list(segment.glob("*recording_capture_ready.json"))
                or list(segment.glob("*recording_nas_finalized.json"))
            ):
                candidates.append(segment)
        return sorted(candidates, key=lambda path: path.stat().st_mtime)
    except OSError:
        return []


def load_captured_segment_state(
    segment: Path,
) -> tuple[dict[str, Any], dict[str, Any] | None, Path | None]:
    capture_markers = sorted(segment.glob("*recording_capture_ready.json"))
    finalized_markers = sorted(segment.glob("*recording_nas_finalized.json"))
    if len(capture_markers) > 1 or len(finalized_markers) > 1:
        raise RuntimeError(f"ambiguous NAS capture marker set: {segment}")
    if capture_markers:
        capture = load_json(capture_markers[0])
        if capture.get("capture_ready") is not True:
            raise RuntimeError(f"invalid NAS capture marker: {capture_markers[0]}")
    elif finalized_markers:
        finalized = load_json(finalized_markers[0])
        ready_value = finalized.get("ready_marker")
        if not isinstance(ready_value, dict):
            raise RuntimeError(
                f"NAS-finalized marker has no ready payload: {finalized_markers[0]}"
            )
        capture = dict(ready_value)
        capture["capture_file"] = str(finalized.get("capture_file") or "")
    else:
        raise RuntimeError(f"NAS capture marker disappeared: {segment}")

    if not finalized_markers:
        return capture, None, None
    finalized_path = finalized_markers[0]
    finalized = load_json(finalized_path)
    ready = finalized.get("ready_marker")
    if finalized.get("nas_finalized") is not True or not isinstance(ready, dict):
        raise RuntimeError(f"invalid NAS-finalized marker: {finalized_path}")
    if ready.get("ready") is not True:
        raise RuntimeError(f"invalid ready payload in NAS capture: {finalized_path}")
    if ready_identity(ready) != ready_identity(capture):
        raise RuntimeError(f"NAS capture/finalized marker identity mismatch: {segment}")
    return capture, ready, finalized_path


def discover_segments(staging_root: Path) -> list[Path]:
    return discover_local_segments(staging_root)


def pending_metrics(segments: list[Path]) -> tuple[int, int, int]:
    total_bytes = 0
    oldest_us = 0
    current_us = now_us()
    for segment in segments:
        try:
            modified_us = segment.stat().st_mtime_ns // 1000
            oldest_us = max(oldest_us, max(0, current_us - modified_us))
            for source, _relative in iter_copy_files(segment):
                if source.is_file():
                    total_bytes += source.stat().st_size
        except OSError:
            continue
    return len(segments), total_bytes, oldest_us


def publish_recovery_pending_count(
    journals: list[Path],
    capture_queue_root: Path,
) -> int:
    pending = 0
    for journal_path in journals:
        try:
            journal = load_json(journal_path)
            capture_directory = marker_filename(
                journal.get("capture_directory"),
                "capture",
                "capture_directory",
            )
            if not (capture_queue_root / capture_directory).exists():
                pending += 1
        except (OSError, TypeError, ValueError, json.JSONDecodeError):
            pending += 1
    return pending


class Uploader:
    def __init__(self, config_path: Path):
        config = load_json(config_path)
        staging = config.get("recording_staging") or {}
        if not isinstance(staging, dict):
            raise ValueError("recording_staging must be an object")
        self.enabled = bool(staging.get("enabled", False))
        staging_root_text = str(staging.get("root") or "")
        nas_root_text = str(config.get("nas_root") or "")
        self.staging_root = Path(staging_root_text).expanduser()
        self.nas_root = Path(nas_root_text).expanduser()
        capture_queue_name = str(staging.get("nas_capture_queue_directory") or CAPTURE_QUEUE_DIRECTORY)
        if Path(capture_queue_name).name != capture_queue_name or capture_queue_name in {"", ".", ".."}:
            raise ValueError("nas_capture_queue_directory must be one directory name")
        self.capture_queue_root = self.nas_root / capture_queue_name
        self.ffmpeg_path = str(config.get("ffmpeg_path") or "ffmpeg")
        self.interval = max(0.25, int(staging.get("upload_interval_ms", 2000)) / 1000.0)
        self.bandwidth_mbps = max(0.0, float(staging.get("upload_bandwidth_limit_mbps", 0)))
        self.delete_after_upload = bool(staging.get("delete_after_upload", True))
        self.retain_local_capture_for_finalize = bool(
            staging.get("retain_local_capture_for_finalize", True)
        )
        self.rgb_output_mode = str(
            staging.get("rgb_output_mode") or RGB_OUTPUT_CONVENTIONAL_MP4
        )
        if self.rgb_output_mode not in RGB_OUTPUT_MODES:
            raise ValueError(
                "recording_staging.rgb_output_mode must be "
                f"one of {sorted(RGB_OUTPUT_MODES)}"
            )
        self.local_cache_high_watermark_percent = min(
            100.0,
            max(0.0, float(staging.get("local_cache_high_watermark_percent", 75))),
        )
        self.local_cache_low_watermark_percent = min(
            self.local_cache_high_watermark_percent,
            max(0.0, float(staging.get("local_cache_low_watermark_percent", 70))),
        )
        self.finalize_workers = min(
            4,
            max(1, int(staging.get("finalize_workers", 2))),
        )
        self.status_write_interval = max(
            0.25,
            int(staging.get("status_write_interval_ms", 2000)) / 1000.0,
        )
        self.pause_during_receiver_finalize = bool(staging.get("pause_during_receiver_finalize", True))
        self.quiet_before_segment_finalize_ms = max(
            0, int(staging.get("quiet_before_segment_finalize_ms", 60000))
        )
        self.pause_record_queue_bytes = max(
            0, int(staging.get("pause_record_queue_bytes", 8 * 1024 * 1024))
        )
        self.pause_record_queue_oldest_age_ms = max(
            0, int(staging.get("pause_record_queue_oldest_age_ms", 500))
        )
        self.segment_seconds = max(0, int(config.get("segment_seconds", 300)))
        self.receiver_admin_url = f"http://127.0.0.1:{int(config.get('admin_port', 18080))}/api/status"
        self.status_path = self.staging_root / ".gwv3_uploader_status.json"
        self.captured = 0
        self.completed = 0
        self.failures = 0
        self.last_error = ""
        self.last_capture_success_us = 0
        self.last_success_us = 0
        self.active_segment = ""
        self.active_phase = ""
        self.active_bytes_done = 0
        self.active_bytes_total = 0
        self.active_started_us = 0
        self.next_active_status_at = 0.0
        self.next_receiver_status_at = 0.0
        self.receiver_pause_phase = ""
        self.running = False
        self.local_remuxes = 0
        self.nas_fallback_remuxes = 0
        self.fragmented_passthroughs = 0
        self.local_cache_pressure_releases = 0
        self.last_capture_duration_ms = 0
        self.last_finalize_duration_ms = 0
        self.last_publish_duration_ms = 0
        self.last_remux_source = ""
        self.status_lock = threading.RLock()
        self.receiver_status_lock = threading.Lock()
        self.cached_pending_status: dict[str, int] | None = None

        if not self.enabled:
            return
        if not staging_root_text or not nas_root_text:
            raise ValueError("recording staging root and nas_root are required")
        if self.staging_root.resolve() == self.nas_root.resolve():
            raise ValueError("recording staging root must differ from nas_root")
        self.staging_root.mkdir(parents=True, exist_ok=True)

    def write_status(self, refresh_metrics: bool = False) -> None:
        with self.status_lock:
            if refresh_metrics or self.cached_pending_status is None:
                local_segments = discover_local_segments(self.staging_root)
                local_cache_segments = discover_local_capture_caches(self.staging_root)
                capture_segments = discover_capture_segments(self.capture_queue_root)
                publish_journals = discover_publish_journals(self.capture_queue_root)
                publish_recovery_count = publish_recovery_pending_count(
                    publish_journals,
                    self.capture_queue_root,
                )
                local_count, local_bytes, local_oldest_us = pending_metrics(local_segments)
                cache_count, cache_bytes, cache_oldest_us = pending_metrics(local_cache_segments)
                capture_count, capture_bytes, capture_oldest_us = pending_metrics(capture_segments)
                self.cached_pending_status = {
                    "local_count": local_count,
                    "local_bytes": local_bytes,
                    "local_oldest_us": local_oldest_us,
                    "cache_count": cache_count,
                    "cache_bytes": cache_bytes,
                    "cache_oldest_us": cache_oldest_us,
                    "capture_count": capture_count,
                    "capture_bytes": capture_bytes,
                    "capture_oldest_us": capture_oldest_us,
                    "publish_journal_count": len(publish_journals),
                    "publish_recovery_count": publish_recovery_count,
                }
            metrics = self.cached_pending_status
            assert metrics is not None
            atomic_json_write(
                self.status_path,
                {
                    "schema": "gwv3_recording_uploader_status_v2",
                    "running": self.running and not STOP_REQUESTED,
                    "updated_us": now_us(),
                    "pipeline_mode": (
                        "nas_first_fragmented_mp4"
                        if self.rgb_output_mode == RGB_OUTPUT_FRAGMENTED_MP4
                        else "nas_first_local_cache_finalize"
                    ),
                    "rgb_output_mode": self.rgb_output_mode,
                    "staging_root": str(self.staging_root),
                    "nas_root": str(self.nas_root),
                    "capture_queue_root": str(self.capture_queue_root),
                    "local_pending_segments": metrics["local_count"],
                    "local_pending_bytes": metrics["local_bytes"],
                    "local_oldest_pending_age_ms": metrics["local_oldest_us"] // 1000,
                    "local_finalize_cache_segments": metrics["cache_count"],
                    "local_finalize_cache_bytes": metrics["cache_bytes"],
                    "local_finalize_cache_oldest_age_ms": metrics["cache_oldest_us"] // 1000,
                    "nas_finalize_pending_segments": (
                        metrics["capture_count"] + metrics["publish_recovery_count"]
                    ),
                    "nas_finalize_pending_bytes": metrics["capture_bytes"],
                    "nas_finalize_oldest_pending_age_ms": (
                        metrics["capture_oldest_us"] // 1000
                    ),
                    "publish_recovery_journals": metrics["publish_journal_count"],
                    "pending_segments": (
                        metrics["local_count"]
                        + metrics["capture_count"]
                        + metrics["publish_recovery_count"]
                    ),
                    "pending_bytes": metrics["local_bytes"] + metrics["capture_bytes"],
                    "oldest_pending_age_ms": max(
                        metrics["local_oldest_us"],
                        metrics["capture_oldest_us"],
                    )
                    // 1000,
                    "active_segment": self.active_segment,
                    "active_phase": self.active_phase,
                    "active_bytes_done": self.active_bytes_done,
                    "active_bytes_total": self.active_bytes_total,
                    "active_progress_percent": (
                        round(
                            self.active_bytes_done * 100.0 / self.active_bytes_total,
                            2,
                        )
                        if self.active_bytes_total > 0
                        else 0.0
                    ),
                    "active_elapsed_ms": (
                        max(0, now_us() - self.active_started_us) // 1000
                        if self.active_started_us > 0
                        else 0
                    ),
                    "captured_segments": self.captured,
                    "completed_segments": self.completed,
                    "failed_attempts": self.failures,
                    "last_capture_success_us": self.last_capture_success_us,
                    "last_success_us": self.last_success_us,
                    "local_remuxes": self.local_remuxes,
                    "nas_fallback_remuxes": self.nas_fallback_remuxes,
                    "fragmented_passthroughs": self.fragmented_passthroughs,
                    "local_cache_pressure_releases": self.local_cache_pressure_releases,
                    "last_capture_duration_ms": self.last_capture_duration_ms,
                    "last_finalize_duration_ms": self.last_finalize_duration_ms,
                    "last_publish_duration_ms": self.last_publish_duration_ms,
                    "last_remux_source": self.last_remux_source,
                    "finalize_workers": self.finalize_workers,
                    "last_error": self.last_error,
                },
            )

    def update_active_status(
        self,
        phase: str | None = None,
        bytes_done: int | None = None,
        bytes_total: int | None = None,
        force: bool = False,
    ) -> None:
        with self.status_lock:
            if phase is not None:
                # Progress callbacks continue to publish byte counters while an
                # operation is paused. They must not make a paused transfer look
                # active by replacing the receiver-I/O wait phase.
                self.active_phase = self.receiver_pause_phase or phase
            if bytes_done is not None:
                self.active_bytes_done = max(0, bytes_done)
            if bytes_total is not None:
                self.active_bytes_total = max(0, bytes_total)
            current = time.monotonic()
            if not force and current < self.next_active_status_at:
                return
            self.next_active_status_at = current + self.status_write_interval
            self.write_status()

    def should_pause_for_receiver_io(self, resume_phase: str) -> bool:
        if not self.pause_during_receiver_finalize:
            return False
        with self.receiver_status_lock:
            current = time.monotonic()
            if current >= self.next_receiver_status_at:
                self.next_receiver_status_at = current + 1.0
                try:
                    with urllib.request.urlopen(self.receiver_admin_url, timeout=0.5) as response:
                        status = json.load(response)
                    if not isinstance(status, dict):
                        raise ValueError("receiver status must be a JSON object")
                    self.receiver_pause_phase = receiver_pause_phase(
                        status,
                        now_us(),
                        self.segment_seconds,
                        self.quiet_before_segment_finalize_ms,
                        self.pause_record_queue_bytes,
                        self.pause_record_queue_oldest_age_ms,
                    )
                except (OSError, TypeError, ValueError, json.JSONDecodeError):
                    # Fail closed while a receiver-I/O pause is already active.
                    # A transient Admin timeout must not resume a large remux or
                    # upload immediately before a segment boundary.
                    pass
            pause_phase = self.receiver_pause_phase
        if pause_phase:
            self.update_active_status(phase=pause_phase)
            return True
        if self.active_phase in {
            "receiver_finalize_wait",
            "receiver_record_pressure_wait",
            "receiver_segment_boundary_wait",
        }:
            self.update_active_status(phase=resume_phase, force=True)
        return False

    def wait_for_receiver_io(self, resume_phase: str) -> None:
        while self.should_pause_for_receiver_io(resume_phase):
            if STOP_REQUESTED:
                raise InterruptedError("recording upload interrupted while waiting for receiver I/O")
            time.sleep(0.25)

    def local_cache_marker(self, segment: Path) -> tuple[Path, dict[str, Any]] | None:
        markers = sorted(segment.glob("*recording_capture_cached.json"))
        if len(markers) != 1:
            return None
        try:
            marker = load_json(markers[0])
        except (OSError, ValueError, json.JSONDecodeError):
            return None
        if (
            marker.get("schema") != "gwv3_recording_local_capture_cache_v1"
            or marker.get("cached") is not True
        ):
            return None
        return markers[0], marker

    def final_destination_for_local_cache(self, marker: dict[str, Any]) -> Path | None:
        try:
            relative = safe_relative_path(marker.get("relative_path"), Path("capture"))
            ready_name = marker_filename(
                marker.get("ready_file"),
                "recording_ready.json",
                "ready_file",
            )
            base = self.nas_root / relative
            candidates = [base]
            try:
                candidates.extend(
                    sorted(base.parent.glob(base.name + "_recovered_*"))
                )
            except OSError:
                pass
            for destination in candidates:
                ready_path = destination / ready_name
                if not ready_path.is_file():
                    continue
                ready = load_json(ready_path)
                if (
                    ready.get("ready") is True
                    and ready_identity(ready) == ready_identity(marker)
                ):
                    return destination
        except (OSError, TypeError, ValueError, json.JSONDecodeError):
            return None
        return None

    def local_cache_has_durable_nas_copy(self, marker: dict[str, Any]) -> bool:
        if self.final_destination_for_local_cache(marker) is not None:
            return True
        try:
            capture_directory = marker_filename(
                marker.get("capture_directory"),
                "capture",
                "capture_directory",
            )
            capture_segment = self.capture_queue_root / capture_directory
            capture_markers = sorted(
                capture_segment.glob("*recording_capture_ready.json")
            )
            if len(capture_markers) != 1:
                return False
            capture = load_json(capture_markers[0])
            return (
                capture.get("capture_ready") is True
                and ready_identity(capture) == ready_identity(marker)
                and str(marker.get("capture_key") or "") == capture_queue_key(capture)
            )
        except (OSError, TypeError, ValueError, json.JSONDecodeError):
            return False

    def release_local_cache(
        self,
        segment: Path,
        marker_path: Path,
        marker: dict[str, Any],
        destination: Path | None,
        reason: str,
    ) -> bool:
        if not self.local_cache_has_durable_nas_copy(marker):
            return False
        try:
            if self.delete_after_upload:
                shutil.rmtree(segment)
                fsync_directory(segment.parent)
            else:
                atomic_json_write(
                    segment / "recording_uploaded.json",
                    {
                        "schema": "gwv3_recording_uploaded_v1",
                        "destination": str(destination or ""),
                        "uploaded_at_us": now_us(),
                    },
                )
                marker_path.unlink(missing_ok=True)
                fsync_directory(segment)
            print(
                f"recording local capture cache released source={segment} "
                f"destination={destination or ''} reason={reason}",
                flush=True,
            )
            return True
        except OSError as error:
            print(
                f"recording local capture cache release deferred source={segment} "
                f"reason={reason} error={error}",
                file=sys.stderr,
                flush=True,
            )
            return False

    def reconcile_local_capture_caches(self) -> bool:
        progress = False
        for segment in discover_local_capture_caches(self.staging_root):
            cached = self.local_cache_marker(segment)
            if cached is None:
                for marker_path in segment.glob("*recording_capture_cached.json"):
                    marker_path.unlink(missing_ok=True)
                fsync_directory(segment)
                print(
                    f"recording invalid local capture cache requeued source={segment}",
                    file=sys.stderr,
                    flush=True,
                )
                progress = True
                continue
            marker_path, marker = cached
            destination = self.final_destination_for_local_cache(marker)
            if destination is not None:
                progress = (
                    self.release_local_cache(
                        segment,
                        marker_path,
                        marker,
                        destination,
                        "final_published",
                    )
                    or progress
                )
                continue
            if not self.local_cache_has_durable_nas_copy(marker):
                marker_path.unlink(missing_ok=True)
                fsync_directory(segment)
                print(
                    f"recording local capture cache requeued source={segment} "
                    "reason=nas_capture_missing",
                    file=sys.stderr,
                    flush=True,
                )
                progress = True
        return progress

    def enforce_local_cache_watermark(self) -> bool:
        if (
            not self.delete_after_upload
            or self.local_cache_high_watermark_percent <= 0
        ):
            return False
        try:
            usage = shutil.disk_usage(self.staging_root)
        except OSError:
            return False
        used_percent = usage.used * 100.0 / usage.total if usage.total > 0 else 0.0
        if used_percent < self.local_cache_high_watermark_percent:
            return False
        progress = False
        for segment in discover_local_capture_caches(self.staging_root):
            cached = self.local_cache_marker(segment)
            if cached is None:
                continue
            marker_path, marker = cached
            if not self.local_cache_has_durable_nas_copy(marker):
                continue
            destination = self.final_destination_for_local_cache(marker)
            if self.release_local_cache(
                segment,
                marker_path,
                marker,
                destination,
                "disk_high_watermark",
            ):
                self.local_cache_pressure_releases += 1
                progress = True
            try:
                usage = shutil.disk_usage(self.staging_root)
            except OSError:
                break
            used_percent = usage.used * 100.0 / usage.total if usage.total > 0 else 0.0
            if used_percent <= self.local_cache_low_watermark_percent:
                break
        return progress

    def process_local_one(self, segment: Path) -> bool:
        operation_started = time.monotonic()
        lock_path = local_segment_lock_path(segment)
        descriptor = acquire_file_lock(lock_path)
        if descriptor is None:
            return False
        self.active_segment = str(segment)
        self.active_started_us = now_us()
        self.active_bytes_done = 0
        self.active_bytes_total = 0
        self.next_active_status_at = 0.0
        self.update_active_status(phase="validating_local_capture", force=True)
        try:
            capture, capture_name = prepare_local_capture(segment, self.staging_root)

            def publish_once() -> tuple[Path, bool]:
                self.wait_for_receiver_io("capturing_to_nas")
                self.update_active_status(
                    phase="capturing_to_nas",
                    bytes_done=0,
                    bytes_total=0,
                    force=True,
                )
                return publish_capture_segment(
                    segment,
                    self.capture_queue_root,
                    capture,
                    capture_name,
                    self.bandwidth_mbps,
                    progress=lambda done, total: self.update_active_status(
                        phase="capturing_to_nas", bytes_done=done, bytes_total=total
                    ),
                    pause=lambda: self.should_pause_for_receiver_io("capturing_to_nas"),
                )

            destination, already_captured = run_with_transient_nas_retries(
                publish_once,
                on_retry=lambda attempt, attempts, error, delay: (
                    self.update_active_status(
                        phase="capture_retry_wait", bytes_done=0, bytes_total=0, force=True
                    ),
                    print(
                        f"recording capture transient NAS error source={segment} "
                        f"attempt={attempt}/{attempts} retry_delay_s={delay:g} error={error}",
                        file=sys.stderr,
                        flush=True,
                    ),
                ),
            )
            if not already_captured:
                self.captured += 1
            self.last_capture_success_us = now_us()
            self.last_error = ""
            if self.retain_local_capture_for_finalize:
                write_local_capture_cache_marker(segment, destination, capture)
            elif self.delete_after_upload:
                shutil.rmtree(segment)
            else:
                atomic_json_write(
                    segment / "recording_uploaded.json",
                    {
                        "schema": "gwv3_recording_captured_v1",
                        "destination": str(destination),
                        "captured_at_us": now_us(),
                    },
                )
            release_file_lock(lock_path, descriptor)
            descriptor = None
            self.last_capture_duration_ms = round(
                (time.monotonic() - operation_started) * 1000
            )
            print(
                f"recording capture completed source={segment} destination={destination} "
                f"duration_ms={self.last_capture_duration_ms} "
                f"local_cache_retained={str(self.retain_local_capture_for_finalize).lower()}",
                flush=True,
            )
            self.enforce_local_cache_watermark()
            return True
        except InterruptedError as error:
            print(f"recording capture interrupted source={segment} error={error}", file=sys.stderr, flush=True)
            return False
        except Exception as error:
            self.failures += 1
            self.last_error = f"{type(error).__name__}: {error}"
            print(f"recording capture failed source={segment} error={self.last_error}", file=sys.stderr, flush=True)
            traceback.print_exc(file=sys.stderr)
            return False
        finally:
            release_file_lock(lock_path, descriptor)
            self.active_segment = ""
            self.active_phase = ""
            self.active_bytes_done = 0
            self.active_bytes_total = 0
            self.active_started_us = 0
            self.update_active_status(force=True)

    def finalize_local_capture_worker(self, segment: Path) -> dict[str, Any]:
        operation_started = time.monotonic()
        lock_path = capture_segment_lock_path(segment)
        descriptor = acquire_file_lock(lock_path)
        if descriptor is None:
            return {"segment": segment, "deferred": True}
        try:
            capture, ready, finalized_path = load_captured_segment_state(segment)
            if ready is not None or finalized_path is not None:
                release_file_lock(lock_path, descriptor)
                return {"segment": segment, "deferred": True}
            local_segment = local_capture_cache_for(
                capture,
                self.staging_root,
                segment,
            )
            if local_segment is None:
                release_file_lock(lock_path, descriptor)
                return {"segment": segment, "deferred": True}
            self.wait_for_receiver_io("finalizing_local_cache_batch")
            finalize_started = time.monotonic()
            ready, finalized_path, remux_source = finalize_captured_segment(
                segment,
                capture,
                self.ffmpeg_path,
                heartbeat=self.update_active_status,
                pause=lambda: self.should_pause_for_receiver_io(
                    "finalizing_local_cache_batch"
                ),
                local_segment=local_segment,
                rgb_output_mode=self.rgb_output_mode,
            )
            return {
                "segment": segment,
                "lock_path": lock_path,
                "descriptor": descriptor,
                "capture": capture,
                "ready": ready,
                "finalized_path": finalized_path,
                "local_segment": local_segment,
                "remux_source": remux_source,
                "finalize_duration_ms": round(
                    (time.monotonic() - finalize_started) * 1000
                ),
                "operation_started": operation_started,
                "deferred": False,
            }
        except Exception:
            release_file_lock(lock_path, descriptor)
            raise

    def publish_parallel_capture_result(self, result: dict[str, Any]) -> bool:
        segment = result["segment"]
        lock_path = result["lock_path"]
        descriptor = result["descriptor"]
        capture = result["capture"]
        ready = result["ready"]
        finalized_path = result["finalized_path"]
        local_segment = result["local_segment"]
        remux_source = str(result["remux_source"])
        finalize_duration_ms = int(result["finalize_duration_ms"])
        operation_started = float(result["operation_started"])
        self.active_segment = str(segment)
        self.active_started_us = now_us()
        self.active_bytes_done = 0
        self.active_bytes_total = 0
        self.update_active_status(phase="publishing_final", force=True)
        try:
            def publish_once() -> Path:
                self.wait_for_receiver_io("publishing_final")
                return publish_finalized_capture(
                    segment,
                    self.nas_root,
                    capture,
                    ready,
                    finalized_path,
                )

            publish_started = time.monotonic()
            destination = run_with_transient_nas_retries(
                publish_once,
                on_retry=lambda attempt, attempts, error, delay: (
                    self.update_active_status(
                        phase="final_publish_retry_wait",
                        force=True,
                    ),
                    print(
                        f"recording final publish transient NAS error source={segment} "
                        f"attempt={attempt}/{attempts} retry_delay_s={delay:g} error={error}",
                        file=sys.stderr,
                        flush=True,
                    ),
                ),
            )
            publish_duration_ms = round(
                (time.monotonic() - publish_started) * 1000
            )
            self.completed += 1
            self.last_success_us = now_us()
            self.last_error = ""
            self.last_finalize_duration_ms = finalize_duration_ms
            self.last_publish_duration_ms = publish_duration_ms
            self.last_remux_source = remux_source
            if remux_source == "local":
                self.local_remuxes += 1
            elif remux_source == "nas":
                self.nas_fallback_remuxes += 1
            elif remux_source == "fragmented_passthrough":
                self.fragmented_passthroughs += 1
            cached = self.local_cache_marker(local_segment)
            if cached is not None:
                self.release_local_cache(
                    local_segment,
                    cached[0],
                    cached[1],
                    destination,
                    "final_published",
                )
            print(
                f"recording final publish completed source={segment} destination={destination} "
                f"remux_source={remux_source} finalize_ms={finalize_duration_ms} "
                f"publish_ms={publish_duration_ms} "
                f"total_ms={round((time.monotonic() - operation_started) * 1000)} "
                "parallel=true",
                flush=True,
            )
            return True
        except InterruptedError as error:
            print(
                f"recording NAS final publication interrupted source={segment} error={error}",
                file=sys.stderr,
                flush=True,
            )
            return False
        except Exception as error:
            self.failures += 1
            self.last_error = f"{type(error).__name__}: {error}"
            print(
                f"recording NAS final publication failed source={segment} "
                f"error={self.last_error}",
                file=sys.stderr,
                flush=True,
            )
            traceback.print_exc(file=sys.stderr)
            return False
        finally:
            release_file_lock(lock_path, descriptor)

    def process_capture_batch(self, segments: list[Path]) -> bool:
        local_candidates: list[Path] = []
        sequential: list[Path] = []
        for segment in segments:
            try:
                capture, ready, finalized_path = load_captured_segment_state(segment)
                if (
                    ready is None
                    and finalized_path is None
                    and local_capture_cache_for(
                        capture,
                        self.staging_root,
                        segment,
                    )
                    is not None
                ):
                    local_candidates.append(segment)
                else:
                    sequential.append(segment)
            except Exception:
                sequential.append(segment)

        if len(local_candidates) < 2 or self.finalize_workers <= 1:
            sequential = segments
            local_candidates = []

        progress = False
        if local_candidates:
            self.active_segment = f"{len(local_candidates)} local capture caches"
            self.active_started_us = now_us()
            self.active_bytes_done = 0
            self.active_bytes_total = 0
            self.update_active_status(
                phase="finalizing_local_cache_batch",
                force=True,
            )
            futures: dict[Future[dict[str, Any]], Path] = {}
            with ThreadPoolExecutor(
                max_workers=min(self.finalize_workers, len(local_candidates)),
                thread_name_prefix="gwv3-remux",
            ) as executor:
                for segment in local_candidates:
                    futures[executor.submit(self.finalize_local_capture_worker, segment)] = segment
                for future in as_completed(futures):
                    segment = futures[future]
                    try:
                        result = future.result()
                    except InterruptedError as error:
                        print(
                            f"recording parallel NAS finalization interrupted "
                            f"source={segment} error={error}",
                            file=sys.stderr,
                            flush=True,
                        )
                        continue
                    except Exception as error:
                        self.failures += 1
                        self.last_error = f"{type(error).__name__}: {error}"
                        print(
                            f"recording parallel NAS finalization failed source={segment} "
                            f"error={self.last_error}",
                            file=sys.stderr,
                            flush=True,
                        )
                        traceback.print_exc(file=sys.stderr)
                        continue
                    if result.get("deferred"):
                        sequential.append(segment)
                    elif self.publish_parallel_capture_result(result):
                        progress = True
            self.active_segment = ""
            self.active_phase = ""
            self.active_started_us = 0
            self.update_active_status(force=True)

        for segment in sequential:
            if STOP_REQUESTED:
                break
            if self.process_capture_one(segment):
                progress = True
        return progress

    def process_capture_one(self, segment: Path) -> bool:
        operation_started = time.monotonic()
        lock_path = capture_segment_lock_path(segment)
        descriptor = acquire_file_lock(lock_path)
        if descriptor is None:
            return False
        self.active_segment = str(segment)
        self.active_started_us = now_us()
        self.active_bytes_done = 0
        self.active_bytes_total = 0
        self.next_active_status_at = 0.0
        self.update_active_status(phase="finalizing_on_nas", force=True)
        try:
            capture, ready, finalized_path = load_captured_segment_state(segment)

            local_segment = local_capture_cache_for(
                capture,
                self.staging_root,
                segment,
            )
            remux_source = "already_finalized"
            finalize_duration_ms = 0
            if ready is None or finalized_path is None:
                self.wait_for_receiver_io("finalizing_on_nas")
                self.update_active_status(phase="finalizing_on_nas", force=True)
                finalize_started = time.monotonic()
                ready, finalized_path, remux_source = finalize_captured_segment(
                    segment,
                    capture,
                    self.ffmpeg_path,
                    heartbeat=self.update_active_status,
                    pause=lambda: self.should_pause_for_receiver_io("finalizing_on_nas"),
                    local_segment=local_segment,
                    rgb_output_mode=self.rgb_output_mode,
                )
                finalize_duration_ms = round(
                    (time.monotonic() - finalize_started) * 1000
                )
            assert ready is not None and finalized_path is not None

            def publish_once() -> Path:
                self.wait_for_receiver_io("publishing_final")
                self.update_active_status(phase="publishing_final", force=True)
                return publish_finalized_capture(
                    segment,
                    self.nas_root,
                    capture,
                    ready,
                    finalized_path,
                )

            publish_started = time.monotonic()
            destination = run_with_transient_nas_retries(
                publish_once,
                on_retry=lambda attempt, attempts, error, delay: (
                    self.update_active_status(phase="final_publish_retry_wait", force=True),
                    print(
                        f"recording final publish transient NAS error source={segment} "
                        f"attempt={attempt}/{attempts} retry_delay_s={delay:g} error={error}",
                        file=sys.stderr,
                        flush=True,
                    ),
                ),
            )
            publish_duration_ms = round(
                (time.monotonic() - publish_started) * 1000
            )
            self.completed += 1
            self.last_success_us = now_us()
            self.last_error = ""
            self.last_finalize_duration_ms = finalize_duration_ms
            self.last_publish_duration_ms = publish_duration_ms
            self.last_remux_source = remux_source
            if remux_source == "local":
                self.local_remuxes += 1
            elif remux_source == "nas":
                self.nas_fallback_remuxes += 1
            elif remux_source == "fragmented_passthrough":
                self.fragmented_passthroughs += 1
            if local_segment is not None:
                cached = self.local_cache_marker(local_segment)
                if cached is not None:
                    self.release_local_cache(
                        local_segment,
                        cached[0],
                        cached[1],
                        destination,
                        "final_published",
                    )
            release_file_lock(lock_path, descriptor)
            descriptor = None
            print(
                f"recording final publish completed source={segment} destination={destination} "
                f"remux_source={remux_source} finalize_ms={finalize_duration_ms} "
                f"publish_ms={publish_duration_ms} "
                f"total_ms={round((time.monotonic() - operation_started) * 1000)}",
                flush=True,
            )
            return True
        except InterruptedError as error:
            print(f"recording NAS finalization interrupted source={segment} error={error}", file=sys.stderr, flush=True)
            return False
        except Exception as error:
            self.failures += 1
            self.last_error = f"{type(error).__name__}: {error}"
            print(f"recording NAS finalization failed source={segment} error={self.last_error}", file=sys.stderr, flush=True)
            traceback.print_exc(file=sys.stderr)
            return False
        finally:
            release_file_lock(lock_path, descriptor)
            self.active_segment = ""
            self.active_phase = ""
            self.active_bytes_done = 0
            self.active_bytes_total = 0
            self.active_started_us = 0
            self.update_active_status(force=True)

    def recover_pending_publications(self) -> bool:
        progress = False
        for journal_path in discover_publish_journals(self.capture_queue_root):
            if STOP_REQUESTED:
                break
            self.active_segment = str(journal_path)
            self.active_started_us = now_us()
            self.active_bytes_done = 0
            self.active_bytes_total = 0
            self.next_active_status_at = 0.0
            self.update_active_status(phase="recovering_final_publish", force=True)
            try:
                destination = run_with_transient_nas_retries(
                    lambda: recover_publish_journal(
                        journal_path,
                        self.nas_root,
                        self.capture_queue_root,
                    ),
                    on_retry=lambda attempt, attempts, error, delay: (
                        self.update_active_status(
                            phase="final_publish_retry_wait",
                            force=True,
                        ),
                        print(
                            f"recording publish recovery transient NAS error "
                            f"journal={journal_path} attempt={attempt}/{attempts} "
                            f"retry_delay_s={delay:g} error={error}",
                            file=sys.stderr,
                            flush=True,
                        ),
                    ),
                )
                if destination is not None:
                    self.completed += 1
                    self.last_success_us = now_us()
                    self.last_error = ""
                    progress = True
                    print(
                        f"recording final publish recovered journal={journal_path} "
                        f"destination={destination}",
                        flush=True,
                    )
            except InterruptedError as error:
                print(
                    f"recording publish recovery interrupted journal={journal_path} error={error}",
                    file=sys.stderr,
                    flush=True,
                )
                break
            except Exception as error:
                self.failures += 1
                self.last_error = f"{type(error).__name__}: {error}"
                print(
                    f"recording publish recovery failed journal={journal_path} "
                    f"error={self.last_error}",
                    file=sys.stderr,
                    flush=True,
                )
                traceback.print_exc(file=sys.stderr)
            finally:
                self.active_segment = ""
                self.active_phase = ""
                self.active_bytes_done = 0
                self.active_bytes_total = 0
                self.active_started_us = 0
                self.update_active_status(force=True)
        return progress

    def process_one(self, segment: Path) -> bool:
        return self.process_local_one(segment)

    def run_once(self) -> bool:
        progress = self.recover_pending_publications()
        progress = self.reconcile_local_capture_caches() or progress
        local_segments = discover_local_segments(self.staging_root)
        self.write_status(refresh_metrics=True)
        for segment in local_segments:
            if STOP_REQUESTED:
                break
            if self.process_local_one(segment):
                progress = True
            self.write_status()
        progress = self.enforce_local_cache_watermark() or progress
        self.write_status(refresh_metrics=True)
        capture_segments = discover_capture_segments(self.capture_queue_root)
        if self.process_capture_batch(capture_segments):
            progress = True
        progress = self.reconcile_local_capture_caches() or progress
        self.write_status(refresh_metrics=True)
        return progress

    def run(self, once: bool) -> int:
        if not self.enabled:
            print("recording uploader disabled by receiver config", flush=True)
            return 0
        if once:
            self.running = True
            try:
                self.run_once()
            finally:
                self.running = False
                remaining_local = discover_local_segments(self.staging_root)
                remaining_capture = discover_capture_segments(self.capture_queue_root)
                remaining_journals = discover_publish_journals(self.capture_queue_root)
                self.write_status(refresh_metrics=True)
            return 0 if not remaining_local and not remaining_capture and not remaining_journals else 2
        self.running = True
        try:
            while not STOP_REQUESTED:
                self.run_once()
                deadline = time.monotonic() + self.interval
                while not STOP_REQUESTED and time.monotonic() < deadline:
                    time.sleep(min(0.25, deadline - time.monotonic()))
        finally:
            self.running = False
            self.write_status(refresh_metrics=True)
        return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--once", action="store_true")
    return parser.parse_args()


def main() -> int:
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    arguments = parse_args()
    try:
        return Uploader(arguments.config).run(arguments.once)
    except Exception as error:
        print(f"recording uploader error: {type(error).__name__}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
