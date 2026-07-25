#!/usr/bin/env python3
"""Finalize locally staged recordings and publish them atomically to NAS."""

from __future__ import annotations

import argparse
import errno
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
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
    "recording_ready.json.tmp",
    "recording_uploaded.json",
}


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
) -> str:
    if int(status.get("record_finalize_outstanding_segments") or 0) > 0:
        return "receiver_finalize_wait"
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
) -> None:
    rgb_path = segment / rgb_name
    if not rgb_path.is_file() or rgb_path.stat().st_size == 0:
        return
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
        return
    if b"mfra" not in atoms:
        raise RuntimeError(f"RGB fragmented MP4 has no closing mfra atom: {rgb_path}")

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
            str(rgb_path),
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


def finalize_staged_segment(
    segment: Path,
    staged: dict[str, Any],
    staged_path: Path,
    ffmpeg_path: str,
    heartbeat: Callable[[], None] | None = None,
    pause: Callable[[], bool] | None = None,
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
    finalize_rgb(segment, rgb_name, ffmpeg_path, expected_rgb_duration, heartbeat, pause)
    rgb_exists = (segment / rgb_name).is_file() and (segment / rgb_name).stat().st_size > 0
    depth_exists = (segment / depth_name).is_file() and (segment / depth_name).stat().st_size > 0
    if not rgb_exists and not depth_exists:
        raise RuntimeError(f"no finalized media file found under {segment}")
    if depth_exists and media_duration(segment / depth_name, ffmpeg_path, pause) is None:
        raise RuntimeError(f"depth recording is not readable: {segment / depth_name}")

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
        "rgb_frame_index_mode": "frames_csv_rgb_recorded_columns",
        "finalized_by": "gwv3_recording_uploader",
    }
    atomic_json_write(ready_path, ready)
    staged_path.unlink(missing_ok=True)
    fsync_directory(segment)
    return ready, ready_path


def iter_copy_files(source: Path) -> Iterable[tuple[Path, Path]]:
    for root, directories, files in os.walk(source):
        directories[:] = sorted(name for name in directories if not name.startswith(".gwv3-uploading-"))
        root_path = Path(root)
        relative_root = root_path.relative_to(source)
        for name in sorted(files):
            if name in TRANSIENT_NAMES or name.startswith(".rgb.mp4.seekable.tmp"):
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


def destination_for(
    source: Path,
    staging_root: Path,
    nas_root: Path,
    ready: dict[str, Any],
    ready_name: str,
) -> tuple[Path, bool]:
    relative = source.relative_to(staging_root)
    if not relative.parts or any(part in {"", ".", ".."} for part in relative.parts):
        raise RuntimeError(f"unsafe staged relative path: {relative}")
    destination = nas_root / relative
    if not destination.exists():
        return destination, False
    existing_marker = destination / ready_name
    if existing_marker.is_file():
        try:
            if ready_identity(load_json(existing_marker)) == ready_identity(ready) and manifest(destination) == manifest(source):
                return destination, True
        except (OSError, ValueError, RuntimeError, json.JSONDecodeError):
            pass
    incomplete_marker = destination / ".gwv3_publish_incomplete.json"
    if incomplete_marker.is_file():
        try:
            if ready_identity(load_json(incomplete_marker)) == ready_identity(ready):
                shutil.rmtree(destination)
                fsync_directory(destination.parent)
                return destination, False
        except (OSError, ValueError, RuntimeError, json.JSONDecodeError):
            pass
    for suffix in range(1, 1000):
        candidate = destination.with_name(f"{destination.name}_recovered_{suffix:03d}")
        if not candidate.exists():
            return candidate, False
    raise RuntimeError(f"cannot allocate non-colliding NAS destination for {destination}")


