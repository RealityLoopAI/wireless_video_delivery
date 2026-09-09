#!/usr/bin/env python3
import argparse
import datetime
import json
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path


def request_json(base_url: str, method: str, path: str, timeout: float = 5.0):
    request = urllib.request.Request(base_url.rstrip("/") + path, method=method)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = json.loads(response.read().decode("utf-8"))
    if not isinstance(payload, dict):
        raise RuntimeError(f"{path} returned a non-object response")
    return payload


def camera_map(status):
    return {
        str(camera.get("camera_key")): camera
        for camera in status.get("cameras", [])
        if isinstance(camera, dict) and camera.get("camera_key")
    }


def uploader_pending(status) -> int:
    uploader = status.get("recording_uploader") or {}
    values = [
        status.get("recording_delivery_pending", 0),
        uploader.get("pending_segments", 0),
        uploader.get("active_transfers", 0),
    ]
    return max(int(value or 0) for value in values)


def wait_status(base_url, description, predicate, timeout_seconds, poll_seconds=0.25):
    deadline = time.monotonic() + timeout_seconds
    latest = None
    while time.monotonic() < deadline:
        latest = request_json(base_url, "GET", "/api/status")
        if predicate(latest):
            return latest
        time.sleep(poll_seconds)
    raise TimeoutError(f"timed out waiting for {description}; latest={latest}")


def require_same_session(status, expected_session_id):
    current = int(status.get("recording_session_id") or 0)
    if current and expected_session_id and current != expected_session_id:
        raise RuntimeError(
            f"recording session changed from {expected_session_id} to {current}; "
            "refusing to control or validate the replacement session"
        )


def recording_roots(config_path: Path) -> list[Path]:
    config = json.loads(config_path.read_text(encoding="utf-8"))
    roots = [Path(str(config.get("nas_root") or ""))]
    staging = config.get("recording_staging") or {}
    if isinstance(staging, dict):
        roots.append(Path(str(staging.get("root") or "")))
    return roots


def recording_session_directories(config_path: Path, session_id: int) -> list[Path]:
    directories: set[Path] = set()
    session_seconds = session_id / 1_000_000
    date_names: set[str] = set()
    for session_time in (
        datetime.datetime.fromtimestamp(session_seconds),
        datetime.datetime.fromtimestamp(session_seconds, datetime.timezone.utc),
    ):
        for day_offset in (-1, 0, 1):
            date_names.add(
                (session_time + datetime.timedelta(days=day_offset)).strftime("%Y-%m-%d")
            )
    for root in recording_roots(config_path):
        if not root.is_absolute() or not root.is_dir() or str(root) == "/":
            continue
        for date_name in date_names:
            for date_directory in root.glob(f"*/{date_name}"):
                for marker in date_directory.rglob("*recording_ready.json"):
                    try:
                        metadata = json.loads(marker.read_text(encoding="utf-8"))
                        matches = int(metadata.get("recording_session_id") or 0) == session_id
                    except (OSError, ValueError, json.JSONDecodeError):
                        matches = False
                    if matches and marker.parent != root and root in marker.parent.parents:
                        directories.add(marker.parent)
    return sorted(directories)


