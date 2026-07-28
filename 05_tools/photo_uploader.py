#!/usr/bin/env python3
"""Publish receiver-staged voice photos to NAS without blocking media ingest."""

from __future__ import annotations

import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import sys
import time
from typing import Any
import zlib


STOP_REQUESTED = False


def request_stop(_signum: int, _frame: object) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True


def now_us() -> int:
    return time.time_ns() // 1000


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


def atomic_json_write(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=True, sort_keys=True)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)
    fsync_directory(path.parent)


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise ValueError(f"JSON object required: {path}")
    return value


def safe_filename(value: Any, field: str) -> str:
    name = str(value or "")
    if not name or name in {".", ".."} or Path(name).name != name:
        raise ValueError(f"unsafe {field}: {name!r}")
    return name


def safe_relative_path(value: Any) -> Path:
    text = str(value or "")
    path = Path(text)
    if not text or path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise ValueError(f"unsafe relative_path: {text!r}")
    if any(not all(character.isalnum() or character in "_.-" for character in part) for part in path.parts):
        raise ValueError(f"unsafe relative_path characters: {text!r}")
    return path


def file_crc32(path: Path) -> tuple[int, int]:
    checksum = 0
    size = 0
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            checksum = zlib.crc32(chunk, checksum)
            size += len(chunk)
    return size, checksum & 0xFFFFFFFF


def same_file_content(path: Path, expected_size: int, expected_crc32: int) -> bool:
    try:
        if path.stat().st_size != expected_size:
            return False
        size, checksum = file_crc32(path)
        return size == expected_size and checksum == expected_crc32
    except OSError:
        return False


def collision_destination(destination: Path) -> Path:
    for index in range(1, 1000):
        candidate = destination.with_name(f"{destination.stem}_{index:03d}{destination.suffix}")
        if not candidate.exists():
            return candidate
    raise RuntimeError(f"cannot allocate collision-free NAS photo path for {destination}")