def publish_segment(
    source: Path,
    staging_root: Path,
    nas_root: Path,
    ready: dict[str, Any],
    ready_name: str,
    bandwidth_mbps: float,
    progress: Callable[[int, int], None] | None = None,
    pause: Callable[[], bool] | None = None,
) -> Path:
    destination, already_published = destination_for(source, staging_root, nas_root, ready, ready_name)
    if already_published:
        (destination / ".gwv3_publish_incomplete.json").unlink(missing_ok=True)
        if progress is not None:
            total = sum(manifest(source).values())
            progress(total, total)
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary_prefix = f".gwv3-uploading-{destination.name}-"
    for stale_temporary in destination.parent.glob(temporary_prefix + "*"):
        if stale_temporary.is_dir():
            shutil.rmtree(stale_temporary, ignore_errors=True)
    temporary = destination.parent / f"{temporary_prefix}{os.getpid()}"
    if temporary.exists():
        shutil.rmtree(temporary)
    temporary.mkdir(parents=False)
    try:
        source_manifest = manifest(source)
        copy_manifest = dict(source_manifest)
        ready_size = copy_manifest.pop(ready_name, None)
        if ready_size is None:
            raise RuntimeError(f"recording ready marker missing before NAS publication: {source}")
        total_bytes = sum(source_manifest.values())
        copied_before_file = 0
        if progress is not None:
            progress(0, total_bytes)
        ordered_files = sorted(copy_manifest)
        for relative_text in ordered_files:
            relative = Path(relative_text)
            file_size = copy_manifest[relative_text]
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
        if manifest(temporary) != copy_manifest:
            raise RuntimeError(f"NAS copy verification failed: {temporary}")
        sender_id, camera_id, segment_start_us = ready_identity(ready)
        atomic_json_write(
            temporary / ".gwv3_publish_incomplete.json",
            {
                "schema": "gwv3_publish_incomplete_v1",
                "sender_id": sender_id,
                "camera_id": camera_id,
                "segment_start_us": segment_start_us,
            },
        )
        os.replace(temporary, destination)
        fsync_directory(destination.parent)
        ready_temporary = destination / (ready_name + ".tmp")
        copy_file_limited(
            source / ready_name,
            ready_temporary,
            0,
            (
                lambda copied, _total: progress(copied_before_file + copied, total_bytes)
                if progress is not None
                else None
            ),
            pause,
        )
        os.replace(ready_temporary, destination / ready_name)
        fsync_directory(destination)
        if manifest(destination) != source_manifest:
            (destination / ready_name).unlink(missing_ok=True)
            fsync_directory(destination)
            raise RuntimeError(f"published NAS segment verification failed: {destination}")
        (destination / ".gwv3_publish_incomplete.json").unlink(missing_ok=True)
        fsync_directory(destination)
        if progress is not None:
            progress(total_bytes, total_bytes)
        return destination
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def acquire_segment_lock(segment: Path) -> int | None:
    lock_path = segment / ".gwv3_uploader.lock"
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


def release_segment_lock(_segment: Path, descriptor: int | None) -> None:
    if descriptor is not None:
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)


