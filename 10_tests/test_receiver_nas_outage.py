#!/usr/bin/env python3
"""NAS admission failures must not interrupt an admitted recording session.

All paths are temporary and all sockets use loopback. No mount operations or
production NAS are used, including in the actual-destination reserve test.
"""
import argparse
import csv
import itertools
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

import test_receiver_hardening as h


BAD_SNAPSHOTS = ("unavailable", "missing", "malformed", "stale", "low_free",
                 "invalid_free", "wrong_mount", "future")


class Receiver:
    def __init__(self, binary, *, staging=True, reserve_mb=0, segment_seconds=3):
        self.temporary = tempfile.TemporaryDirectory(prefix="gwv3_nas_outage_")
        self.root = Path(self.temporary.name)
        self.nas = self.root / "nas"
        self.nas.mkdir()
        self.snapshot = self.root / "capacity.json"
        self.admin = h.free_port(socket.SOCK_STREAM)
        self.media_port = h.free_port(socket.SOCK_STREAM)
        self.status_port = h.free_port(socket.SOCK_DGRAM)
        self.frame_id = 0
        self.config = {
            "media_bind_ip": "127.0.0.1", "media_port": self.media_port,
            "admin_bind_ip": "127.0.0.1", "admin_port": self.admin,
            "status_bind_ip": "127.0.0.1", "status_port": self.status_port,
            "nas_root": str(self.nas), "state_path": str(self.root / "state.json"),
            "log_directory": str(self.root / "logs"), "preview_enabled": False,
            "media_udp_enabled": False, "receiver_discovery": {"enabled": False},
            "clock_sync": {"enabled": False}, "task_audio": {"enabled": False},
            "recording_start_lead_ms": 0, "recording_stop_drain_timeout_ms": 100,
            "segment_seconds": segment_seconds, "depth_fps": 30,
            "min_free_disk_mb": reserve_mb, "shared_nas_min_free_mb": 1024,
            "nas_auto_mount": {"enabled": True, "require_for_new_recording": True,
                               "status_path": str(self.snapshot), "status_max_age_ms": 10000},
            "recording_staging": {
                "enabled": staging, "root": str(self.root / "staging"),
                "defer_player_compatible_finalize": True,
                "rgb_output_mode": "fragmented_mp4", "idle_finalize_ms": 10000,
                "media_recovery_grace_ms": 10000,
            },
        }
        self.write_snapshot("healthy")
        config_path = self.root / "config.json"
        config_path.write_text(json.dumps(self.config), encoding="utf-8")
        self.log_path = self.root / "receiver.log"
        self.log = self.log_path.open("wb")
        self.proc = subprocess.Popen([binary, "--config", str(config_path)],
                                     stdout=self.log, stderr=subprocess.STDOUT)
        self.media = None

    def __enter__(self):
        try:
            h.wait_http(self.admin)
            h.status_message(self.status_port, {
                "protocol_version": "3.0", "message_type": "camera_announce",
                "sender_id": "outage-test", "camera_id": "cam01",
                "rgb_profile": {"width": 64, "height": 48, "fps": 30},
                "depth_profile": {"width": 64, "height": 48, "fps": 30, "depth_scale": 1},
            })
            self.wait(lambda s: bool(s.get("cameras")), "camera announcement")
            self.media = socket.create_connection(("127.0.0.1", self.media_port), timeout=3)
            return self
        except Exception:
            self.__exit__(Exception, None, None)
            raise

    def __exit__(self, exc_type, exc, traceback):
        if self.media:
            self.media.close()
        self.proc.terminate()
        try:
            self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        self.log.close()
        if exc_type:
            print(self.log_path.read_text(encoding="utf-8", errors="replace"))
        self.temporary.cleanup()

    def write_snapshot(self, kind):
        if kind == "missing":
            self.snapshot.unlink(missing_ok=True)
            return
        value = {"ready": True, "free_bytes": 10 * 2**30,
                 "mount_point": str(self.nas), "updated_us": time.time_ns() // 1000}
        if kind == "unavailable":
            value.update(ready=False, free_bytes=None)
        elif kind == "stale":
            value["updated_us"] = 1
        elif kind == "low_free":
            value["free_bytes"] = 0
        elif kind == "invalid_free":
            value["free_bytes"] = "unknown"
        elif kind == "wrong_mount":
            value["mount_point"] = str(self.root / "wrong-mount")
        elif kind == "future":
            value["updated_us"] += 60_000_000
        text = "{invalid json" if kind == "malformed" else json.dumps(value)
        pending = self.snapshot.with_suffix(".tmp")
        pending.write_text(text, encoding="utf-8")
        pending.replace(self.snapshot)

    def status(self):
        assert self.proc.poll() is None, "receiver exited unexpectedly"
        return json.loads(h.request(self.admin, "GET", "/api/status")[2])

    def command(self, action):
        return json.loads(h.request(self.admin, "POST", "/api/record/" + action)[2])

    def wait(self, predicate, description, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            current = self.status()
            if predicate(current):
                return current
            time.sleep(0.05)
        raise AssertionError(f"timed out waiting for {description}: {current}")

    def send_frames(self, fixture, count=45):
        for _ in range(count):
            timestamp = time.time_ns() // 1000
            self.media.sendall(h.rgb_packet("outage-test", "cam01", self.frame_id,
                                            64, 48, timestamp, fixture))
            self.media.sendall(h.depth_packet("outage-test", "cam01", self.frame_id,
                                              64, 48, timestamp))
            self.frame_id += 1
            time.sleep(1 / 30)

    def recorded_frames(self):
        # A direct segment may move midway through discovery or opening a CSV.
        # Retry the complete scan instead of returning incomplete frame evidence.
        for attempt in range(3):
            try:
                return self._recorded_frames_once()
            except FileNotFoundError:
                if attempt == 2:
                    raise
                time.sleep(0.02)

    def _recorded_frames_once(self):
        # A finalized segment replaces its live journal. Frame identities avoid
        # double counting while that atomic publication happens during a scan.
        frames = {"rgb": set(), "depth": set()}
        def scan_error(error):
            raise error

        paths = []
        for directory, _, files in os.walk(self.root, onerror=scan_error):
            paths.extend(Path(directory) / name for name in files
                         if name in {"frames.csv", "frames.csv.inprogress", "rgb_recorded_frames.csv"})
        for path in paths:
            with path.open(newline="", encoding="utf-8") as stream:
                # A live CSV flush can end mid-row while this reader catches up.
                # Stop at its first incomplete line: a concurrently appended
                # suffix is not a new row. The next fresh scan sees the full tail.
                for row in csv.DictReader(itertools.takewhile(lambda line: line.endswith("\n"), stream)):
                    kind = "rgb" if path.name == "rgb_recorded_frames.csv" else row.get("stream_type", "")
                    if kind == "rgb" and path.name != "rgb_recorded_frames.csv":
                        # The packet journal alone does not prove an RGB frame
                        # reached the muxer. Final maps mark successful writes.
                        if path.name != "frames.csv" or row.get("rgb_recorded") != "1":
                            continue
                    if kind.startswith("depth"):
                        kind = "depth"
                    frame_id = row.get("frame_id", "")
                    if kind in frames and frame_id and frame_id.isdigit():
                        frames[kind].add(int(frame_id))
        return frames

    def assert_recorded_media(self):
        frames = self.recorded_frames()
        for kind, filename, codec in (("rgb", "rgb.mp4", "h264"), ("depth", "depth.mkv", "ffv1")):
            files = list(self.root.rglob(filename))
            assert files, f"no finalized {kind} video"
            total = 0
            for path in files:
                result = subprocess.run([
                    "ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames",
                    "-show_entries", "stream=codec_name,nb_read_frames", "-of", "json", str(path),
                ], capture_output=True, text=True, check=True, timeout=10)
                stream = json.loads(result.stdout)["streams"][0]
                count = int(stream["nb_read_frames"])
                assert count > 0 and stream["codec_name"] == codec, (path, stream)
                total += count
            expected = len(frames[kind])
            assert expected > 0
            if kind == "rgb":
                assert total == expected, (kind, total, expected)
            else:
                # Depth output can trim timestamp boundaries, but must account
                # for the frame journal, including frames sent during outages.
                assert total >= expected * 0.9, (kind, total, expected)


def assert_active(status, session, label):
    camera = status["cameras"][0]
    assert status["recording_all"] and not status["recording_faulted"], (
        f"{label}: NAS snapshot alone aborted recording: {status}")
    assert status["recording_session_id"] == session, f"{label}: global session changed"
    assert camera["recording_session_id"] == session, f"{label}: camera session changed"
    assert camera["recording"] and camera["segment_active"], f"{label}: segment stopped"
    assert camera["record_write_errors"] == 0, f"{label}: false recording write error"
    assert not status["recording_storage"]["hard_limit"], f"{label}: false destination hard limit"
    return camera


def test_continuity(binary, fixture, staging):
    with Receiver(binary, staging=staging) as receiver:
        start = receiver.command("start-all")
        assert start["ok"], start
        session = start["recording_session_id"]
        receiver.send_frames(fixture)
        before = assert_active(receiver.status(), session, "healthy")
        first_segment = before["global_segment_index"]
        for kind in BAD_SNAPSHOTS:
            frames_before = receiver.recorded_frames()
            receiver.write_snapshot(kind)
            receiver.send_frames(fixture)
            current = receiver.wait(lambda s: not s["recording_start_ready"], kind)
            assert_active(current, session, kind)
            if kind not in {"low_free", "invalid_free"}:
                assert current["nas_auto_mount"]["ready"] is False, kind
            frames_after = receiver.recorded_frames()
            # A new segment probes RGB FPS for up to 60 frames / three seconds.
            # Keep sending under the same unhealthy snapshot until the muxer has
            # enough evidence; never substitute received packets for writes.
            growth_deadline = time.monotonic() + 6
            while (not all(frames_after[stream] - frames_before[stream] for stream in ("rgb", "depth"))
                   and time.monotonic() < growth_deadline):
                receiver.send_frames(fixture, 15)
                current = receiver.status()
                assert_active(current, session, kind)
                frames_after = receiver.recorded_frames()
            for stream in ("rgb", "depth"):
                assert frames_after[stream] - frames_before[stream], (
                    f"{kind}: {stream} frame files stopped growing; "
                    f"before={len(frames_before[stream])}, after={len(frames_after[stream])}, "
                    f"camera={current['cameras'][0]}")
        # Several three-second boundaries have passed entirely under bad snapshots.
        assert current["cameras"][0]["global_segment_index"] > first_segment
        receiver.write_snapshot("healthy")
        receiver.send_frames(fixture)
        current = receiver.wait(lambda s: s["recording_start_ready"], "NAS recovery")
        assert_active(current, session, "recovered")
        assert current["nas_auto_mount"]["ready"] is True

        receiver.write_snapshot("unavailable")
        assert receiver.command("stop-all")["ok"]
        stopped = receiver.wait(lambda s: not s["recording_all"] and
                                not s["cameras"][0]["segment_active"] and
                                s["record_finalize_outstanding_segments"] == 0, "explicit stop", 15)
        assert not stopped["recording_faulted"], stopped
        receiver.assert_recorded_media()
        frames_before = receiver.recorded_frames()
        receiver.send_frames(fixture, 15)
        assert receiver.recorded_frames() == frames_before, "explicit stop still records frames"

        for kind in BAD_SNAPSHOTS:
            receiver.write_snapshot(kind)
            for action in ("start-all", "start-sender?sender_id=outage-test",
                           "start?sender_id=outage-test&camera_id=cam01"):
                assert receiver.command(action)["ok"] is False, (kind, action)
        # The original admission guard reserves 2 GiB beyond the 1 GiB NAS minimum.
        receiver.write_snapshot("healthy")
        value = json.loads(receiver.snapshot.read_text())
        value["free_bytes"] = 2 * 2**30
        receiver.snapshot.write_text(json.dumps(value), encoding="utf-8")
        assert receiver.command("start-all")["ok"] is False, "NAS recovery headroom lost"
        receiver.write_snapshot("healthy")
        restarted = receiver.command("start-all")
        assert restarted["ok"] and restarted["recording_session_id"] != session, restarted
        assert receiver.command("stop-all")["ok"]
        print(f"NAS snapshot continuity, rollover, recovery and admission passed (staging={staging})")


def test_staging_nas_path_outage(binary, fixture):
    with Receiver(binary, staging=True) as receiver:
        start = receiver.command("start-all")
        assert start["ok"], start
        session = start["recording_session_id"]
        receiver.send_frames(fixture)
        initial = assert_active(receiver.status(), session, "healthy NAS path")
        before = receiver.recorded_frames()
        retained = receiver.root / "nas-retained"
        receiver.nas.rename(retained)
        receiver.nas.write_text("temporary test NAS path unavailable", encoding="utf-8")
        receiver.write_snapshot("unavailable")
        try:
            # Four seconds must cross the three-second segment boundary while
            # only the NAS publication path is unusable; the staging root is healthy.
            receiver.send_frames(fixture, 120)
            current = receiver.status()
            camera = assert_active(current, session, "NAS path unavailable during staging rollover")
            assert camera["global_segment_index"] > initial["global_segment_index"], current
            after = receiver.recorded_frames()
            assert all(after[stream] - before[stream] for stream in ("rgb", "depth"))
            assert not current["recording_start_ready"]
            assert not current["nas_auto_mount"]["ready"]
            assert receiver.command("stop-all")["ok"]
            receiver.wait(lambda s: not s["recording_all"] and
                          not s["cameras"][0]["segment_active"] and
                          s["record_finalize_outstanding_segments"] == 0, "staged outage stop", 15)
            receiver.assert_recorded_media()
            assert list((receiver.root / "staging").rglob("recording_staged.json")), (
                "NAS outage lost the finalized local upload backlog")
        finally:
            receiver.nas.unlink()
            retained.rename(receiver.nas)
        print("staging rollover survives an unusable NAS publication path")


def test_destination_reserve(binary, fixture):
    # Use the real free-space readings from two filesystems without filling either.
    # Repoint only this test's segment path; its already-open files stay in the
    # renamed temporary directory until shutdown restores the original path.
    assert Path("/dev/shm").is_dir(), "Linux /dev/shm required for reserve regression"
    reserve_mb = shutil.disk_usage("/dev/shm").total // 2**20 + 1
    assert reserve_mb < 1024 * 1024, "/dev/shm too large for bounded reserve test"
    with Receiver(binary, reserve_mb=reserve_mb, segment_seconds=60) as receiver:
        assert receiver.command("start-all")["ok"]
        receiver.send_frames(fixture)
        segment = Path(receiver.status()["cameras"][0]["segment_dir"])
        retained = segment.with_name(segment.name + "-retained")
        segment.rename(retained)
        with tempfile.TemporaryDirectory(prefix="gwv3_reserve_", dir="/dev/shm") as limited:
            segment.symlink_to(limited, target_is_directory=True)
            try:
                receiver.send_frames(fixture, 30)
                failed = receiver.wait(lambda s: s["recording_faulted"], "actual destination reserve fault")
                assert not failed["recording_all"], failed
                assert "reserve" in failed["recording_fault_reason"], failed
                assert failed["cameras"][0]["record_write_errors"] > 0, failed
            finally:
                segment.unlink()
                retained.rename(segment)

    # Actual root reserve exhaustion remains a truthful hard_limit and admission block.
    with Receiver(binary, reserve_mb=1024 * 1024) as receiver:
        current = receiver.status()
        if current["recording_storage"]["free_bytes"] >= 1024**4:
            raise AssertionError("test filesystem must have less than 1 TiB available")
        assert current["recording_storage"]["hard_limit"], current
        assert receiver.command("start-all")["ok"] is False
    print("actual recording destination reserve failure remains visible")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--receiver", required=True)
    parser.add_argument("--case", choices=("all", "staging-nas-path"), default="all")
    args = parser.parse_args()
    assert shutil.which("ffmpeg") and shutil.which("ffprobe"), "ffmpeg and ffprobe required"
    fixture = h.generate_h264_fixture(1)
    if args.case == "staging-nas-path":
        test_staging_nas_path_outage(args.receiver, fixture)
        return
    for staging in (True, False):
        test_continuity(args.receiver, fixture, staging)
    test_staging_nas_path_outage(args.receiver, fixture)
    test_destination_reserve(args.receiver, fixture)


if __name__ == "__main__":
    main()
