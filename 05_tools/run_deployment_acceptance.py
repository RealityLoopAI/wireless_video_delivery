#!/usr/bin/env python3
import argparse
import json
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


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run an explicit short recording acceptance test against a GWV3 receiver"
    )
    parser.add_argument("--admin", default="http://127.0.0.1:18080")
    parser.add_argument("--record-seconds", type=float, default=60.0)
    parser.add_argument("--start-timeout", type=float, default=30.0)
    parser.add_argument("--finalize-timeout", type=float, default=900.0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.record_seconds <= 0:
        parser.error("--record-seconds must be positive")

    started_by_this_process = False
    report = {
        "ok": False,
        "admin": args.admin,
        "record_seconds": args.record_seconds,
        "started_at_us": int(time.time() * 1_000_000),
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

        def all_started(status):
            cameras = camera_map(status)
            return all(bool(cameras.get(key, {}).get("recording")) for key in before_cameras)

        wait_status(args.admin, "all live cameras to start", all_started, args.start_timeout)
        time.sleep(args.record_seconds)

        stop_response = request_json(args.admin, "POST", "/api/record/stop-all")
        if stop_response.get("ok") is not True:
            raise RuntimeError(f"stop-all was rejected: {stop_response}")
        started_by_this_process = False

        def finalized(status):
            return (
                status.get("recording_state") == "idle"
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
    except Exception as exc:
        report["error"] = str(exc)
        report["finished_at_us"] = int(time.time() * 1_000_000)
        if started_by_this_process:
            try:
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