def discover_segments(staging_root: Path) -> list[Path]:
    candidates: dict[str, Path] = {}
    if not staging_root.exists():
        return []
    for marker_pattern in ("*recording_staged.json", "*recording_ready.json"):
        for marker in staging_root.rglob(marker_pattern):
            if marker.parent == staging_root or ".gwv3-uploading-" in marker.as_posix():
                continue
            if (marker.parent / "recording_uploaded.json").is_file():
                continue
            candidates[str(marker.parent)] = marker.parent
    return sorted(candidates.values(), key=lambda path: path.stat().st_mtime)


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
        self.ffmpeg_path = str(config.get("ffmpeg_path") or "ffmpeg")
        self.interval = max(0.25, int(staging.get("upload_interval_ms", 2000)) / 1000.0)
        self.bandwidth_mbps = max(0.0, float(staging.get("upload_bandwidth_limit_mbps", 0)))
        self.delete_after_upload = bool(staging.get("delete_after_upload", True))
        self.pause_during_receiver_finalize = bool(staging.get("pause_during_receiver_finalize", True))
        self.quiet_before_segment_finalize_ms = max(
            0, int(staging.get("quiet_before_segment_finalize_ms", 60000))
        )
        self.segment_seconds = max(0, int(config.get("segment_seconds", 300)))
        self.receiver_admin_url = f"http://127.0.0.1:{int(config.get('admin_port', 18080))}/api/status"
        self.status_path = self.staging_root / ".gwv3_uploader_status.json"
        self.completed = 0
        self.failures = 0
        self.last_error = ""
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

        if not self.enabled:
            return
        if not staging_root_text or not nas_root_text:
            raise ValueError("recording staging root and nas_root are required")
        if self.staging_root.resolve() == self.nas_root.resolve():
            raise ValueError("recording staging root must differ from nas_root")
        self.staging_root.mkdir(parents=True, exist_ok=True)

    def write_status(self, segments: list[Path]) -> None:
        pending_count, pending_bytes, oldest_us = pending_metrics(segments)
        atomic_json_write(
            self.status_path,
            {
                "schema": "gwv3_recording_uploader_status_v1",
                "running": self.running and not STOP_REQUESTED,
                "updated_us": now_us(),
                "staging_root": str(self.staging_root),
                "nas_root": str(self.nas_root),
                "pending_segments": pending_count,
                "pending_bytes": pending_bytes,
                "oldest_pending_age_ms": oldest_us // 1000,
                "active_segment": self.active_segment,
                "active_phase": self.active_phase,
                "active_bytes_done": self.active_bytes_done,
                "active_bytes_total": self.active_bytes_total,
                "active_progress_percent": (
                    round(self.active_bytes_done * 100.0 / self.active_bytes_total, 2)
                    if self.active_bytes_total > 0
                    else 0.0
                ),
                "active_elapsed_ms": (
                    max(0, now_us() - self.active_started_us) // 1000 if self.active_started_us > 0 else 0
                ),
                "completed_segments": self.completed,
                "failed_attempts": self.failures,
                "last_success_us": self.last_success_us,
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
        self.next_active_status_at = current + 1.0
        self.write_status(discover_segments(self.staging_root))

    def should_pause_for_receiver_io(self, resume_phase: str) -> bool:
        if not self.pause_during_receiver_finalize:
            return False
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
                )
            except (OSError, TypeError, ValueError, json.JSONDecodeError):
                # Fail closed while a receiver-I/O pause is already active.
                # A transient Admin timeout must not resume a large remux or
                # upload immediately before a segment boundary.
                pass
        if self.receiver_pause_phase:
            self.update_active_status(phase=self.receiver_pause_phase)
            return True
        if self.active_phase in {"receiver_finalize_wait", "receiver_segment_boundary_wait"}:
            self.update_active_status(phase=resume_phase, force=True)
        return False

    def wait_for_receiver_io(self, resume_phase: str) -> None:
        while self.should_pause_for_receiver_io(resume_phase):
            if STOP_REQUESTED:
                raise InterruptedError("recording upload interrupted while waiting for receiver I/O")
            time.sleep(0.25)

    def process_one(self, segment: Path) -> bool:
        descriptor = acquire_segment_lock(segment)
        if descriptor is None:
            return False
        self.active_segment = str(segment)
        self.active_started_us = now_us()
        self.active_bytes_done = 0
        self.active_bytes_total = 0
        self.next_active_status_at = 0.0
        self.update_active_status(phase="finalizing", force=True)
        try:
            staged_markers = sorted(segment.glob("*recording_staged.json"))
            ready_markers = sorted(segment.glob("*recording_ready.json"))
            if len(staged_markers) > 1 or len(ready_markers) > 1:
                raise RuntimeError(f"ambiguous recording marker set: {segment}")
            if staged_markers:
                staged_path = staged_markers[0]
                self.wait_for_receiver_io("finalizing")
                ready, ready_path = finalize_staged_segment(
                    segment,
                    load_json(staged_path),
                    staged_path,
                    self.ffmpeg_path,
                    heartbeat=self.update_active_status,
                    pause=lambda: self.should_pause_for_receiver_io("finalizing"),
                )
            else:
                if not ready_markers:
                    raise RuntimeError(f"recording marker disappeared: {segment}")
                ready_path = ready_markers[0]
                ready = load_json(ready_path)
                if ready.get("ready") is not True:
                    raise RuntimeError(f"invalid ready marker: {segment}")
            def publish_once() -> Path:
                self.wait_for_receiver_io("uploading")
                self.update_active_status(phase="uploading", bytes_done=0, bytes_total=0, force=True)
                return publish_segment(
                    segment,
                    self.staging_root,
                    self.nas_root,
                    ready,
                    ready_path.name,
                    self.bandwidth_mbps,
                    progress=lambda done, total: self.update_active_status(
                        phase="uploading", bytes_done=done, bytes_total=total
                    ),
                    pause=lambda: self.should_pause_for_receiver_io("uploading"),
                )

            destination = run_with_transient_nas_retries(
                publish_once,
                on_retry=lambda attempt, attempts, error, delay: (
                    self.update_active_status(
                        phase="upload_retry_wait", bytes_done=0, bytes_total=0, force=True
                    ),
                    print(
                        f"recording upload transient NAS error source={segment} "
                        f"attempt={attempt}/{attempts} retry_delay_s={delay:g} error={error}",
                        file=sys.stderr,
                        flush=True,
                    ),
                ),
            )
            self.completed += 1
            self.last_success_us = now_us()
            self.last_error = ""
            if self.delete_after_upload:
                shutil.rmtree(segment)
            else:
                atomic_json_write(
                    segment / "recording_uploaded.json",
                    {"schema": "gwv3_recording_uploaded_v1", "destination": str(destination), "uploaded_at_us": now_us()},
                )
            release_segment_lock(segment, descriptor)
            descriptor = None
            print(f"recording upload completed source={segment} destination={destination}", flush=True)
            return True
        except InterruptedError as error:
            print(f"recording upload interrupted source={segment} error={error}", file=sys.stderr, flush=True)
            return False
        except Exception as error:
            self.failures += 1
            self.last_error = f"{type(error).__name__}: {error}"
            print(f"recording upload failed source={segment} error={self.last_error}", file=sys.stderr, flush=True)
            traceback.print_exc(file=sys.stderr)
            return False
        finally:
            release_segment_lock(segment, descriptor)
            self.active_segment = ""
            self.active_phase = ""
            self.active_bytes_done = 0
            self.active_bytes_total = 0
            self.active_started_us = 0
            self.update_active_status(force=True)

    def run_once(self) -> bool:
        segments = discover_segments(self.staging_root)
        self.write_status(segments)
        progress = False
        for segment in segments:
            if STOP_REQUESTED:
                break
            if self.process_one(segment):
                progress = True
            self.write_status(discover_segments(self.staging_root))
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
                remaining = discover_segments(self.staging_root)
                self.write_status(remaining)
            return 0 if not remaining else 2
        self.running = True
        try:
            while not STOP_REQUESTED:
                self.run_once()
                deadline = time.monotonic() + self.interval
                while not STOP_REQUESTED and time.monotonic() < deadline:
                    time.sleep(min(0.25, deadline - time.monotonic()))
        finally:
            self.running = False
            self.write_status(discover_segments(self.staging_root))
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
