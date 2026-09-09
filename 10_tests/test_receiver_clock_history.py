#!/usr/bin/env python3
"""Inject delayed-frame clock reports through real UDP/TCP and check final CSV."""
import argparse
import csv
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time

import test_receiver_hardening as h


def run(receiver, holdover=False):
    with tempfile.TemporaryDirectory(prefix="gwv3_clock_history_") as tmp:
        root = Path(tmp)
        ports = {name: h.free_port(kind) for name, kind in (
            ("status", socket.SOCK_DGRAM), ("clock", socket.SOCK_DGRAM),
            ("media", socket.SOCK_STREAM), ("admin", socket.SOCK_STREAM))}
        assert len(set(ports.values())) == 4
        cfg = {"status_bind_ip": "127.0.0.1", "status_port": ports["status"],
               "media_bind_ip": "127.0.0.1", "media_port": ports["media"],
               "admin_bind_ip": "127.0.0.1", "admin_port": ports["admin"],
               "receiver_discovery": {"enabled": False},
               "clock_sync": {"enabled": True, "bind_ip": "127.0.0.1", "port": ports["clock"]},
               "nas_auto_mount": {"enabled": False}, "preview_enabled": False,
               "nas_root": str(root / "nas"), "state_path": str(root / "state.json"),
               "log_directory": str(root / "logs"), "task_audio": {"enabled": False},
               "recording_start_lead_ms": 0, "recording_stop_drain_timeout_ms": 1000,
               "recording_staging": {"enabled": False}}
        config = root / "receiver.json"
        config.write_text(json.dumps(cfg))
        payload = h.generate_h264_fixture(1)
        with (root / "stdout.log").open("wb") as log:
            proc = subprocess.Popen([receiver, "--config", str(config)], stdout=log, stderr=log)
            try:
                h.wait_http(ports["admin"])

                def api(method, path):
                    code, _, raw = h.request(ports["admin"], method, path)
                    assert code == 200, (code, raw)
                    return json.loads(raw)

                def wait_for(predicate):
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        state = api("GET", "/api/status")
                        if predicate(state):
                            return state
                        time.sleep(.02)
                    raise AssertionError(state)

                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                    probe.settimeout(2)
                    probe.sendto(json.dumps({"protocol_version": "3.0", "message_type": "clock_sync_probe",
                        "sender_id": "clock-history", "sequence": 1,
                        "t1_sender_send_us": time.time_ns() // 1000}).encode(), ("127.0.0.1", ports["clock"]))
                    probe.recv(4096)

                def report(reference, offset, drift):
                    h.status_message(ports["status"], {"protocol_version": "3.0", "message_type": "clock_sync_report",
                        "sender_id": "clock-history", "clock_sync_valid": True, "clock_offset_us": offset,
                        "clock_delay_us": 1000, "clock_drift_ppm": drift, "clock_last_sync_us": reference})
                    wait_for(lambda s: any(m["clock_last_sync_us"] == reference for m in s["clock_sync"]))

                h.status_message(ports["status"], {"protocol_version": "3.0", "message_type": "camera_announce",
                    "sender_id": "clock-history", "camera_id": "cam01",
                    "rgb_profile": {"width": 64, "height": 48, "fps": 30},
                    "depth_profile": {"width": 64, "height": 48, "fps": 30, "depth_scale": 1}})
                time.sleep(.05)
                with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as media:
                    def rgbd(fid, stamp):
                        media.sendall(h.rgb_packet("clock-history", "cam01", fid, 64, 48, stamp, payload))
                        media.sendall(h.depth_packet("clock-history", "cam01", fid, 64, 48, stamp))
                    rgbd(1, time.time_ns() // 1000)
                    wait_for(lambda s: s["cameras"] and s["cameras"][0]["live"])
                    start = api("POST", "/api/record/start-all")
                    time.sleep(max(0, start["recording_start_us"] / 1e6 - time.time()) + .05)
                    capture = time.time_ns() // 1000
                    reference = capture - (11000000 if holdover else 1000000)
                    report(reference, 14611, -200)
                    rgbd(2, capture)
                    wait_for(lambda s: s["cameras"][0]["record_enqueued_packets"] >= 2)
                    # Advance only the synthetic clock report, not the OS clock.
                    # This is the same negative elapsed time as a six-minute TCP backlog.
                    report(capture + 369000000, 13845, 138.313)
                    rgbd(3, capture + 33333)
                    rgbd(4, capture + 66666)
                    time.sleep(.12)
                    end = api("POST", "/api/record/stop-all")["recording_end_global_us"]
                    rgbd(5, end + 100000)
                    wait_for(lambda s: not s["cameras"][0]["record_tail_draining"]
                             and not s["cameras"][0]["record_finalizing"]
                             and not s["record_finalize_outstanding_segments"])
                files = list((root / "nas").rglob("frames.csv"))
                assert len(files) == 1, files
                with files[0].open(newline="") as f:
                    rows = list(csv.DictReader(f))
                for stream in ("rgb", "depth"):
                    group = [r for r in rows if r["stream_type"] == stream]
                    assert [r["frame_id"] for r in group] == ["2", "3", "4"], group
                    for row in group:
                        assert row["clock_sync_valid"] == ("0" if holdover else "1"), row
                        assert row["clock_mapping_version"] == "2", row
                        assert int(row["clock_model_reference_timestamp_us"]) == reference, row
                        assert int(row["clock_applied_offset_us"]) == 14611, row
                        assert int(row["global_timestamp_us"]) == int(row["frame_system_timestamp_us"]) + 14611, row
                    assert all(int(b["global_timestamp_us"]) - int(a["global_timestamp_us"]) == 33333
                               for a, b in zip(group, group[1:])), group
                assert [r["rgb_video_frame_index"] for r in rows if r["stream_type"] == "rgb"] == ["0", "1", "2"]
                print(f"PASS final RGBD CSV uses one historical clock snapshot, holdover={holdover}", flush=True)
            except Exception:
                print((root / "stdout.log").read_text(errors="replace")[-10000:], flush=True)
                raise
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
                    raise AssertionError("receiver shutdown hung")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--receiver", required=True)
    receiver = parser.parse_args().receiver
    run(receiver)
    run(receiver, holdover=True)
