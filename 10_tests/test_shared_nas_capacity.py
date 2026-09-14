"""Receiver consumes a fresh local capacity snapshot, never a network stat."""
import argparse
import csv
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import test_receiver_hardening as h


def exercise_recording_retry(receiver, recover):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        nas = root / "nas"
        nas.mkdir()
        snapshot = root / "capacity.json"
        good = {"ready": True, "free_bytes": 10 * 2**30, "mount_point": str(nas),
                "updated_us": time.time_ns() // 1000}

        def publish(value):
            pending = root / "capacity.tmp"
            pending.write_text(json.dumps(value))
            pending.replace(snapshot)

        publish(good)
        media_port, admin_port = h.free_port(socket.SOCK_STREAM), h.free_port(socket.SOCK_STREAM)
        status_port = h.free_port(socket.SOCK_DGRAM)
        cfg = dict(media_bind_ip="127.0.0.1", media_port=media_port,
            admin_bind_ip="127.0.0.1", admin_port=admin_port,
            status_bind_ip="127.0.0.1", status_port=status_port,
            nas_root=str(nas), state_path=str(root / "state.json"),
            log_directory=str(root / "logs"), shared_nas_min_free_mb=1024,
            min_free_disk_mb=0, min_free_disk_percent=0, preview_enabled=False,
            receiver_discovery={"enabled": False}, clock_sync={"enabled": False},
            task_audio={"enabled": False}, recording_start_lead_ms=0,
            recording_stop_drain_timeout_ms=0,
            nas_auto_mount={"enabled": False, "status_path": str(snapshot)},
            recording_staging={"enabled": True, "root": str(root / "staging")})
        path = root / "config.json"
        path.write_text(json.dumps(cfg))
        with (root / "output.log").open("w") as log:
            proc = subprocess.Popen([receiver, "--config", str(path)], stdout=log, stderr=log)
            try:
                h.wait_http(admin_port)
                def state():
                    return json.loads(h.request(admin_port, "GET", "/api/status", timeout=1)[2])

                def wait(predicate):
                    end = time.monotonic() + 10
                    while time.monotonic() < end:
                        s = state()
                        if predicate(s):
                            return s
                        time.sleep(.02)
                    raise AssertionError(s)

                h.status_message(status_port, dict(protocol_version="3.0", message_type="camera_announce",
                    sender_id="storage-retry", camera_id="cam01",
                    rgb_profile={"width": 64, "height": 48, "fps": 30},
                    depth_profile={"width": 64, "height": 48, "fps": 30}))
                with socket.create_connection(("127.0.0.1", media_port)) as media:
                    media.sendall(h.depth_packet("storage-retry", "cam01", 1, 64, 48, time.time_ns() // 1000))
                    wait(lambda s: s["cameras"] and s["cameras"][0]["depth_packets"] >= 1)
                    assert json.loads(h.request(admin_port, "POST", "/api/record/start-all")[2])["ok"]
                    time.sleep(.05)
                    media.sendall(h.rgb_packet("storage-retry", "cam01", 2, 64, 48,
                        time.time_ns() // 1000, h.generate_h264_fixture(1)))
                    wait(lambda s: s["cameras"][0]["segment_active"])
                    publish({**good, "updated_us": 1})
                    for fid in range(2, 62):
                        media.sendall(h.depth_packet("storage-retry", "cam01", fid, 64, 48, time.time_ns() // 1000))
                    # The worker must pause, while the management API remains responsive.
                    wait(lambda s: (root / "output.log").read_text().find("recording storage snapshot retry") >= 0)
                    assert not state()["recording_faulted"]
                    if recover:
                        publish({**good, "updated_us": time.time_ns() // 1000})
                        wait(lambda s: s["cameras"][0]["record_dequeued_packets"] >= 61
                             and not s["cameras"][0]["record_active_writes"])
                        assert not state()["recording_faulted"]
                        # A real low-space report must still fault; no stale-success cache.
                        publish({**good, "free_bytes": 0, "updated_us": time.time_ns() // 1000})
                        for fid in range(62, 122):
                            media.sendall(h.depth_packet("storage-retry", "cam01", fid, 64, 48, time.time_ns() // 1000))
                        result = wait(lambda s: s["recording_faulted"])
                        assert "shared_nas_low_space" in result["recording_fault_reason"]
                        wait(lambda s: s["record_finalize_outstanding_segments"] == 0
                             and bool(list((root / "staging").rglob("frames.csv"))))
                        depth_ids = set()
                        for frames in (root / "staging").rglob("frames.csv"):
                            with frames.open(newline="") as source:
                                depth_ids.update(int(row["frame_id"]) for row in csv.DictReader(source)
                                                 if row["stream_type"] == "depth")
                        assert set(range(2, 62)) <= depth_ids, depth_ids
                    else:
                        result = wait(lambda s: s["recording_faulted"])
                        assert "nas_snapshot_stale" in result["recording_fault_reason"]
                print("recording retry", "recovery then low-space stop" if recover else "expiry stop", "passed")
            finally:
                proc.terminate()
                proc.wait(timeout=15)


def run(receiver):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        nas = root / "nas"
        nas.mkdir()
        status = root / "capacity.json"
        ports = [h.free_port(socket.SOCK_STREAM) for _ in range(2)]
        config = dict(media_bind_ip="127.0.0.1", media_port=ports[0],
            admin_bind_ip="127.0.0.1", admin_port=ports[1],
            status_bind_ip="127.0.0.1", status_port=h.free_port(socket.SOCK_DGRAM),
            nas_root=str(nas), state_path=str(root / "state.json"),
            log_directory=str(root / "logs"), shared_nas_min_free_mb=1024,
            min_free_disk_mb=0, preview_enabled=False,
            receiver_discovery={"enabled": False}, clock_sync={"enabled": False},
            task_audio={"enabled": False}, recording_start_lead_ms=0,
            nas_auto_mount={"enabled": False, "status_path": str(status)},
            recording_staging={"enabled": True, "root": str(root / "staging")})
        path = root / "config.json"
        path.write_text(json.dumps(config))
        with (root / "output.log").open("w") as log:
            proc = subprocess.Popen([receiver, "--config", str(path)], stdout=log, stderr=log)
            try:
                h.wait_http(ports[1])
                def start_ok():
                    return json.loads(h.request(ports[1], "POST", "/api/record/start-all")[2]).get("ok")
                def reason():
                    return json.loads(h.request(ports[1], "GET", "/api/status")[2])["recording_storage"]["check_reason"]
                self_status = {"ready": True, "free_bytes": 0, "mount_point": str(nas),
                    "updated_us": time.time_ns() // 1000}
                status.write_text(json.dumps(self_status))
                assert start_ok() is False
                assert reason().startswith("shared_nas_low_space")
                self_status["free_bytes"] = 10 * 2**30
                self_status["updated_us"] = 1
                status.write_text(json.dumps(self_status))
                assert start_ok() is False
                assert reason().startswith("nas_snapshot_stale")
                self_status["updated_us"] = time.time_ns() // 1000 + 60_000_000
                status.write_text(json.dumps(self_status))
                assert start_ok() is False
                assert reason().startswith("nas_snapshot_future")
                status.write_text("{")
                assert start_ok() is False
                assert reason().startswith("nas_snapshot_invalid")
                self_status["updated_us"] = time.time_ns() // 1000
                self_status["mount_point"] = "/wrong"
                status.write_text(json.dumps(self_status))
                assert start_ok() is False
                assert reason().startswith("nas_snapshot_wrong_mount")
                self_status["mount_point"] = str(nas)
                status.write_text(json.dumps(self_status))
                assert start_ok() is True
                assert reason() == "ok"
                h.request(ports[1], "POST", "/api/record/stop-all")
                status.unlink()
                assert start_ok() is False
                assert reason().startswith("nas_snapshot_unreadable")
                print("shared NAS low/stale/wrong-path/missing snapshots rejected; recovery accepted")
            finally:
                proc.terminate()
                proc.wait(timeout=15)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--receiver", required=True)
    receiver = p.parse_args().receiver
    run(receiver)
    exercise_recording_retry(receiver, True)
    exercise_recording_retry(receiver, False)