def copy_file_atomic(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(f".{destination.name}.gwv3-photo-{os.getpid()}.tmp")
    try:
        temporary.unlink()
    except FileNotFoundError:
        pass
    try:
        with source.open("rb") as source_handle, temporary.open("xb") as destination_handle:
            shutil.copyfileobj(source_handle, destination_handle, length=1024 * 1024)
            destination_handle.flush()
            os.fsync(destination_handle.fileno())
        os.replace(temporary, destination)
        fsync_directory(destination.parent)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def publish_job(job_directory: Path, nas_root: Path) -> Path:
    marker_path = job_directory / "photo_ready.json"
    marker = load_json(marker_path)
    if marker.get("ready") is not True:
        raise ValueError(f"photo task is not ready: {job_directory}")
    jpeg_name = safe_filename(marker.get("jpeg_file"), "jpeg_file")
    source = job_directory / jpeg_name
    if not source.is_file():
        raise FileNotFoundError(f"staged JPEG is missing: {source}")

    expected_size = int(marker["jpeg_size"]) if "jpeg_size" in marker else -1
    expected_crc32 = int(marker["jpeg_crc32"]) if "jpeg_crc32" in marker else -1
    actual_size, actual_crc32 = file_crc32(source)
    if expected_size != actual_size or expected_crc32 != actual_crc32:
        raise ValueError(
            f"staged JPEG integrity mismatch: size={actual_size}/{expected_size} "
            f"crc32={actual_crc32}/{expected_crc32}"
        )

    relative = safe_relative_path(marker.get("relative_path"))
    destination = nas_root / relative
    if destination.exists():
        if same_file_content(destination, expected_size, expected_crc32):
            published = destination
        else:
            destination = collision_destination(destination)
            marker["relative_path"] = destination.relative_to(nas_root).as_posix()
            atomic_json_write(marker_path, marker)
            copy_file_atomic(source, destination)
            published = destination
    else:
        copy_file_atomic(source, destination)
        published = destination

    shutil.rmtree(job_directory)
    fsync_directory(job_directory.parent)
    return published


class PhotoUploader:
    def __init__(self, config_path: Path, require_nas_mount: bool = True) -> None:
        config = load_json(config_path)
        photo = config.get("photo_capture") or {}
        if not isinstance(photo, dict):
            raise ValueError("photo_capture config must be an object")
        self.enabled = bool(photo.get("enabled", False))
        staging_root_text = str(photo.get("staging_root") or "")
        nas_root_text = str(config.get("nas_root") or "")
        self.staging_root = Path(staging_root_text).expanduser()
        self.nas_root = Path(nas_root_text).expanduser()
        self.interval_seconds = max(0.05, int(photo.get("upload_interval_ms", 250)) / 1000.0)
        self.require_nas_mount = require_nas_mount
        if self.enabled and (not staging_root_text or not nas_root_text):
            raise ValueError("photo_capture.staging_root and nas_root are required")
        self.status_path = self.staging_root.parent / ".gwv3_photo_uploader_status.json"
        self.completed = 0
        self.failures = 0
        self.last_error = ""
        self.last_published_path = ""
        self.failure_counts: dict[str, int] = {}
        self.retry_after: dict[str, float] = {}

    def nas_available(self) -> bool:
        if not self.nas_root.is_dir():
            return False
        return not self.require_nas_mount or os.path.ismount(self.nas_root)

    def write_status(self, pending: int) -> None:
        payload = {
            "schema_version": 1,
            "running": not STOP_REQUESTED,
            "enabled": self.enabled,
            "nas_available": self.nas_available() if self.enabled else False,
            "pending": pending,
            "completed": self.completed,
            "failures": self.failures,
            "last_error": self.last_error,
            "last_published_path": self.last_published_path,
            "updated_at_unix_us": now_us(),
        }
        try:
            atomic_json_write(self.status_path, payload)
        except OSError:
            pass

    def ready_jobs(self) -> list[Path]:
        if not self.staging_root.is_dir():
            return []
        jobs = []
        for path in self.staging_root.iterdir():
            if path.name.startswith(".") or not path.is_dir():
                continue
            if (path / "photo_ready.json").is_file():
                jobs.append(path)
        return sorted(jobs, key=lambda item: item.stat().st_mtime_ns)

    def run_once(self) -> int:
        jobs = self.ready_jobs()
        if not jobs:
            self.write_status(0)
            return 0
        if not self.nas_available():
            self.last_error = f"NAS root is not mounted: {self.nas_root}"
            self.write_status(len(jobs))
            return 0

        published_count = 0
        for index, job in enumerate(jobs):
            if STOP_REQUESTED:
                break
            if time.monotonic() < self.retry_after.get(job.name, 0.0):
                continue
            try:
                published = publish_job(job, self.nas_root)
                self.completed += 1
                published_count += 1
                self.last_error = ""
                self.last_published_path = str(published)
                self.failure_counts.pop(job.name, None)
                self.retry_after.pop(job.name, None)
                print(f"photo published request_id={job.name} destination={published}", flush=True)
            except Exception as error:
                self.failures += 1
                self.last_error = str(error)
                failure_count = self.failure_counts.get(job.name, 0) + 1
                self.failure_counts[job.name] = failure_count
                retry_delay = min(60.0, float(2 ** min(failure_count - 1, 6)))
                self.retry_after[job.name] = time.monotonic() + retry_delay
                if failure_count <= 3 or failure_count % 10 == 0:
                    print(
                        f"photo publish failed request_id={job.name} retry_seconds={retry_delay:g} error={error}",
                        file=sys.stderr,
                        flush=True,
                    )
            self.write_status(max(0, len(jobs) - index - 1))
        return published_count

    def run(self, once: bool = False) -> None:
        if not self.enabled:
            self.write_status(0)
            print("photo uploader disabled by receiver config", flush=True)
            return
        self.staging_root.mkdir(parents=True, exist_ok=True)
        lock_path = self.staging_root / ".gwv3_photo_uploader.lock"
        with lock_path.open("a+b") as lock:
            try:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise RuntimeError("another photo uploader is already running") from error
            while not STOP_REQUESTED:
                self.run_once()
                if once:
                    break
                deadline = time.monotonic() + self.interval_seconds
                while not STOP_REQUESTED and time.monotonic() < deadline:
                    time.sleep(min(0.1, deadline - time.monotonic()))
        self.write_status(len(self.ready_jobs()))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--once", action="store_true")
    parser.add_argument(
        "--allow-unmounted-nas",
        action="store_true",
        help="Allow publishing into a normal directory; intended only for tests.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    uploader = PhotoUploader(args.config, require_nas_mount=not args.allow_unmounted_nas)
    uploader.run(once=args.once)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
