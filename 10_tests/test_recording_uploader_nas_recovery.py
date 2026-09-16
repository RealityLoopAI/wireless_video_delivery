#!/usr/bin/env python3
"""NAS status and capacity recovery using only temporary local directories."""

import argparse
import errno
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
from unittest import mock


def load_uploader(path: Path):
    spec = importlib.util.spec_from_file_location("gwv3_uploader_nas_recovery", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_fixture(module, root: Path):
    staging = root / "staging"
    nas = root / "nas"
    nas.mkdir()
    segment = staging / "camera-a" / "2026-09-16" / "120000"
    segment.mkdir(parents=True)
    ffmpeg = shutil.which("ffmpeg")
    assert ffmpeg is not None, "NAS recovery integration test requires ffmpeg"
    subprocess.run([
        ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi",
        "-i", "color=c=black:s=64x48:r=5", "-frames:v", "5",
        "-c:v", "libx264", "-threads", "1",
        "-movflags", "+frag_keyframe+empty_moov+default_base_moof",
        str(segment / "rgb.mp4"),
    ], check=True, timeout=30, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    (segment / "frames.csv").write_text(
        "frame_index,timestamp_us\n" + "".join(f"{index},{index * 200000}\n" for index in range(5))
    )
    (segment / "meta.json").write_text(json.dumps({"closed": True}))
    (segment / "recording_staged.json").write_text(json.dumps({
        "staged": True,
        "sender_id": "sender-a",
        "camera_id": "cam01",
        "segment_start_us": 1,
        "relative_path": "camera-a/2026-09-16/120000",
    }))
    status_path = root / "nas-status.json"
    config_path = root / "receiver.json"
    config_path.write_text(json.dumps({
        "nas_root": str(nas),
        "nas_min_free_mb": 1,
        "nas_auto_mount": {
            "enabled": True,
            "status_path": str(status_path),
            "status_max_age_ms": 10000,
        },
        "recording_staging": {
            "enabled": True,
            "root": str(staging),
            "upload_interval_ms": 250,
            "delete_after_upload": True,
            "retain_local_capture_for_finalize": True,
            "local_cache_high_watermark_percent": 0,
            "incremental_mirror_enabled": False,
            "pause_during_receiver_finalize": False,
            "rgb_output_mode": "fragmented_mp4",
        },
    }))
    uploader = module.Uploader(config_path)
    original_files = {path.name: path.read_bytes() for path in segment.iterdir()}
    return uploader, segment, status_path, original_files


def write_snapshot(module, uploader, path: Path, **overrides):
    snapshot = {
        "ready": True,
        "mount_point": str(uploader.nas_root),
        "updated_us": module.now_us(),
    }
    snapshot.update(overrides)
    path.write_text(json.dumps(snapshot))


def assert_retained(module, uploader, segment: Path, original_files):
    assert segment.is_dir(), "NAS outage removed the local pending recording"
    assert {path.name: path.read_bytes() for path in segment.iterdir()} == original_files
    assert module.discover_local_segments(uploader.staging_root) == [segment]
    assert not module.discover_capture_segments(uploader.capture_queue_root)
    assert uploader.captured == 0
    assert uploader.completed == 0


def assert_published(module, uploader, segment: Path, original_files):
    destination = uploader.nas_root / segment.relative_to(uploader.staging_root)
    assert not segment.exists(), "recovered publication did not release its local cache"
    for name in ("rgb.mp4", "frames.csv", "meta.json"):
        assert (destination / name).read_bytes() == original_files[name]
    assert json.loads((destination / "recording_ready.json").read_text())["ready"] is True
    assert not module.discover_local_segments(uploader.staging_root)
    assert not module.discover_capture_segments(uploader.capture_queue_root)
    assert not module.discover_publish_journals(uploader.capture_queue_root)
    assert uploader.captured == 1
    assert uploader.completed == 1
    assert uploader.last_error == ""


def test_snapshot_pause_and_recovery(module):
    with tempfile.TemporaryDirectory(prefix="gwv3_snapshot_recovery_") as temporary:
        uploader, segment, status_path, original_files = make_fixture(module, Path(temporary))
        # Capacity is tested separately; snapshot behavior must not depend on
        # free space or staging pressure on the host running the test.
        uploader.nas_min_free_bytes = 0
        for snapshot in ("missing", "unready", "stale", "malformed", "wrong_mount"):
            if snapshot == "unready":
                write_snapshot(module, uploader, status_path, ready=False)
            elif snapshot == "stale":
                write_snapshot(module, uploader, status_path, updated_us=1)
            elif snapshot == "malformed":
                status_path.write_text("{invalid json")
            elif snapshot == "wrong_mount":
                write_snapshot(module, uploader, status_path, mount_point=str(Path(temporary) / "other"))
            assert uploader.run_once() is False, snapshot
            assert_retained(module, uploader, segment, original_files)
            status = json.loads(uploader.status_path.read_text())
            assert status["local_pending_segments"] == 1, snapshot
            assert status["nas_mount_ready"] is False, snapshot

        write_snapshot(module, uploader, status_path)
        assert uploader.run_once() is True
        assert_published(module, uploader, segment, original_files)
        print("same-uploader NAS snapshot pause/recovery passed")


def test_midpass_capacity_failure_keeps_daemon_alive(module):
    with tempfile.TemporaryDirectory(prefix="gwv3_midpass_recovery_") as temporary:
        uploader, segment, status_path, original_files = make_fixture(module, Path(temporary))
        write_snapshot(module, uploader, status_path)
        original_run_once = uploader.run_once
        original_recover = uploader.recover_pending_publications
        state = {"passes": 0, "unavailable": False, "failed_capacity_checks": 0}

        def disk_usage(path):
            if Path(path) == uploader.nas_root and state["unavailable"]:
                state["failed_capacity_checks"] += 1
                return SimpleNamespace(total=1 << 40, used=1 << 40, free=0)
            return SimpleNamespace(total=1 << 40, used=0, free=1 << 40)

        def recover_then_lose_capacity():
            result = original_recover()
            if state["passes"] == 1:
                # The pass has already accepted a healthy mount snapshot.
                state["unavailable"] = True
            return result

        def observed_pass():
            state["passes"] += 1
            if state["passes"] == 2:
                assert_retained(module, uploader, segment, original_files)
                state["unavailable"] = False
                write_snapshot(module, uploader, status_path)
            try:
                return original_run_once()
            finally:
                # Bound the daemon test even if recovery makes no progress.
                if state["passes"] >= 2:
                    module.STOP_REQUESTED = True

        previous_stop = module.STOP_REQUESTED
        module.STOP_REQUESTED = False
        try:
            with mock.patch.object(module.shutil, "disk_usage", side_effect=disk_usage), \
                    mock.patch.object(uploader, "recover_pending_publications", side_effect=recover_then_lose_capacity), \
                    mock.patch.object(uploader, "run_once", side_effect=observed_pass):
                try:
                    result = uploader.run_locked(False)
                except OSError as error:
                    assert error.errno == errno.ENOSPC
                    assert_retained(module, uploader, segment, original_files)
                    raise AssertionError(
                        "mid-pass NAS capacity loss exited the uploader daemon instead of deferring and recovering"
                    ) from error
        finally:
            module.STOP_REQUESTED = previous_stop
        assert result == 0
        assert state["passes"] == 2
        assert state["failed_capacity_checks"] > 0
        assert_published(module, uploader, segment, original_files)
        print("mid-pass NAS capacity loss retained files and daemon recovered")


def test_unexpected_error_propagates(module):
    with tempfile.TemporaryDirectory(prefix="gwv3_unexpected_error_") as temporary:
        uploader, segment, status_path, original_files = make_fixture(module, Path(temporary))
        uploader.nas_min_free_bytes = 0
        write_snapshot(module, uploader, status_path)
        unexpected = ValueError("simulated non-storage programming error")

        def fail_receiver_check(*_args, **_kwargs):
            # Bound the test if an overly broad handler swallows this error.
            module.STOP_REQUESTED = True
            raise unexpected

        previous_stop = module.STOP_REQUESTED
        module.STOP_REQUESTED = False
        try:
            with mock.patch.object(uploader, "should_pause_for_receiver_io", side_effect=fail_receiver_check):
                try:
                    uploader.run_locked(False)
                except ValueError as error:
                    assert error is unexpected
                else:
                    raise AssertionError("uploader swallowed an unexpected non-storage error")
        finally:
            module.STOP_REQUESTED = previous_stop
        assert_retained(module, uploader, segment, original_files)
        print("unexpected non-storage errors propagate")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--uploader", required=True, type=Path)
    args = parser.parse_args()
    uploader_module = load_uploader(args.uploader)
    test_snapshot_pause_and_recovery(uploader_module)
    test_unexpected_error_propagates(uploader_module)
    test_midpass_capacity_failure_keeps_daemon_alive(uploader_module)
