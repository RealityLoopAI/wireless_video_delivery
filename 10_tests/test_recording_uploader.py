#!/usr/bin/env python3
import argparse
import errno
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
from types import SimpleNamespace


def atoms(path: Path) -> set[bytes]:
    result: set[bytes] = set()
    size = path.stat().st_size
    offset = 0
    with path.open("rb") as handle:
        while offset + 8 <= size:
            handle.seek(offset)
            header = handle.read(8)
            assert len(header) == 8
            atom_size = int.from_bytes(header[:4], "big")
            atom_type = header[4:8]
            header_size = 8
            if atom_size == 1:
                atom_size = int.from_bytes(handle.read(8), "big")
                header_size = 16
            elif atom_size == 0:
                atom_size = size - offset
            assert header_size <= atom_size <= size - offset
            result.add(atom_type)
            offset += atom_size
    assert offset == size
    return result


def remove_closing_mfra(path: Path) -> None:
    size = path.stat().st_size
    offset = 0
    with path.open("r+b") as handle:
        while offset + 8 <= size:
            handle.seek(offset)
            header = handle.read(8)
            assert len(header) == 8
            atom_size = int.from_bytes(header[:4], "big")
            atom_type = header[4:8]
            header_size = 8
            if atom_size == 1:
                atom_size = int.from_bytes(handle.read(8), "big")
                header_size = 16
            elif atom_size == 0:
                atom_size = size - offset
            assert header_size <= atom_size <= size - offset
            if atom_type == b"mfra":
                assert offset + atom_size == size
                handle.truncate(offset)
                return
            offset += atom_size
    raise AssertionError("fixture has no closing mfra atom")