def validate_recording_session(config_path: Path, session_id: int, expected_cameras: int) -> dict:
    directories = recording_session_directories(config_path, session_id)
    if len(directories) < expected_cameras:
        raise RuntimeError(
            f"recorded session has {len(directories)} published directories for {expected_cameras} cameras"
        )
    results = []
    quality_failures = []
    for directory in directories:
        ready = list(directory.glob("*recording_ready.json"))
        csv_files = list(directory.glob("*frames.csv"))
        rgb_files = list(directory.glob("*.mp4"))
        depth_files = list(directory.glob("*.mkv"))
        if len(ready) != 1 or len(csv_files) != 1 or len(rgb_files) != 1 or len(depth_files) != 1:
            raise RuntimeError(f"recorded directory is incomplete: {directory}")
        if any(path.stat().st_size <= 0 for path in csv_files + rgb_files + depth_files):
            raise RuntimeError(f"recorded directory contains an empty file: {directory}")
        with csv_files[0].open("r", encoding="utf-8", errors="replace") as source:
            line_count = sum(1 for _ in source)
        if line_count < 3:
            raise RuntimeError(f"frames CSV has no frame records: {csv_files[0]}")
        probe = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "stream=codec_type", "-of", "csv=p=0", str(rgb_files[0])],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
        if probe.returncode != 0 or "video" not in probe.stdout:
            raise RuntimeError(f"RGB recording is not readable by ffprobe: {rgb_files[0]}")
        metadata = json.loads(ready[0].read_text(encoding="utf-8"))
        quality_status = str(metadata.get("recording_quality_status") or "unknown")
        quality_passed = (
            metadata.get("ready") is True
            and metadata.get("recording_complete") is True
            and quality_status == "complete"
        )
        if not quality_passed:
            quality_failures.append(
                f"{directory}: {quality_status}: "
                + str(metadata.get("recording_quality_reason") or "completeness was not verified")
            )
        results.append(
            {
                "directory": str(directory),
                "frames_csv_lines": line_count,
                "rgb_bytes": rgb_files[0].stat().st_size,
                "depth_bytes": depth_files[0].stat().st_size,
                "recording_quality_status": quality_status,
                "quality_passed": quality_passed,
            }
        )
    return {
        "directories": results,
        "quality_passed": not quality_failures,
        "quality_failures": quality_failures,
    }


