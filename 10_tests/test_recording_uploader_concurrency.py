#!/usr/bin/env python3
import argparse
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import threading
import time


def load_uploader(path: Path):
    spec = importlib.util.spec_from_file_location("recording_uploader_under_test", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(uploader_path: Path) -> None:
    uploader_module = load_uploader(uploader_path)
    with tempfile.TemporaryDirectory(prefix="gwv3_uploader_concurrency_") as temporary_text:
        temporary = Path(temporary_text)
        staging = temporary / "staging"
        nas = temporary / "nas"
        staging.mkdir()
        nas.mkdir()
        config_path = temporary / "receiver.json"
        config_path.write_text(
            json.dumps(
                {
                    "nas_root": str(nas),
                    "recording_staging": {
                        "enabled": True,
                        "root": str(staging),
                        "capture_workers": 16,
                        "full_copy_workers": 3,
                        "active_recording_full_copy_workers": 1,
                        "upload_bandwidth_limit_mbps": 240,
                        "full_copy_bandwidth_limit_mbps": 64,
                        "finalize_workers": 8,
                        "incremental_mirror_enabled": True,
                        "incremental_mirror_workers": 3,
                        "pause_during_receiver_finalize": False,
                    },
                }
            ),
            encoding="ascii",
        )
        uploader = uploader_module.Uploader(config_path)
        assert uploader.bandwidth_mbps == 240
        assert uploader.full_copy_bandwidth_mbps == 64
        assert uploader.full_copy_workers == 3
        assert uploader.active_recording_full_copy_workers == 1
        assert uploader.incremental_mirror_workers == 3
        assert uploader.incremental_mirror_max_copy_bytes == 16 * 1024 * 1024
        assert uploader.nas_write_limiter.bandwidth_mbps == 240
        uploader.receiver_recording_active = True
        uploader.active_recording_full_copy_workers = 0
        assert uploader.current_full_copy_limit() == 0
        uploader.receiver_recording_active = False
        assert uploader.current_full_copy_limit() == uploader.full_copy_workers
        uploader.active_recording_full_copy_workers = 1

        limiter = uploader_module.SharedBandwidthLimiter(32)
        limiter_threads = [
            threading.Thread(target=limiter.consume, args=(256 * 1024,))
            for _ in range(4)
        ]
        limiter_started = time.monotonic()
        for thread in limiter_threads:
            thread.start()
        for thread in limiter_threads:
            thread.join()
        limiter_elapsed = time.monotonic() - limiter_started
        assert 0.18 <= limiter_elapsed < 1.5
        limited_bytes, limited_rate = limiter.snapshot()
        assert limited_bytes == 1024 * 1024
        assert 1.0 <= limited_rate <= 40.0
        uploader.begin_priority_capture()
        uploader.begin_priority_capture()
        assert uploader.priority_capture_count == 2
        assert uploader.priority_capture_io_active.is_set()
        uploader.end_priority_capture()
        assert uploader.priority_capture_io_active.is_set()
        uploader.end_priority_capture()
        assert uploader.priority_capture_count == 0
        assert not uploader.priority_capture_io_active.is_set()

        available_bytes = uploader.staging_available_bytes()
        uploader.min_free_disk_bytes = available_bytes + 1
        uploader.emergency_free_disk_headroom_bytes = 0
        assert uploader.staging_disk_emergency() is True
        assert uploader.staging_disk_pressure() is True
        uploader.min_free_disk_bytes = 0
        assert uploader.staging_disk_emergency() is False

        original_disk_usage = uploader_module.shutil.disk_usage
        disk_state = {"used": 72, "free": 28}

        class DiskUsage:
            total = 100

            @property
            def used(self):
                return disk_state["used"]

            @property
            def free(self):
                return disk_state["free"]

        uploader_module.shutil.disk_usage = lambda _path: DiskUsage()
        try:
            uploader.staging_pressure_active = False
            disk_state.update(used=76, free=24)
            assert uploader.staging_disk_pressure() is True
            disk_state.update(used=72, free=28)
            assert uploader.staging_disk_pressure() is True
            disk_state.update(used=69, free=31)
            assert uploader.staging_disk_pressure() is False
        finally:
            uploader_module.shutil.disk_usage = original_disk_usage

        busy_priority_lock = threading.Lock()
        busy_priority_lock.acquire()
        original_mirror_lock = uploader.incremental_mirror_lock
        uploader.incremental_mirror_lock = lambda _directory: busy_priority_lock
        try:
            started = time.monotonic()
            assert uploader.local_segment_mirror_reuse_percent(staging) == 0.0
            assert time.monotonic() - started < 0.1
        finally:
            uploader.incremental_mirror_lock = original_mirror_lock
            busy_priority_lock.release()
        uploader.incremental_mirror_enabled = False
        uploader.incremental_mirror_lock = lambda _directory: (_ for _ in ()).throw(
            AssertionError("disabled incremental mirror attempted NAS state access")
        )
        assert uploader.local_segment_mirror_reuse_percent(staging) == 0.0
        uploader.incremental_mirror_lock = original_mirror_lock
        uploader.incremental_mirror_enabled = True
        process_lock_path = staging / uploader_module.UPLOADER_PROCESS_LOCK_NAME
        process_lock = uploader_module.acquire_file_lock(process_lock_path)
        assert process_lock is not None
        try:
            assert uploader.run(True) == 1
        finally:
            uploader_module.release_file_lock(process_lock_path, process_lock)
        original_discover_local_segments = uploader_module.discover_local_segments
        scan_observed = {}

        def delayed_discover_local_segments(root: Path):
            scan_observed["called_us"] = uploader_module.now_us()
            time.sleep(0.02)
            return original_discover_local_segments(root)

        uploader_module.discover_local_segments = delayed_discover_local_segments
        try:
            uploader.write_status(refresh_metrics=True)
        finally:
            uploader_module.discover_local_segments = original_discover_local_segments
        uploader_status = json.loads(uploader.status_path.read_text(encoding="utf-8"))
        assert int(uploader_status["pending_metrics_refreshed_us"]) > 0
        assert int(uploader_status["pending_metrics_refreshed_us"]) <= scan_observed["called_us"]

        active = 0
        peak = 0
        observed: dict[str, list[str]] = {}
        lock = threading.Lock()

        def fake_capture(segment: Path, _track_active: bool = True) -> bool:
            nonlocal active, peak
            with lock:
                active += 1
                peak = max(peak, active)
                observed.setdefault(segment.relative_to(staging).parts[0], []).append(
                    segment.name
                )
            time.sleep(0.1)
            with lock:
                active -= 1
            return True

        uploader.process_local_one = fake_capture
        uploader.local_segment_mirror_reuse_percent = lambda _segment: 95.0
        original_write_status = uploader.write_status
        # Status fsync latency must not serialize the worker-concurrency unit
        # test when it runs on a receiver whose system disk is busy.
        uploader.write_status = lambda *_args, **_kwargs: None
        segments = [
            staging / f"camera-{index}" / "2026-08-07" / "120000"
            for index in range(5)
        ]
        segments.append(staging / "camera-0" / "2026-08-07" / "121500")
        assert uploader.process_local_batch(segments) is True
        assert peak == 5, f"expected one capture worker per route, peak={peak}"
        assert observed["camera-0"] == ["120000"]
        assert uploader.process_local_batch([segments[-1]]) is True
        assert observed["camera-0"] == ["120000", "121500"]

        observed.clear()
        active_recording_peak = 0

        def fake_active_recording_capture(
            segment: Path,
            _track_active: bool = True,
        ) -> bool:
            nonlocal active, active_recording_peak
            with lock:
                active += 1
                active_recording_peak = max(active_recording_peak, active)
                observed.setdefault(segment.relative_to(staging).parts[0], []).append(
                    segment.name
                )
            time.sleep(0.05)
            with lock:
                active -= 1
            return True

        uploader.process_local_one = fake_active_recording_capture
        uploader.local_segment_mirror_reuse_percent = lambda _segment: 0.0
        uploader.receiver_recording_active = True
        uploader.staging_disk_pressure = lambda: False
        assert uploader.current_full_copy_limit() == 1
        assert uploader.process_local_batch(segments[:5]) is True
        assert active_recording_peak == 1

        active_recording_peak = 0
        observed.clear()
        uploader.staging_disk_pressure = lambda: True
        assert uploader.current_full_copy_limit() == 3
        assert uploader.process_local_batch(segments[:5]) is True
        assert active_recording_peak == 3
        uploader.receiver_recording_active = False
        uploader.process_local_one = fake_capture

        observed.clear()
        priority_segments = [
            staging / "camera-priority" / "2026-08-07" / "old-unmirrored",
            staging / "camera-priority" / "2026-08-07" / "new-mirrored",
        ]
        uploader.local_segment_mirror_reuse_percent = (
            lambda segment: 50.0 if segment.name == "new-mirrored" else 0.0
        )
        assert uploader.process_local_batch(priority_segments) is True
        assert observed["camera-priority"] == ["new-mirrored"]
        assert uploader.process_local_batch(priority_segments[:1]) is True
        assert observed["camera-priority"] == ["new-mirrored", "old-unmirrored"]
        uploader.write_status = original_write_status

        chunk_size = 1024 * 1024
        budget_source = temporary / "budget-source.bin"
        budget_destination = temporary / "budget-destination.bin"
        budget_source.write_bytes(b"B" * (8 * chunk_size))
        dd_path = uploader_module.shutil.which("dd")
        if dd_path:
            direct_source = temporary / "direct-source.bin"
            direct_destination = temporary / "direct-destination.bin"
            direct_source.write_bytes(b"D" * (8 * chunk_size + 123))
            direct_progress: list[tuple[int, int]] = []
            direct_limiter = uploader_module.SharedBandwidthLimiter(0)
            uploader_module.copy_file_direct_dd(
                direct_source,
                direct_destination,
                dd_path,
                direct_source.stat().st_size,
                lambda done, total: direct_progress.append((done, total)),
                lambda: False,
                direct_limiter,
            )
            assert direct_destination.read_bytes() == direct_source.read_bytes()
            assert direct_progress[-1] == (
                direct_source.stat().st_size,
                direct_source.stat().st_size,
            )
            assert direct_limiter.snapshot()[0] == direct_source.stat().st_size
        budget_entry, budget_copied = uploader_module.mirror_complete_file_chunks(
            budget_source,
            budget_destination,
            None,
            chunk_size,
            0,
            0,
            max_copy_bytes=2 * chunk_size,
        )
        assert budget_copied == 2 * chunk_size
        assert budget_entry["mirrored_bytes"] == 2 * chunk_size
        budget_entry, budget_copied = uploader_module.mirror_complete_file_chunks(
            budget_source,
            budget_destination,
            budget_entry,
            chunk_size,
            0,
            0,
            max_copy_bytes=2 * chunk_size,
        )
        assert budget_copied == 2 * chunk_size
        assert budget_entry["mirrored_bytes"] == 4 * chunk_size

        takeover_capture = {
            "sender_id": "restart-test",
            "camera_id": "cam01",
            "segment_start_us": 123456,
            "relative_path": "restart-test_cam01/2026-08-07/120000",
        }
        takeover_hashes = [str(index) * 64 for index in range(4)]
        takeover_state = {
            "schema": "gwv3_incremental_mirror_v1",
            "instance_id": "99999999-old",
            "identity": uploader_module.incremental_mirror_identity(takeover_capture),
            "files": {
                "rgb.mp4": {
                    "chunk_size": chunk_size,
                    "chunk_hashes": takeover_hashes,
                    "durable_chunks": 2,
                    "mirrored_bytes": 4 * chunk_size,
                }
            },
        }
        resumed = uploader_module.normalize_incremental_mirror_state(
            takeover_state,
            takeover_capture,
            "12345678-new",
        )
        resumed_entry = resumed["files"]["rgb.mp4"]
        assert resumed["instance_id"] == "12345678-new"
        assert resumed_entry["chunk_hashes"] == takeover_hashes[:2]
        assert resumed_entry["durable_chunks"] == 2
        assert resumed_entry["mirrored_bytes"] == 2 * chunk_size

        active_owner_state = dict(takeover_state)
        active_owner_state["instance_id"] = f"{os.getpid()}-active"
        try:
            uploader_module.normalize_incremental_mirror_state(
                active_owner_state,
                takeover_capture,
                "12345678-new",
            )
        except uploader_module.IncrementalMirrorOwnedError:
            pass
        else:
            raise AssertionError("a running uploader mirror owner was taken over")

        reuse_source = temporary / "restart-segment"
        reuse_source.mkdir()
        reuse_file = reuse_source / "rgb.mp4"
        reuse_file.write_bytes(b"R" * (4 * chunk_size))
        reuse_stat = reuse_file.stat()
        reuse_directory = uploader_module.incremental_mirror_directory(
            uploader.capture_queue_root,
            takeover_capture,
        )
        reuse_directory.mkdir(parents=True)
        (reuse_directory / "rgb.mp4").write_bytes(reuse_file.read_bytes())
        takeover_state["files"]["rgb.mp4"].update(
            {
                "source_name": "rgb.mp4",
                "source_dev": int(reuse_stat.st_dev),
                "source_ino": int(reuse_stat.st_ino),
            }
        )
        uploader_module.write_incremental_mirror_state(
            reuse_directory,
            takeover_state,
        )
        reusable_bytes, total_bytes = uploader_module.incremental_mirror_reuse_bytes(
            reuse_source,
            uploader.capture_queue_root,
            takeover_capture,
            "12345678-new",
        )
        assert reusable_bytes == 2 * chunk_size
        assert total_bytes == 4 * chunk_size

        source = temporary / "source.bin"
        destination = temporary / "destination.bin"
        chunks = [bytes([index]) * chunk_size for index in range(8)]
        source.write_bytes(b"".join(chunks))
        destination.write_bytes(source.read_bytes())
        source_stat = source.stat()
        entry = {
            "source_name": source.name,
            "source_dev": int(source_stat.st_dev),
            "source_ino": int(source_stat.st_ino),
            "chunk_size": chunk_size,
            "chunk_hashes": [uploader_module.hashlib.sha256(chunk).hexdigest() for chunk in chunks],
        }
        tail = b"closed-tail" * 4096
        with source.open("ab") as handle:
            handle.write(tail)
        fast_stats: dict[str, int | bool] = {}
        written = uploader_module.synchronize_file_from_incremental_mirror(
            source,
            destination,
            entry,
            0,
            stats=fast_stats,
        )
        assert written == len(tail)
        assert fast_stats["fast_path"] is True
        assert int(fast_stats["verified_bytes"]) == 4 * chunk_size
        assert destination.read_bytes() == source.read_bytes()

        with source.open("r+b") as handle:
            handle.write(b"container-header-updated")
        fallback_stats: dict[str, int | bool] = {}
        uploader_module.synchronize_file_from_incremental_mirror(
            source,
            destination,
            entry,
            0,
            stats=fallback_stats,
        )
        assert fallback_stats["fast_path"] is False
        assert int(fallback_stats["verified_bytes"]) == 8 * chunk_size
        assert destination.read_bytes() == source.read_bytes()

        depth_source = temporary / "depth.mkv"
        depth_destination = temporary / "depth-mirror.mkv"
        depth_chunks = [bytes([index + 16]) * chunk_size for index in range(8)]
        depth_source.write_bytes(b"".join(depth_chunks))
        depth_destination.write_bytes(depth_source.read_bytes())
        depth_stat = depth_source.stat()
        depth_entry = {
            "source_name": depth_source.name,
            "source_dev": int(depth_stat.st_dev),
            "source_ino": int(depth_stat.st_ino),
            "chunk_size": chunk_size,
            "chunk_hashes": [
                uploader_module.hashlib.sha256(chunk).hexdigest()
                for chunk in depth_chunks
            ],
        }
        depth_tail = b"depth-closed-tail" * 4096
        with depth_source.open("r+b") as handle:
            handle.write(b"matroska-header-updated")
            handle.seek(0, 2)
            handle.write(depth_tail)
        depth_stats: dict[str, int | bool] = {}
        depth_written = uploader_module.synchronize_file_from_incremental_mirror(
            depth_source,
            depth_destination,
            depth_entry,
            0,
            stats=depth_stats,
        )
        assert depth_stats["fast_path"] is True
        assert depth_stats["mutable_header_repaired"] is True
        assert int(depth_stats["verified_bytes"]) == 4 * chunk_size
        assert depth_written == chunk_size + len(depth_tail)
        assert depth_destination.read_bytes() == depth_source.read_bytes()

        mirror_worker_calls = 0
        mirror_worker_lock = threading.Lock()

        def fake_active_mirror_scan() -> bool:
            nonlocal mirror_worker_calls
            with mirror_worker_lock:
                mirror_worker_calls += 1
            time.sleep(0.02)
            return True

        uploader.process_active_mirrors = fake_active_mirror_scan
        uploader.start_incremental_mirror_worker()
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            with mirror_worker_lock:
                if mirror_worker_calls >= 2:
                    break
            time.sleep(0.02)
        uploader.stop_incremental_mirror_worker()
        with mirror_worker_lock:
            assert mirror_worker_calls >= 2
        assert uploader.incremental_mirror_worker_running is False
        assert uploader.incremental_mirror_worker_thread is None

        mirror_active = 0
        mirror_peak = 0
        mirror_routes: list[str] = []
        mirror_lock = threading.Lock()

        def fake_process_active_mirror(
            segment: Path,
            _capture: dict,
            _sources: dict,
        ) -> bool:
            nonlocal mirror_active, mirror_peak
            with mirror_lock:
                mirror_active += 1
                mirror_peak = max(mirror_peak, mirror_active)
                mirror_routes.append(segment.name)
            time.sleep(0.05)
            with mirror_lock:
                mirror_active -= 1
            return True

        active_segments = [
            (
                staging / f"mirror-camera-{index}" / "2026-08-10" / f"segment-{index}",
                {"sender_id": f"sender-{index}", "camera_id": "cam01"},
                {},
            )
            for index in range(5)
        ]
        original_discover_active = uploader_module.discover_active_segments
        uploader_module.discover_active_segments = lambda _root: active_segments
        uploader.process_active_mirror = fake_process_active_mirror
        uploader.incremental_mirror_worker_stop.clear()
        uploader.next_incremental_mirror_at = 0.0
        try:
            assert uploader_module.Uploader.process_active_mirrors(uploader) is True
            uploader.next_incremental_mirror_at = 0.0
            assert uploader_module.Uploader.process_active_mirrors(uploader) is True
        finally:
            uploader_module.discover_active_segments = original_discover_active
        assert mirror_peak == 3
        assert set(mirror_routes) == {
            f"segment-{index}" for index in range(5)
        }

    print("recording uploader concurrency and incremental fast-path test passed")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--uploader", required=True, type=Path)
    return parser.parse_args()


if __name__ == "__main__":
    run(parse_args().uploader)
