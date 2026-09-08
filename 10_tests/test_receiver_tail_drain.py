#!/usr/bin/env python3
"""Exercise late RGBD tails on isolated receiver ports and temporary storage."""
import argparse
import csv
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time

import test_receiver_hardening as h


def exercise(receiver, scope, timeout=False, restart=False, rgb_only=False,
             first_in_flight=False, shutdown=False):
    with tempfile.TemporaryDirectory(prefix="gwv3_tail_drain_") as tmp:
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
            "recording_start_lead_ms": 0, "recording_stop_drain_timeout_ms": 1500,
            "recording_staging": {"enabled": False, "idle_finalize_ms": 10000},
            "task_audio": {"enabled": False},
        }
        config = root / "receiver.json"
        config.write_text(json.dumps(cfg), encoding="ascii")
        payload = h.generate_h264_fixture(1)
        with (root / "stdout.log").open("wb") as log:
            proc = subprocess.Popen([receiver, "--config", str(config)], stdout=log, stderr=log)
            try:
                h.wait_http(ports["admin"])

                def api(method, path):
                    status, _, raw = h.request(ports["admin"], method, path, timeout=2)
                    assert status == 200, (status, raw)
                    result = json.loads(raw)
                    assert result.get("ok", True), result
                    return result

                def wait_for(predicate, seconds=8):
                    deadline = time.monotonic() + seconds
                    while time.monotonic() < deadline:
                        value = api("GET", "/api/status")
                        if predicate(value):
                            return value
                        time.sleep(.02)
                    raise AssertionError(value)

                announce = {"protocol_version": "3.0", "message_type": "camera_announce",
                            "sender_id": "tail-test", "camera_id": "cam01",
                            "rgb_profile": {"width": 64, "height": 48, "fps": 30}}
                if not rgb_only:
                    announce["depth_profile"] = {"width": 64, "height": 48, "fps": 30, "depth_scale": 1}
                h.status_message(ports["status"], announce)
                time.sleep(.1)
                with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as media:
                    def rgb(fid, stamp):
                        media.sendall(h.rgb_packet("tail-test", "cam01", fid, 64, 48, stamp, payload))

                    def depth(fid, stamp):
                        if not rgb_only:
                            media.sendall(h.depth_packet("tail-test", "cam01", fid, 64, 48, stamp))

                    rgb(1, time.time_ns() // 1000)
                    depth(1, time.time_ns() // 1000)
                    wait_for(lambda d: d["cameras"] and d["cameras"][0]["rgb_packets"] >= 1)
                    suffix = {"all": "-all", "sender": "-sender?sender_id=tail-test",
                              "camera": "?sender_id=tail-test&camera_id=cam01"}[scope]
                    start = api("POST", "/api/record/start" + suffix)
                    time.sleep(max(0, start["recording_start_us"] / 1e6 - time.time()) + .05)
                    for fid in (() if first_in_flight else (2, 3, 4)):
                        rgb(fid, time.time_ns() // 1000)
                        depth(fid, time.time_ns() // 1000)
                        time.sleep(.035)
                    if not first_in_flight:
                        wait_for(lambda d: d["cameras"][0]["record_enqueued_packets"] >= (3 if rgb_only else 6)
                                 and d["cameras"][0]["record_queue_packets"] == 0)
                    late_stamp = time.time_ns() // 1000
                    time.sleep(.01)
                    t0 = time.monotonic()
                    stop = api("POST", "/api/record/stop" + suffix)
                    assert time.monotonic() - t0 < .5, "stop API blocked on media tail"
                    end = stop["recording_end_global_us"]
                    assert start["recording_start_us"] < late_stamp < end
                    if shutdown:
                        assert api("GET", "/api/status")["cameras"][0]["record_tail_draining"]
                        proc.terminate()
                        proc.wait(timeout=5)
                        assert proc.returncode == 0, proc.returncode
                        print("PASS shutdown during tail drain", flush=True)
                        return
                    if restart:
                        pending = api("POST", "/api/record/start" + suffix)
                        assert pending.get("start_pending", True), pending
                    # Repeated stop must not extend the old task's end or cancel its drain.
                    if not restart:
                        time.sleep(.08)
                        api("POST", "/api/record/stop" + suffix)
                    rgb(5, late_stamp)
                    rgb(6, end + 1)
                    time.sleep(.12)
                    if not rgb_only:
                        value = api("GET", "/api/status")["cameras"][0]
                        assert value.get("record_tail_draining"), ("RGB watermark closed a pending depth tail", value)
                    if not timeout:
                        depth(5, late_stamp)
                        depth(6, end + 1)
                    value = wait_for(lambda d: not d["cameras"][0].get("record_tail_draining", False)
                                     and not d["record_finalize_outstanding_segments"]
                                     and not d["cameras"][0]["record_finalizing"])
                    if restart:
                        value = wait_for(lambda d: d["cameras"][0]["recording_start_us"] > end)
                        stamp = max(time.time_ns() // 1000, value["cameras"][0]["recording_start_us"] + 1)
                        rgb(20, stamp)
                        depth(20, stamp)
                        wait_for(lambda d: d["cameras"][0]["segment_active"])
                        time.sleep(.02)
                        end2 = api("POST", "/api/record/stop" + suffix)["recording_end_global_us"]
                        rgb(21, end2 + 1)
                        depth(21, end2 + 1)
                        wait_for(lambda d: not d["cameras"][0]["record_finalizing"]
                                 and not d["record_finalize_outstanding_segments"])
                    files = sorted((root / "nas").rglob("frames.csv"))
                    assert len(files) == (2 if restart else 1), files
                    old = next(p for p in files if json.loads((p.parent / "meta.json").read_text())[
                        "recording_window_end_global_us"] == end)
                    with old.open(newline="", encoding="utf-8-sig") as f:
                        rows = list(csv.DictReader(f))
                    ids = {s: [int(r["frame_id"]) for r in rows if r["stream_type"] == s]
                           for s in ("rgb", "depth")}
                    initial = [] if first_in_flight else [2, 3, 4]
                    assert ids["rgb"] == initial + [5], ids
                    assert ids["depth"] == ([] if rgb_only else initial if timeout else initial + [5]), ids
                    meta = json.loads((old.parent / "meta.json").read_text())
                    assert ("tail drain" in meta["recording_quality_reason"]) == timeout, meta
                    if restart:
                        new = next(p for p in files if p != old)
                        with new.open(newline="", encoding="utf-8-sig") as f:
                            assert {r["frame_id"] for r in csv.DictReader(f)} == {"20"}
                    print("PASS", scope, "timeout", timeout, "restart", restart, "rgb_only", rgb_only,
                          "first_in_flight", first_in_flight, flush=True)
            except Exception:
                print((root / "stdout.log").read_text(errors="replace")[-12000:], flush=True)
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
    for scope in ("all", "sender", "camera"):
        exercise(args.receiver, scope, restart=True)
        exercise(args.receiver, scope, timeout=True)
    exercise(args.receiver, "all", rgb_only=True)
    exercise(args.receiver, "all", first_in_flight=True)
    exercise(args.receiver, "all", shutdown=True)
