#!/usr/bin/env python3
"""A media outage must not detach an active recording after five seconds."""
import argparse
import csv
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time

import test_receiver_hardening as h


def exercise(receiver, tail=False, expire=False, pause_seconds=6.2):
    with tempfile.TemporaryDirectory(prefix="gwv3_media_recovery_") as tmp:
        root = Path(tmp)
        ports = {"status": h.free_port(socket.SOCK_DGRAM),
                 "media": h.free_port(socket.SOCK_STREAM), "admin": h.free_port(socket.SOCK_STREAM)}
        assert len(set(ports.values())) == 3
        cfg = {
            "status_bind_ip": "127.0.0.1", "status_port": ports["status"],
            "media_bind_ip": "127.0.0.1", "media_port": ports["media"],
            "admin_bind_ip": "127.0.0.1", "admin_port": ports["admin"],
            "receiver_discovery": {"enabled": False}, "clock_sync": {"enabled": False},
            "nas_auto_mount": {"enabled": False}, "preview_enabled": False,
            "nas_root": str(root / "nas"), "state_path": str(root / "state.json"),
            "log_directory": str(root / "logs"), "segment_seconds": 900,
            "recording_start_lead_ms": 0, "recording_stop_drain_timeout_ms": 3000,
            "recording_staging": {"enabled": False, "idle_finalize_ms": 1000 if tail or expire else 5000},
            "task_audio": {"enabled": False},
        }
        if expire:
            cfg["recording_staging"]["media_recovery_grace_ms"] = 1500
        config = root / "receiver.json"
        config.write_text(json.dumps(cfg))
        payload = h.generate_h264_fixture(1)
        with (root / "stdout.log").open("wb") as log:
            proc = subprocess.Popen([receiver, "--config", str(config)], stdout=log, stderr=log)
            try:
                h.wait_http(ports["admin"])

                def api(method, path):
                    code, _, raw = h.request(ports["admin"], method, path, timeout=3)
                    assert code == 200, (code, raw)
                    value = json.loads(raw)
                    assert value.get("ok", True), value
                    return value

                def wait(predicate):
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        value = api("GET", "/api/status")
                        if predicate(value):
                            return value
                        time.sleep(.02)
                    raise AssertionError(value)

                h.status_message(ports["status"], {"message_type": "camera_announce", "protocol_version": "3.0",
                    "sender_id": "recovery-test", "camera_id": "cam01",
                    "rgb_profile": {"width": 64, "height": 48, "fps": 30},
                    "depth_profile": {"width": 64, "height": 48, "fps": 30, "depth_scale": 1}})
                time.sleep(.1)
                with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as media:
                    def pair(fid, stamp):
                        media.sendall(h.rgb_packet("recovery-test", "cam01", fid, 64, 48, stamp, payload))
                        media.sendall(h.depth_packet("recovery-test", "cam01", fid, 64, 48, stamp))

                    pair(1, time.time_ns() // 1000)
                    wait(lambda s: s["cameras"] and s["cameras"][0]["rgb_packets"] > 0)
                    start = api("POST", "/api/record/start-all")["recording_start_us"]
                    time.sleep(max(0, start / 1e6 - time.time()) + .02)
                    stamp = time.time_ns() // 1000
                    pair(10, stamp)
                    state = wait(lambda s: s["cameras"][0]["segment_active"] and s["record_queue_total_bytes"] == 0)
                    directory = state["cameras"][0]["segment_dir"]
                    time.sleep(.05)
                    late_stamp = time.time_ns() // 1000
                    if tail:
                        end = api("POST", "/api/record/stop-all")["recording_end_global_us"]
                    time.sleep(2.2 if expire else 1.4 if tail else pause_seconds)
                    cam = api("GET", "/api/status")["cameras"][0]
                    if expire:
                        assert cam["media_idle_finalizations"] == 1, cam
                        assert cam["recording"], "expiry must not cancel the recording session"
                        wait(lambda s: not s["cameras"][0]["record_finalizing"])
                    else:
                        assert cam["segment_active"] and cam["segment_dir"] == directory, cam
                        assert cam["media_idle_finalizations"] == 0, cam
                    if tail:
                        assert cam["record_tail_draining"], cam
                    elif not expire:
                        assert cam["recording"] and cam["record_media_stalled"], cam
                    pair(11, late_stamp)
                    wait(lambda s: s["cameras"][0]["record_dequeued_packets"] >= 4)
                    if not tail:
                        end = api("POST", "/api/record/stop-all")["recording_end_global_us"]
                    pair(12, end + 1)
                    wait(lambda s: not s["cameras"][0]["record_tail_draining"]
                         and not s["cameras"][0]["record_finalizing"]
                         and not s["record_finalize_outstanding_segments"])
                    def published():
                        return [p for p in (root / "nas").rglob("recording_ready.json")
                                if not any(x.startswith(".") for x in p.relative_to(root / "nas").parts)]
                    wait(lambda s: len(published()) == (2 if expire else 1))
                    rows = []
                    for marker in published():
                        with (marker.parent / "frames.csv").open() as source:
                            rows.extend(csv.DictReader(source))
                    for stream in ("rgb", "depth"):
                        assert sorted(int(r["frame_id"]) for r in rows if r["stream_type"] == stream) == [10, 11], rows
                    print("PASS", "bounded expiry and resume" if expire else "stop-tail isolation" if tail else f"{pause_seconds}-second media recovery", flush=True)
            except Exception:
                print((root / "stdout.log").read_text(errors="replace")[-4000:], flush=True)
                raise
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=3)
                    raise AssertionError("receiver shutdown hung")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--receiver", required=True)
    parser.add_argument("--pause-seconds", type=float, default=6.2)
    args = parser.parse_args()
    exercise(args.receiver, pause_seconds=args.pause_seconds)
    exercise(args.receiver, tail=True)
    exercise(args.receiver, expire=True)
