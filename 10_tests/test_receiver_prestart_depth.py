#!/usr/bin/env python3
"""Keep depth received before a delayed first RGB keyframe, on isolated ports."""
import argparse
import csv
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time

import test_receiver_hardening as h


def exercise(receiver, scenario):
    with tempfile.TemporaryDirectory(prefix="gwv3_prestart_") as temporary:
        root = Path(temporary)
        ports = {"status": h.free_port(socket.SOCK_DGRAM), "media": h.free_port(socket.SOCK_STREAM),
                 "admin": h.free_port(socket.SOCK_STREAM)}
        assert len(set(ports.values())) == 3
        cfg = {
            "status_bind_ip": "127.0.0.1", "status_port": ports["status"],
            "media_bind_ip": "127.0.0.1", "media_port": ports["media"],
            "admin_bind_ip": "127.0.0.1", "admin_port": ports["admin"],
            "receiver_discovery": {"enabled": False}, "clock_sync": {"enabled": False},
            "nas_auto_mount": {"enabled": False}, "preview_enabled": False,
            "nas_root": str(root / "nas"), "state_path": str(root / "state.json"),
            "log_directory": str(root / "logs"), "segment_seconds": 900,
            "recording_start_lead_ms": 0, "recording_stop_drain_timeout_ms": 1000,
            "record_queue_max_mb": 4, "record_queue_total_max_mb": 8,
            "recording_staging": {"enabled": False, "idle_finalize_ms": 10000},
            "task_audio": {"enabled": False},
        }
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

                def wait(predicate, timeout=10):
                    end = time.monotonic() + timeout
                    while time.monotonic() < end:
                        value = api("GET", "/api/status")
                        if predicate(value):
                            return value
                        time.sleep(.02)
                    raise AssertionError(value)

                h.status_message(ports["status"], {"message_type": "camera_announce", "protocol_version": "3.0",
                    "sender_id": "prestart-test", "camera_id": "cam01",
                    "rgb_profile": {"width": 64, "height": 48, "fps": 30},
                    "depth_profile": {"width": 64, "height": 48, "fps": 30, "depth_scale": 1}})
                time.sleep(.1)
                with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as rgb_socket, \
                     socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as depth_socket:
                    def rgb(fid, stamp):
                        rgb_socket.sendall(h.rgb_packet("prestart-test", "cam01", fid, 64, 48, stamp, payload))

                    def depth(fid, stamp):
                        depth_socket.sendall(h.depth_packet("prestart-test", "cam01", fid, 64, 48, stamp))

                    rgb(1, time.time_ns() // 1000)
                    depth(1, time.time_ns() // 1000)
                    wait(lambda s: s["cameras"] and s["cameras"][0]["rgb_packets"] > 0)
                    start = api("POST", "/api/record/start-all")["recording_start_us"]
                    time.sleep(max(0, start / 1e6 - time.time()) + .01)
                    count = 1100 if scenario == "bounded" else 60
                    stamps = [start + 1000 + i * (1 if scenario == "bounded" else 33333) for i in range(count)]
                    for i, stamp in enumerate(stamps):
                        depth(100 + i, stamp)
                    state = wait(lambda s: s["cameras"][0]["record_dequeued_packets"] >= count
                                 and s["cameras"][0]["record_active_writes"] == 0)
                    cam = state["cameras"][0]
                    assert not cam["segment_active"], "depth must not open a non-decodable RGBD directory"
                    assert cam.get("record_prestart_depth_packets", 0) > 0, "depth was discarded while RGB was delayed"
                    if scenario == "bounded":
                        assert 0 < cam["record_prestart_depth_bytes"] <= 1024 * 1024, cam
                        assert cam["segment_prestart_depth_drops"] > 0, cam
                        assert cam["record_prestart_depth_packets"] + cam["segment_prestart_depth_drops"] == count, cam
                    else:
                        assert cam["record_prestart_depth_packets"] == count, cam
                    time.sleep(max(0, stamps[-1] / 1e6 - time.time()) + .1)
                    if scenario == "delayed":
                        for i, stamp in enumerate(stamps):
                            rgb(100 + i, stamp)
                        wait(lambda s: s["cameras"][0]["record_prestart_depth_replay_attempts"] == count
                             and s["cameras"][0]["record_prestart_depth_packets"] == 0
                             and s["record_queue_total_bytes"] == 0)
                    end = api("POST", "/api/record/stop-all")["recording_end_global_us"]
                    rgb(2000, end + 1)
                    depth(2000, end + 1)
                    wait(lambda s: not s["cameras"][0]["record_finalizing"]
                         and not s["record_finalize_outstanding_segments"] and s["record_queue_total_bytes"] == 0)
                    if scenario == "restart":
                        start2 = api("POST", "/api/record/start-all")["recording_start_us"]
                        stamp = max(time.time_ns() // 1000, start2 + 1)
                        rgb(3000, stamp)
                        depth(3000, stamp)
                        wait(lambda s: s["cameras"][0]["segment_active"] and s["record_queue_total_bytes"] == 0)
                        end2 = api("POST", "/api/record/stop-all")["recording_end_global_us"]
                        rgb(3001, end2 + 1)
                        depth(3001, end2 + 1)
                        wait(lambda s: not s["cameras"][0]["record_finalizing"]
                             and not s["record_finalize_outstanding_segments"] and s["record_queue_total_bytes"] == 0)
                    if scenario != "bounded":
                        # Detaching permits a new session before the old writer
                        # publishes files. Only the ready marker means delivery.
                        wait(lambda s: len(list((root / "nas").rglob("recording_ready.json"))) == 1)
                    files = list((root / "nas").rglob("frames.csv"))
                    assert len(files) == (0 if scenario == "bounded" else 1), files
                    if files:
                        rows = list(csv.DictReader(files[0].open(newline="")))
                        expected = list(range(100, 160)) if scenario == "delayed" else [3000]
                        for stream in ("rgb", "depth"):
                            ids = [int(r["frame_id"]) for r in rows if r["stream_type"] == stream]
                            assert ids == expected, (stream, ids, expected)
                        meta = json.loads((files[0].parent / "meta.json").read_text())
                        assert "depth first frame is late" not in meta["recording_quality_reason"], meta
                        assert (files[0].parent / "recording_ready.json").exists()
                    print("PASS prestart depth", scenario, flush=True)
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
    args = parser.parse_args()
    for scenario in ("delayed", "bounded", "restart"):
        exercise(args.receiver, scenario)