def cleanup_recording_session(config_path: Path, session_id: int) -> list[str]:
    removed: list[str] = []
    roots = recording_roots(config_path)
    for directory in recording_session_directories(config_path, session_id):
        root = next((item for item in roots if item in directory.parents), None)
        if root is None:
            continue
        shutil.rmtree(directory)
        removed.append(str(directory))
        parent = directory.parent
        while parent != root and root in parent.parents:
            try:
                parent.rmdir()
            except OSError:
                break
            parent = parent.parent
    return removed


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run an explicit short recording acceptance test against a GWV3 receiver"
    )
    parser.add_argument("--admin", default="http://127.0.0.1:18080")
    parser.add_argument("--record-seconds", type=float, default=60.0)
    parser.add_argument("--start-timeout", type=float, default=30.0)
    parser.add_argument("--finalize-timeout", type=float, default=900.0)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--cleanup-on-success", action="store_true")
    parser.add_argument(
        "--require-complete-recording",
        action="store_true",
        help="fail on partial or unverified recording quality, retaining test files",
    )
    args = parser.parse_args()
    if args.record_seconds <= 0:
        parser.error("--record-seconds must be positive")
    if args.require_complete_recording and args.config is None:
        parser.error("--require-complete-recording requires --config")

    started_by_this_process = False
    report = {
        "ok": False,
        "admin": args.admin,
        "record_seconds": args.record_seconds,
        "started_at_us": int(time.time() * 1_000_000),
        "require_complete_recording": args.require_complete_recording,
    }
    try:
        before = request_json(args.admin, "GET", "/api/status")
        if before.get("recording_state") != "idle" or before.get("recording_all"):
            raise RuntimeError("receiver is not idle; refusing to interfere with an existing recording")
        if before.get("recording_start_ready") is not True:
            raise RuntimeError(
                "receiver is not ready to record: "
                + str(before.get("recording_start_block_reason") or "unknown reason")
            )
        before_cameras = {
            key: camera
            for key, camera in camera_map(before).items()
            if camera.get("live") is True
        }
        if not before_cameras:
            raise RuntimeError("receiver has no live cameras")
        report["camera_keys"] = sorted(before_cameras)

        start_response = request_json(args.admin, "POST", "/api/record/start-all")
        if start_response.get("ok") is not True:
            raise RuntimeError(f"start-all was rejected: {start_response}")
        started_by_this_process = True
        recording_session_id = int(start_response.get("recording_session_id") or 0)
        report["recording_session_id"] = recording_session_id

        def all_started(status):
            require_same_session(status, recording_session_id)
            cameras = camera_map(status)
            return not status.get("recording_start_pending") and all(
                bool(cameras.get(key, {}).get("recording"))
                and not cameras.get(key, {}).get("recording_start_pending")
                for key in before_cameras
            )

        wait_status(args.admin, "all live cameras to start", all_started, args.start_timeout)
        time.sleep(args.record_seconds)

        require_same_session(request_json(args.admin, "GET", "/api/status"), recording_session_id)
        report["stop_requested_at_us"] = int(time.time() * 1_000_000)
        stop_started = time.monotonic()
        stop_response = request_json(args.admin, "POST", "/api/record/stop-all")
        if stop_response.get("ok") is not True:
            raise RuntimeError(f"stop-all was rejected: {stop_response}")
        started_by_this_process = False

        def finalized(status):
            require_same_session(status, recording_session_id)
            cameras = camera_map(status)
            # Idle only means new recording input stopped; tail drain can still
            # precede both finalizer registration and uploader discovery.
            cameras_completed = all(
                key in cameras
                and not cameras[key].get("record_tail_draining")
                and not cameras[key].get("record_finalizing")
                and int(cameras[key].get("segment_finalize_completed") or 0)
                > int(old.get("segment_finalize_completed") or 0)
                for key, old in before_cameras.items()
            )
            return (
                cameras_completed
                and status.get("recording_state") == "idle"
                and not status.get("recording_all")
                and int(status.get("record_finalize_outstanding_segments") or 0) == 0
                and int(status.get("record_queue_total_bytes") or 0) == 0
                and uploader_pending(status) == 0
            )

        after = wait_status(
            args.admin,
            "recording finalization and NAS delivery",
            finalized,
            args.finalize_timeout,
        )
        report["stop_to_delivery_status_ms"] = round((time.monotonic() - stop_started) * 1000, 3)
        after_cameras = camera_map(after)
        failures = []
        camera_results = {}
        for key, old in before_cameras.items():
            current = after_cameras.get(key)
            if current is None:
                failures.append(f"{key}: missing after recording")
                continue
            completed_delta = int(current.get("segment_finalize_completed") or 0) - int(
                old.get("segment_finalize_completed") or 0
            )
            write_error_delta = int(current.get("record_write_errors") or 0) - int(
                old.get("record_write_errors") or 0
            )
            camera_results[key] = {
                "segment_finalize_completed_delta": completed_delta,
                "record_write_errors_delta": write_error_delta,
                "live_after": bool(current.get("live")),
            }
            if completed_delta < 1:
                failures.append(f"{key}: no completed segment was recorded")
            if write_error_delta != 0:
                failures.append(f"{key}: record_write_errors increased by {write_error_delta}")
            if current.get("live") is not True:
                failures.append(f"{key}: camera is no longer live")
        report["camera_results"] = camera_results
        report["finished_at_us"] = int(time.time() * 1_000_000)
        report["failures"] = failures
        report["ok"] = not failures
        if failures:
            raise RuntimeError("; ".join(failures))
        if args.config is not None:
            report["recording_files"] = validate_recording_session(
                args.config, recording_session_id, len(before_cameras)
            )
            if args.require_complete_recording and not report["recording_files"]["quality_passed"]:
                raise RuntimeError("; ".join(report["recording_files"]["quality_failures"]))
        if args.cleanup_on_success:
            if args.config is None:
                raise RuntimeError("--cleanup-on-success requires --config")
            if recording_session_id <= 0:
                raise RuntimeError("receiver did not return a recording_session_id")
            report["cleanup_removed_directories"] = cleanup_recording_session(
                args.config, recording_session_id
            )
    except Exception as exc:
        report["ok"] = False
        report["error"] = str(exc)
        report["finished_at_us"] = int(time.time() * 1_000_000)
        if started_by_this_process:
            try:
                require_same_session(request_json(args.admin, "GET", "/api/status"), recording_session_id)
                request_json(args.admin, "POST", "/api/record/stop-all")
                report["emergency_stop_requested"] = True
            except Exception as stop_exc:
                report["emergency_stop_error"] = str(stop_exc)
        result = 1
    else:
        result = 0

    rendered = json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return result


if __name__ == "__main__":
    sys.exit(main())