def run(args: argparse.Namespace) -> None:
    ffmpeg = shutil.which("ffmpeg")
    ffprobe = shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        print("recording uploader fault-injection test skipped: ffmpeg/ffprobe unavailable")
        return

    with tempfile.TemporaryDirectory(prefix="gwv3_uploader_test_") as temporary_text:
        temporary = Path(temporary_text)
        staging_root = temporary / "staging"
        nas_root = temporary / "nas"
        segment = staging_root / "camera-a" / "2026-07-21" / "120000"
        segment.mkdir(parents=True)
        prefix = "test_Short_0048_"
        rgb_path = segment / f"{prefix}rgb.mp4"
        generated = subprocess.run(
            [
                ffmpeg,
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "lavfi",
                "-i",
                "testsrc2=size=128x80:rate=30",
                "-frames:v",
                "90",
                "-c:v",
                "libx264",
                "-preset",
                "ultrafast",
                "-movflags",
                "+frag_keyframe+empty_moov+default_base_moof",
                str(rgb_path),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
            check=False,
        )
        assert generated.returncode == 0, generated.stderr.decode(errors="replace")
        assert b"moof" in atoms(rgb_path)
        assert b"mfra" in atoms(rgb_path)
        assert b"sidx" not in atoms(rgb_path)
        invalid_segment = temporary / "invalid_mfra"
        invalid_segment.mkdir()
        invalid_rgb = invalid_segment / "rgb.mp4"
        shutil.copy2(rgb_path, invalid_rgb)
        remove_closing_mfra(invalid_rgb)
        assert b"mfra" not in atoms(invalid_rgb)
        spec = importlib.util.spec_from_file_location("recording_uploader_under_test", args.uploader)
        assert spec is not None and spec.loader is not None
        uploader_module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(uploader_module)
        quality_source = {
            "recording_quality_status": "partial",
            "recording_complete": False,
            "recording_quality_reason": "rgb tail is missing",
            "rgb_coverage_ratio": 0.75,
        }
        quality_ready = uploader_module.build_ready_marker(
            quality_source,
            uploader_module.RGB_OUTPUT_FRAGMENTED_MP4,
        )
        assert quality_ready["recording_quality_status"] == "partial"
        assert quality_ready["recording_complete"] is False
        assert quality_ready["rgb_coverage_ratio"] == 0.75

        incremental_staging = temporary / "incremental-staging"
        incremental_nas = temporary / "incremental-nas"
        incremental_segment = (
            incremental_staging
            / "camera-incremental"
            / "2026-07-30"
            / "120000"
        )
        incremental_segment.mkdir(parents=True)
        incremental_nas.mkdir()
        incremental_prefix = "incremental_"
        incremental_rgb = incremental_segment / f"{incremental_prefix}rgb.mp4"
        incremental_depth_part = (
            incremental_segment / f"{incremental_prefix}depth_part_000.mkv"
        )
        incremental_rgb.write_bytes(
            b"R" * (3 * 1024 * 1024) + b"rgb-active-tail"
        )
        incremental_depth_part.write_bytes(
            b"D" * (2 * 1024 * 1024) + b"depth-active-tail"
        )
        incremental_relative = "camera-incremental/2026-07-30/120000"
        incremental_meta = {
            "closed": False,
            "sender_id": "sender-incremental",
            "camera_id": "cam01",
            "segment_start_us": 1785384000000000,
            "recording_relative_path": incremental_relative,
            "file_prefix": incremental_prefix,
            "rgb_file": f"{incremental_prefix}rgb.mp4",
            "depth_file": f"{incremental_prefix}depth.mkv",
        }
        incremental_meta_path = (
            incremental_segment / f"{incremental_prefix}meta.json"
        )
        incremental_meta_path.write_text(
            json.dumps(incremental_meta),
            encoding="ascii",
        )
        incremental_config_path = temporary / "incremental-receiver.json"
        incremental_config_path.write_text(
            json.dumps(
                {
                    "nas_root": str(incremental_nas),
                    "recording_staging": {
                        "enabled": True,
                        "root": str(incremental_staging),
                        "incremental_mirror_enabled": True,
                        "incremental_mirror_interval_ms": 250,
                        "incremental_mirror_chunk_mb": 1,
                        "incremental_mirror_lag_mb": 0,
                        "pause_during_receiver_finalize": False,
                    },
                }
            ),
            encoding="ascii",
        )
        incremental_uploader = uploader_module.Uploader(
            incremental_config_path
        )
        assert incremental_uploader.process_active_mirrors() is True
        descriptor = uploader_module.active_segment_descriptor(
            incremental_segment,
            incremental_staging,
        )
        assert descriptor is not None
        incremental_capture = descriptor[0]
        mirror_directory = uploader_module.incremental_mirror_directory(
            incremental_nas / ".gwv3_capture_queue",
            incremental_capture,
        )
        mirror_state = json.loads(
            (
                mirror_directory
                / uploader_module.INCREMENTAL_MIRROR_STATE_NAME
            ).read_text(encoding="utf-8")
        )
        assert mirror_state["files"][f"{incremental_prefix}rgb.mp4"][
            "mirrored_bytes"
        ] == 3 * 1024 * 1024
        assert mirror_state["files"][f"{incremental_prefix}depth.mkv"][
            "mirrored_bytes"
        ] == 2 * 1024 * 1024
        assert incremental_uploader.incremental_mirror_active_source_bytes == (
            incremental_rgb.stat().st_size + incremental_depth_part.stat().st_size
        )
        assert incremental_uploader.incremental_mirror_active_reusable_bytes == (
            5 * 1024 * 1024
        )
        assert incremental_uploader.incremental_mirror_active_lag_bytes == (
            len(b"rgb-active-tail") + len(b"depth-active-tail")
        )

        with incremental_rgb.open("r+b") as handle:
            handle.write(b"RGB-CLOSED")
            handle.seek(0, 2)
            handle.write(b"-rgb-final-tail")
        incremental_depth = (
            incremental_segment / f"{incremental_prefix}depth.mkv"
        )
        incremental_depth_part.replace(incremental_depth)
        with incremental_depth.open("r+b") as handle:
            handle.write(b"DEPTH-CLOSED")
            handle.seek(0, 2)
            handle.write(b"-depth-final-tail")
        incremental_frames = (
            incremental_segment / f"{incremental_prefix}frames.csv"
        )
        incremental_frames.write_text(
            "local_time_us,stream_type\n1,rgb\n",
            encoding="ascii",
        )
        incremental_meta["closed"] = True
        incremental_meta_path.write_text(
            json.dumps(incremental_meta),
            encoding="ascii",
        )
        incremental_capture.update(
            {
                "schema": "gwv3_recording_capture_ready_v1",
                "capture_ready": True,
                "segment_end_us": 1785384015000000,
                "recording_session_id": 1785384000000000,
                "recording_window_start_global_us": 1785384000000000,
                "recording_window_end_global_us": 1785384015000000,
                "frames_file": incremental_frames.name,
                "meta_file": incremental_meta_path.name,
                "ready_file": f"{incremental_prefix}recording_ready.json",
                "capture_file": (
                    f"{incremental_prefix}recording_capture_ready.json"
                ),
            }
        )
        incremental_marker = (
            incremental_segment
            / f"{incremental_prefix}recording_staged.json"
        )
        incremental_marker.write_text(
            json.dumps(
                {
                    **incremental_capture,
                    "schema": "gwv3_recording_staged_v1",
                    "staged": True,
                }
            ),
            encoding="ascii",
        )
        original_copy_file_limited = uploader_module.copy_file_limited

        def reject_incremental_media_recopy(
            source: Path,
            *copy_args: object,
            **copy_kwargs: object,
        ) -> None:
            assert source.name not in {
                incremental_rgb.name,
                incremental_depth.name,
            }, f"incrementally mirrored media was fully recopied: {source}"
            original_copy_file_limited(source, *copy_args, **copy_kwargs)

        uploader_module.copy_file_limited = reject_incremental_media_recopy
        try:
            incremental_destination, incremental_already_captured = (
                uploader_module.publish_capture_segment(
                    incremental_segment,
                    incremental_nas / ".gwv3_capture_queue",
                    incremental_capture,
                    str(incremental_capture["capture_file"]),
                    0,
                    incremental_mirror_instance_id=(
                        incremental_uploader.incremental_mirror_instance_id
                    ),
                )
            )
        finally:
            uploader_module.copy_file_limited = original_copy_file_limited
        assert incremental_already_captured is False
        assert (
            incremental_destination / incremental_rgb.name
        ).read_bytes() == incremental_rgb.read_bytes()
        assert (
            incremental_destination / incremental_depth.name
        ).read_bytes() == incremental_depth.read_bytes()
        assert not (
            incremental_destination
            / uploader_module.INCREMENTAL_MIRROR_STATE_NAME
        ).exists()

        pause_now_us = 1_800_000_000_000_000
        active_camera = {
            "recording": True,
            "segment_active": True,
            "segment_start_us": pause_now_us - 840_000_000,
        }
        assert (
            uploader_module.receiver_pause_phase(
                {"record_finalize_outstanding_segments": 1, "cameras": []},
                pause_now_us,
                900,
                60000,
            )
            == "receiver_finalize_wait"
        )
        assert (
            uploader_module.receiver_pause_phase(
                {"record_finalize_outstanding_segments": 0, "cameras": [active_camera]},
                pause_now_us,
                900,
                60000,
            )
            == "receiver_segment_boundary_wait"
        )
        active_camera["segment_start_us"] = pause_now_us - 800_000_000
        assert (
            uploader_module.receiver_pause_phase(
                {"record_finalize_outstanding_segments": 0, "cameras": [active_camera]},
                pause_now_us,
                900,
                60000,
            )
            == ""
        )
        assert (
            uploader_module.receiver_pause_phase(
                {"record_queue_total_bytes": 8 * 1024 * 1024, "cameras": []},
                pause_now_us,
                900,
                10000,
                8 * 1024 * 1024,
                500,
            )
            == "receiver_record_pressure_wait"
        )
        assert (
            uploader_module.receiver_pause_phase(
                {"record_queue_total_bytes": 0, "cameras": [{"record_queue_oldest_age_ms": 500}]},
                pause_now_us,
                900,
                10000,
                8 * 1024 * 1024,
                500,
            )
            == "receiver_record_pressure_wait"
        )
        phase_guard = object.__new__(uploader_module.Uploader)
        phase_guard.receiver_pause_phase = "receiver_segment_boundary_wait"
        phase_guard.active_phase = "receiver_segment_boundary_wait"
        phase_guard.active_bytes_done = 0
        phase_guard.active_bytes_total = 0
        phase_guard.next_active_status_at = float("inf")
        phase_guard.status_write_interval = 1.0
        phase_guard.status_lock = threading.RLock()
        phase_guard.update_active_status(
            phase="uploading",
            bytes_done=4096,
            bytes_total=8192,
        )
        assert phase_guard.active_phase == "receiver_segment_boundary_wait"
        assert phase_guard.active_bytes_done == 4096
        assert phase_guard.active_bytes_total == 8192
        pause_guard = object.__new__(uploader_module.Uploader)
        pause_guard.pause_during_receiver_finalize = True
        pause_guard.next_receiver_status_at = 0.0
        pause_guard.receiver_admin_url = "http://127.0.0.1:1/api/status"
        pause_guard.segment_seconds = 900
        pause_guard.quiet_before_segment_finalize_ms = 60000
        pause_guard.pause_record_queue_bytes = 8 * 1024 * 1024
        pause_guard.pause_record_queue_oldest_age_ms = 500
        pause_guard.receiver_pause_phase = "receiver_segment_boundary_wait"
        pause_guard.active_phase = "receiver_segment_boundary_wait"
        pause_guard.receiver_status_lock = threading.Lock()
        pause_status_updates: list[dict[str, object]] = []
        pause_guard.update_active_status = lambda **values: pause_status_updates.append(values)
        original_urlopen = uploader_module.urllib.request.urlopen

        def failing_receiver_status(*_args: object, **_kwargs: object) -> None:
            raise OSError(errno.ETIMEDOUT, "simulated receiver Admin timeout")

        uploader_module.urllib.request.urlopen = failing_receiver_status
        try:
            assert pause_guard.should_pause_for_receiver_io("uploading") is True
        finally:
            uploader_module.urllib.request.urlopen = original_urlopen
        assert pause_guard.receiver_pause_phase == "receiver_segment_boundary_wait"
        assert pause_status_updates[-1]["phase"] == "receiver_segment_boundary_wait"
        retry_attempts = 0

        def transient_operation() -> str:
            nonlocal retry_attempts
            retry_attempts += 1
            if retry_attempts < 3:
                raise OSError(errno.EFAULT, "simulated transient CIFS failure")
            return "recovered"

        assert (
            uploader_module.run_with_transient_nas_retries(
                transient_operation, max_attempts=3, initial_delay_seconds=0
            )
            == "recovered"
        )
        assert retry_attempts == 3
        permanent_attempts = 0

        def permanent_operation() -> None:
            nonlocal permanent_attempts
            permanent_attempts += 1
            raise OSError(errno.EACCES, "simulated permanent permission failure")

        try:
            uploader_module.run_with_transient_nas_retries(
                permanent_operation, max_attempts=3, initial_delay_seconds=0
            )
        except OSError as error:
            assert error.errno == errno.EACCES
        else:
            raise AssertionError("permanent NAS error was retried or ignored")
        assert permanent_attempts == 1
        metadata_source = temporary / "metadata_source.bin"
        metadata_destination = temporary / "metadata_destination.bin"
        metadata_source.write_bytes(b"metadata-copy-test")
        original_copystat = uploader_module.shutil.copystat
        pause_checks = 0

        def failing_copystat(*_args: object, **_kwargs: object) -> None:
            raise OSError(errno.EFAULT, "simulated CIFS metadata failure")

        def temporary_pause() -> bool:
            nonlocal pause_checks
            pause_checks += 1
            return pause_checks <= 2

        uploader_module.shutil.copystat = failing_copystat
        try:
            uploader_module.copy_file_limited(
                metadata_source, metadata_destination, 0, pause=temporary_pause
            )
        finally:
            uploader_module.shutil.copystat = original_copystat
        assert metadata_destination.read_bytes() == metadata_source.read_bytes()
        assert pause_checks >= 3
        process_pause_checks = 0

        def process_pause() -> bool:
            nonlocal process_pause_checks
            process_pause_checks += 1
            return process_pause_checks <= 2

        paused_process = uploader_module.run_checked(
            [args.python, "-c", "import time; time.sleep(0.2)"],
            temporary / "paused_process.log",
            timeout=5,
            pause=process_pause,
        )
        assert paused_process.returncode == 0 and process_pause_checks >= 3
        probe_pause_checks = 0

        def probe_pause() -> bool:
            nonlocal probe_pause_checks
            probe_pause_checks += 1
            return probe_pause_checks <= 2

        assert uploader_module.media_duration(rgb_path, ffmpeg, pause=probe_pause) is not None
        assert probe_pause_checks >= 3
        try:
            uploader_module.finalize_rgb(invalid_segment, invalid_rgb.name, ffmpeg)
        except RuntimeError as error:
            assert "no closing mfra atom" in str(error)
        else:
            raise AssertionError("fragmented MP4 without mfra was accepted")
        (segment / f"{prefix}frames.csv").write_text("local_time_us,stream_type\n1,rgb\n", encoding="ascii")
        (segment / f"{prefix}meta.json").write_text(
            json.dumps(
                {
                    "closed": True,
                    "recording_ready_file": f"{prefix}recording_ready.json",
                    "rgb_target_duration_sec": 3600.0,
                    "rgb_container_expected_duration_sec": 3.0,
                    "rgb_frames": 90,
                    "rgb_record_fps": 30.0,
                }
            ),
            encoding="ascii",
        )
        staged = {
            "schema": "gwv3_recording_staged_v1",
            "staged": True,
            "segment_start_us": 1784625600000000,
            # The recorder may remain administratively active after media input
            # stops. That wall-clock span must not be used to validate MP4 media.
            "segment_end_us": 1784629200000000,
            "sender_id": "sender-a",
            "camera_id": "cam01",
            "relative_path": "camera-a/2026-07-21/120000",
            "frames_file": f"{prefix}frames.csv",
            "meta_file": f"{prefix}meta.json",
            "ready_file": f"{prefix}recording_ready.json",
            "rgb_file": f"{prefix}rgb.mp4",
            "depth_file": f"{prefix}depth.mkv",
            "recording_quality_status": "partial",
            "recording_complete": False,
            "recording_quality_reason": "rgb tail is missing",
            "rgb_coverage_ratio": 0.75,
        }
        (segment / f"{prefix}recording_staged.json").write_text(json.dumps(staged), encoding="ascii")

        config = {
            "nas_root": str(nas_root),
            "ffmpeg_path": ffmpeg,
            "recording_staging": {
                "enabled": True,
                "root": str(staging_root),
                "upload_interval_ms": 250,
                "delete_after_upload": True,
                "pause_during_receiver_finalize": False,
            },
        }
        config_path = temporary / "receiver.json"
        config_path.write_text(json.dumps(config), encoding="ascii")

        interrupt_staging = temporary / "interrupt-staging"
        interrupt_nas = temporary / "interrupt-nas"
        interrupt_segment = interrupt_staging / "camera" / "2026-07-24" / "120000"
        shutil.copytree(segment, interrupt_segment)
        interrupt_nas.mkdir()
        interrupt_config_path = temporary / "interrupt-receiver.json"
        interrupt_config_path.write_text(
            json.dumps(
                {
                    "nas_root": str(interrupt_nas),
                    "recording_staging": {
                        "enabled": True,
                        "root": str(interrupt_staging),
                        "pause_during_receiver_finalize": False,
                    },
                }
            ),
            encoding="ascii",
        )
        interrupt_uploader = uploader_module.Uploader(interrupt_config_path)
        original_publish_capture = uploader_module.publish_capture_segment

        def interrupt_capture(*_args: object, **_kwargs: object) -> None:
            raise InterruptedError("simulated service stop")

        uploader_module.publish_capture_segment = interrupt_capture
        try:
            assert interrupt_uploader.process_one(interrupt_segment) is False
        finally:
            uploader_module.publish_capture_segment = original_publish_capture
        assert interrupt_uploader.failures == 0
        assert interrupt_uploader.last_error == ""

        # A regular file at nas_root simulates an unavailable/broken mount.
        nas_root.write_text("offline", encoding="ascii")
        first = subprocess.run(
            [args.python, args.uploader, "--config", str(config_path), "--once"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
            check=False,
        )
        assert first.returncode == 2, (first.stdout, first.stderr)
        assert segment.exists(), "failed upload removed the only local recording"
        assert (segment / f"{prefix}recording_staged.json").is_file()
        assert not (segment / f"{prefix}recording_ready.json").exists()
        assert b"moof" in atoms(rgb_path), "NAS-first capture unexpectedly rewrote the local fMP4"

        nas_root.unlink()
        nas_root.mkdir()
        (segment / "aaa_interruption_probe.bin").write_bytes(b"x" * (2 * 1024 * 1024))
        config["recording_staging"]["upload_bandwidth_limit_mbps"] = 1
        config_path.write_text(json.dumps(config), encoding="ascii")
        interrupted = subprocess.Popen(
            [args.python, args.uploader, "--config", str(config_path), "--once"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        # A busy receiver may spend several seconds finalizing the synthetic
        # conventional MP4 before it reaches the intentionally throttled copy.
        deadline = time.monotonic() + 30
        active_status = {}
        while time.monotonic() < deadline:
            status_path = staging_root / ".gwv3_uploader_status.json"
            if status_path.is_file():
                active_status = json.loads(status_path.read_text(encoding="utf-8"))
            if (
                list(nas_root.rglob(".gwv3-uploading-*"))
                and active_status.get("active_phase") == "capturing_to_nas"
                and int(active_status.get("active_bytes_total", 0)) > 0
            ):
                break
            assert interrupted.poll() is None, "uploader exited before interruption point"
            time.sleep(0.02)
        else:
            interrupted.kill()
            raise AssertionError("uploader did not enter the NAS copy phase")
        assert active_status["active_segment"] == str(segment)
        assert 0 <= float(active_status["active_progress_percent"]) <= 100
        assert int(active_status["active_elapsed_ms"]) >= 0
        interrupted.terminate()
        interrupted.wait(timeout=5)
        assert segment.exists(), "interrupted upload removed the local segment"

        config["recording_staging"]["upload_bandwidth_limit_mbps"] = 0
        config_path.write_text(json.dumps(config), encoding="ascii")
        second = subprocess.run(
            [args.python, args.uploader, "--config", str(config_path), "--once"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            # Real receiver disks can be busy with active recordings while
            # this recovery path verifies and fsyncs the synthetic segment.
            timeout=120,
            check=False,
        )
        assert second.returncode == 0, (second.stdout, second.stderr)
        destination = nas_root / "camera-a" / "2026-07-21" / "120000"
        assert not segment.exists(), "successfully published local segment was not released"
        final_ready = json.loads((destination / f"{prefix}recording_ready.json").read_text(encoding="utf-8"))
        assert final_ready["ready"] is True
        assert final_ready["recording_quality_status"] == "partial"
        assert final_ready["recording_complete"] is False
        assert final_ready["recording_quality_reason"] == "rgb tail is missing"
        assert final_ready["rgb_coverage_ratio"] == 0.75
        assert not list(nas_root.rglob(".gwv3-uploading-*")), "stale interrupted upload directory was not cleaned"
        assert b"moov" in atoms(destination / f"{prefix}rgb.mp4") and b"moof" not in atoms(destination / f"{prefix}rgb.mp4")
        probe = subprocess.run(
            [ffprobe, "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(destination / f"{prefix}rgb.mp4")],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )
        assert probe.returncode == 0 and float(probe.stdout) > 2.0, probe.stderr
        status = json.loads((staging_root / ".gwv3_uploader_status.json").read_text(encoding="utf-8"))
        assert status["schema"] == "gwv3_recording_uploader_status_v2"
        assert status["pipeline_mode"] == "nas_first_local_cache_finalize"
        assert status["pending_segments"] == 0
        assert status["local_pending_segments"] == 0
        assert status["local_finalize_cache_segments"] == 0
        assert status["nas_finalize_pending_segments"] == 0
        assert status["publish_recovery_journals"] == 0
        assert status["captured_segments"] == 1
        assert status["completed_segments"] == 1
        assert status["local_remuxes"] == 1
        assert status["nas_fallback_remuxes"] == 0
        assert status["last_remux_source"] == "local"
        assert not list((nas_root / ".gwv3_capture_queue").rglob("*recording_capture_ready.json"))
        assert not list((nas_root / ".gwv3_capture_queue").rglob("*recording_ready.json"))

        recovery_staging = temporary / "recovery-staging"
        recovery_nas = temporary / "recovery-nas"
        recovery_segment = recovery_staging / "camera-recovery" / "2026-07-24" / "130000"
        shutil.copytree(interrupt_segment, recovery_segment)
        recovery_marker_path = next(recovery_segment.glob("*recording_staged.json"))
        recovery_marker = json.loads(recovery_marker_path.read_text(encoding="ascii"))
        recovery_marker["relative_path"] = "camera-recovery/2026-07-24/130000"
        recovery_marker_path.write_text(json.dumps(recovery_marker), encoding="ascii")
        recovery_nas.mkdir()
        recovery_config_path = temporary / "recovery-receiver.json"
        recovery_config_path.write_text(
            json.dumps(
                {
                    "nas_root": str(recovery_nas),
                    "ffmpeg_path": ffmpeg,
                    "recording_staging": {
                        "enabled": True,
                        "root": str(recovery_staging),
                        "delete_after_upload": True,
                        "pause_during_receiver_finalize": False,
                    },
                }
            ),
            encoding="ascii",
        )
        recovery_uploader = uploader_module.Uploader(recovery_config_path)
        original_finalize_capture = uploader_module.finalize_captured_segment

        def fail_nas_finalize(*_args: object, **_kwargs: object) -> None:
            raise RuntimeError("simulated NAS-side remux failure")

        uploader_module.finalize_captured_segment = fail_nas_finalize
        try:
            assert recovery_uploader.run_once() is True
        finally:
            uploader_module.finalize_captured_segment = original_finalize_capture
        assert recovery_segment.exists(), "local remux cache was released before NAS finalization"
        assert list(recovery_segment.glob("*recording_capture_cached.json"))
        recovery_captures = uploader_module.discover_capture_segments(
            recovery_nas / uploader_module.CAPTURE_QUEUE_DIRECTORY
        )
        assert len(recovery_captures) == 1
        captured_rgb = recovery_captures[0] / f"{prefix}rgb.mp4"
        assert b"moof" in atoms(captured_rgb), "failed NAS finalize destroyed the captured fMP4"
        assert recovery_uploader.failures == 1

        original_final_publish = uploader_module.publish_finalized_capture
        observed_local_sources: list[Path | None] = []
        observed_remux_sources: list[str] = []
        original_finalize_capture = uploader_module.finalize_captured_segment

        def observe_local_finalize(*finalize_args: object, **finalize_kwargs: object):
            observed_local_sources.append(finalize_kwargs.get("local_segment"))
            result = original_finalize_capture(*finalize_args, **finalize_kwargs)
            observed_remux_sources.append(result[2])
            return result

        def interrupt_final_publish(*_args: object, **_kwargs: object) -> None:
            raise InterruptedError("simulated stop after NAS remux")

        uploader_module.finalize_captured_segment = observe_local_finalize
        uploader_module.publish_finalized_capture = interrupt_final_publish
        try:
            assert recovery_uploader.process_capture_one(recovery_captures[0]) is False
        finally:
            uploader_module.finalize_captured_segment = original_finalize_capture
            uploader_module.publish_finalized_capture = original_final_publish
        assert observed_local_sources == [recovery_segment]
        assert observed_remux_sources == ["local"]
        assert list(recovery_captures[0].glob("*recording_nas_finalized.json"))
        assert not list(recovery_captures[0].glob("*recording_ready.json"))
        assert not (recovery_captures[0] / ".gwv3_uploader.lock").exists()
        assert not list(
            (recovery_nas / uploader_module.CAPTURE_QUEUE_DIRECTORY).glob(".gwv3-lock-*")
        )
        assert b"moof" not in atoms(captured_rgb), "NAS remux did not finish before final publish interruption"

        recovered_destination = recovery_nas / "camera-recovery" / "2026-07-24" / "130000"
        original_recover_publish = uploader_module.recover_publish_journal

        def interrupt_after_directory_move(*_args: object, **_kwargs: object) -> None:
            raise InterruptedError("simulated stop after final directory move")

        uploader_module.recover_publish_journal = interrupt_after_directory_move
        try:
            assert recovery_uploader.process_capture_one(recovery_captures[0]) is False
        finally:
            uploader_module.recover_publish_journal = original_recover_publish
        assert not recovery_captures[0].exists()
        assert recovered_destination.is_dir()
        assert not (recovered_destination / f"{prefix}recording_ready.json").exists()
        assert (recovered_destination / f"{prefix}recording_ready.json.pending").is_file()
        assert not list(
            (recovery_nas / uploader_module.CAPTURE_QUEUE_DIRECTORY).glob(".gwv3-lock-*")
        )
        assert list(
            (recovery_nas / uploader_module.CAPTURE_QUEUE_DIRECTORY).glob(
                uploader_module.PUBLISH_JOURNAL_PREFIX
                + "*"
                + uploader_module.PUBLISH_JOURNAL_SUFFIX
            )
        )

        assert recovery_uploader.run_once() is True
        assert (recovered_destination / f"{prefix}recording_ready.json").is_file()
        assert b"moof" not in atoms(recovered_destination / f"{prefix}rgb.mp4")
        assert not (recovered_destination / f"{prefix}recording_ready.json.pending").exists()
        assert not (recovered_destination / ".gwv3_publish_incomplete.json").exists()
        assert not uploader_module.discover_capture_segments(
            recovery_nas / uploader_module.CAPTURE_QUEUE_DIRECTORY
        )
        assert not uploader_module.discover_publish_journals(
            recovery_nas / uploader_module.CAPTURE_QUEUE_DIRECTORY
        )
        assert not recovery_segment.exists(), "published local remux cache was not released"

        parallel_staging = temporary / "parallel-staging"
        parallel_nas = temporary / "parallel-nas"
        parallel_nas.mkdir()
        for index in range(2):
            parallel_segment = (
                parallel_staging
                / f"camera-parallel-{index}"
                / "2026-07-24"
                / "140000"
            )
            shutil.copytree(interrupt_segment, parallel_segment)
            marker_path = next(parallel_segment.glob("*recording_staged.json"))
            marker = json.loads(marker_path.read_text(encoding="ascii"))
            marker["sender_id"] = f"sender-parallel-{index}"
            marker["camera_id"] = f"cam{index + 1:02d}"
            marker["segment_start_us"] = 1784892000000000 + index
            marker["relative_path"] = (
                f"camera-parallel-{index}/2026-07-24/140000"
            )
            marker_path.write_text(json.dumps(marker), encoding="ascii")
        parallel_config_path = temporary / "parallel-receiver.json"
        parallel_config_path.write_text(
            json.dumps(
                {
                    "nas_root": str(parallel_nas),
                    "ffmpeg_path": ffmpeg,
                    "recording_staging": {
                        "enabled": True,
                        "root": str(parallel_staging),
                        "delete_after_upload": True,
                        "retain_local_capture_for_finalize": True,
                        "capture_workers": 2,
                        "full_copy_workers": 2,
                        "finalize_workers": 2,
                        "pause_during_receiver_finalize": False,
                    },
                }
            ),
            encoding="ascii",
        )
        parallel_uploader = uploader_module.Uploader(parallel_config_path)
        assert parallel_uploader.run_once() is True
        assert parallel_uploader.local_remuxes == 2
        assert parallel_uploader.nas_fallback_remuxes == 0
        assert not uploader_module.discover_local_capture_caches(parallel_staging)
        assert not uploader_module.discover_capture_segments(
            parallel_nas / uploader_module.CAPTURE_QUEUE_DIRECTORY
        )
        for index in range(2):
            parallel_destination = (
                parallel_nas
                / f"camera-parallel-{index}"
                / "2026-07-24"
                / "140000"
            )
            assert b"moof" not in atoms(
                parallel_destination / f"{prefix}rgb.mp4"
            )

        fallback_staging = temporary / "fallback-staging"
        fallback_nas = temporary / "fallback-nas"
        fallback_segment = (
            fallback_staging / "camera-fallback" / "2026-07-24" / "150000"
        )
        shutil.copytree(interrupt_segment, fallback_segment)
        fallback_marker_path = next(fallback_segment.glob("*recording_staged.json"))
        fallback_marker = json.loads(
            fallback_marker_path.read_text(encoding="ascii")
        )
        fallback_marker["relative_path"] = (
            "camera-fallback/2026-07-24/150000"
        )
        fallback_marker_path.write_text(
            json.dumps(fallback_marker),
            encoding="ascii",
        )
        fallback_nas.mkdir()
        fallback_config_path = temporary / "fallback-receiver.json"
        fallback_config_path.write_text(
            json.dumps(
                {
                    "nas_root": str(fallback_nas),
                    "ffmpeg_path": ffmpeg,
                    "recording_staging": {
                        "enabled": True,
                        "root": str(fallback_staging),
                        "delete_after_upload": True,
                        "retain_local_capture_for_finalize": True,
                        "local_cache_high_watermark_percent": 75,
                        "local_cache_low_watermark_percent": 70,
                        "finalize_workers": 2,
                        "pause_during_receiver_finalize": False,
                    },
                }
            ),
            encoding="ascii",
        )
        fallback_uploader = uploader_module.Uploader(fallback_config_path)
        original_disk_usage = uploader_module.shutil.disk_usage
        uploader_module.shutil.disk_usage = lambda _path: SimpleNamespace(
            total=100,
            used=80,
            free=20,
        )
        try:
            assert fallback_uploader.run_once() is True
        finally:
            uploader_module.shutil.disk_usage = original_disk_usage
        assert fallback_uploader.local_remuxes == 0
        assert fallback_uploader.nas_fallback_remuxes == 1
        assert fallback_uploader.local_cache_pressure_releases == 1
        assert not fallback_segment.exists()

        fragmented_staging = temporary / "fragmented-staging"
        fragmented_nas = temporary / "fragmented-nas"
        fragmented_segment = (
            fragmented_staging / "camera-fragmented" / "2026-07-24" / "160000"
        )
        shutil.copytree(interrupt_segment, fragmented_segment)
        fragmented_marker_path = next(
            fragmented_segment.glob("*recording_staged.json")
        )
        fragmented_marker = json.loads(
            fragmented_marker_path.read_text(encoding="ascii")
        )
        fragmented_marker["relative_path"] = (
            "camera-fragmented/2026-07-24/160000"
        )
        fragmented_marker_path.write_text(
            json.dumps(fragmented_marker),
            encoding="ascii",
        )
        fragmented_nas.mkdir()
        fragmented_config_path = temporary / "fragmented-receiver.json"
        fragmented_config_path.write_text(
            json.dumps(
                {
                    "nas_root": str(fragmented_nas),
                    "ffmpeg_path": ffmpeg,
                    "recording_staging": {
                        "enabled": True,
                        "root": str(fragmented_staging),
                        "rgb_output_mode": "fragmented_mp4",
                        "delete_after_upload": True,
                        "retain_local_capture_for_finalize": True,
                        "pause_during_receiver_finalize": False,
                    },
                }
            ),
            encoding="ascii",
        )
        fragmented_uploader = uploader_module.Uploader(fragmented_config_path)
        original_run_checked = uploader_module.run_checked

        def reject_remux(*_args: object, **_kwargs: object) -> None:
            raise AssertionError("fMP4 delivery unexpectedly invoked FFmpeg remux")

        uploader_module.run_checked = reject_remux
        try:
            assert fragmented_uploader.run_once() is True
        finally:
            uploader_module.run_checked = original_run_checked
        fragmented_destination = (
            fragmented_nas / "camera-fragmented" / "2026-07-24" / "160000"
        )
        fragmented_rgb = fragmented_destination / f"{prefix}rgb.mp4"
        fragmented_atoms = atoms(fragmented_rgb)
        assert {b"moov", b"moof", b"mfra"} <= fragmented_atoms
        fragmented_ready = json.loads(
            (
                fragmented_destination / f"{prefix}recording_ready.json"
            ).read_text(encoding="utf-8")
        )
        assert fragmented_ready["rgb_container_format"] == "fragmented_mp4"
        assert fragmented_ready["rgb_fragmented"] is True
        assert fragmented_uploader.fragmented_passthroughs == 1
        assert fragmented_uploader.local_remuxes == 0
        assert fragmented_uploader.nas_fallback_remuxes == 0
        fragmented_status = json.loads(
            (
                fragmented_staging / ".gwv3_uploader_status.json"
            ).read_text(encoding="utf-8")
        )
        assert fragmented_status["pipeline_mode"] == "nas_first_fragmented_mp4"
        assert fragmented_status["rgb_output_mode"] == "fragmented_mp4"
        assert fragmented_status["fragmented_passthroughs"] == 1
        assert fragmented_status["last_remux_source"] == "fragmented_passthrough"
        fragmented_probe = subprocess.run(
            [
                ffprobe,
                "-v",
                "error",
                "-show_entries",
                "format=duration",
                "-of",
                "csv=p=0",
                str(fragmented_rgb),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )
        assert (
            fragmented_probe.returncode == 0
            and float(fragmented_probe.stdout) > 2.0
        ), fragmented_probe.stderr
        fragmented_seek = subprocess.run(
            [
                ffmpeg,
                "-v",
                "error",
                "-ss",
                "1",
                "-i",
                str(fragmented_rgb),
                "-frames:v",
                "1",
                "-f",
                "null",
                "-",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )
        assert fragmented_seek.returncode == 0, fragmented_seek.stderr

        invalid_mode_config = json.loads(
            fragmented_config_path.read_text(encoding="ascii")
        )
        invalid_mode_config["recording_staging"]["rgb_output_mode"] = "unknown"
        invalid_mode_path = temporary / "invalid-output-mode.json"
        invalid_mode_path.write_text(
            json.dumps(invalid_mode_config),
            encoding="ascii",
        )
        try:
            uploader_module.Uploader(invalid_mode_path)
        except ValueError as error:
            assert "rgb_output_mode" in str(error)
        else:
            raise AssertionError("invalid RGB output mode was accepted")

        unsafe_segment = staging_root / "camera-unsafe" / "2026-07-21" / "120100"
        unsafe_segment.mkdir(parents=True)
        unsafe_marker = dict(staged)
        unsafe_marker["relative_path"] = "camera-unsafe/2026-07-21/120100"
        unsafe_marker["frames_file"] = "../escape.csv"
        (unsafe_segment / "recording_staged.json").write_text(json.dumps(unsafe_marker), encoding="ascii")
        unsafe = subprocess.run(
            [args.python, args.uploader, "--config", str(config_path), "--once"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=30,
            check=False,
        )
        assert unsafe.returncode == 2, (unsafe.stdout, unsafe.stderr)
        assert b"unsafe frames_file" in unsafe.stderr
        assert unsafe_segment.exists(), "rejected staging marker must remain available for inspection"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--uploader", required=True)
    parser.add_argument("--python", default="python3")
    return parser.parse_args()


if __name__ == "__main__":
    run(parse_args())
