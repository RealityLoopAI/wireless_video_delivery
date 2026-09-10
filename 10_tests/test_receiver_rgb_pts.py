#!/usr/bin/env python3
"""A recording packet's capture time, not frame count, determines MP4 PTS."""
import argparse
import csv
import json
from pathlib import Path
import socket
import shutil
import subprocess
import tempfile
import time
import sys

import test_receiver_hardening as h
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "05_tools"))
import recording_uploader


def exercise(receiver, mode="fragmented_mp4", recovery=False):
    with tempfile.TemporaryDirectory(prefix="gwv3_rgb_pts_") as temporary:
        root = Path(temporary)
        ports = {"status": h.free_port(socket.SOCK_DGRAM), "media": h.free_port(socket.SOCK_STREAM),
                 "admin": h.free_port(socket.SOCK_STREAM)}
        cfg = {
            "status_bind_ip": "127.0.0.1", "status_port": ports["status"],
            "media_bind_ip": "127.0.0.1", "media_port": ports["media"],
            "admin_bind_ip": "127.0.0.1", "admin_port": ports["admin"],
            "receiver_discovery": {"enabled": False}, "clock_sync": {"enabled": False},
            "nas_auto_mount": {"enabled": False}, "preview_enabled": False,
            "nas_root": str(root / "nas"), "state_path": str(root / "state.json"),
            "log_directory": str(root / "logs"), "segment_seconds": 900,
            "recording_start_lead_ms": 0, "recording_stop_drain_timeout_ms": 1000,
            "recording_staging": {"enabled": False, "idle_finalize_ms": 10000,
                                  "rgb_output_mode": mode},
            "write_debug_h264": recovery,
            "task_audio": {"enabled": False},
        }
        if recovery:
            wrapper = root / "ffmpeg_fail_live.py"
            wrapper.write_text("#!/usr/bin/env python3\nimport os,sys\n"
                               "if 'nut' in sys.argv and 'pipe:0' in sys.argv: sys.exit(23)\n"
                               f"os.execv({shutil.which('ffmpeg')!r}, ['ffmpeg']+sys.argv[1:])\n")
            wrapper.chmod(0o755)
            cfg["ffmpeg_path"] = str(wrapper)
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
                    return json.loads(raw)

                def wait(predicate):
                    end = time.monotonic() + 15
                    while time.monotonic() < end:
                        value = api("GET", "/api/status")
                        if predicate(value):
                            return value
                        time.sleep(.02)
                    raise AssertionError(value)

                def published(name):
                    return [p for p in (root / "nas").rglob(name)
                            if ".gwv3_direct_inprogress" not in p.parts]

                h.status_message(ports["status"], {"message_type": "camera_announce", "protocol_version": "3.0",
                    "sender_id": "pts-test", "camera_id": "cam01",
                    "rgb_profile": {"width": 64, "height": 48, "fps": 30}})
                time.sleep(.1)
                with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as sock:
                    def send(fid, stamp):
                        sock.sendall(h.rgb_packet("pts-test", "cam01", fid, 64, 48, stamp, payload))

                    send(1, time.time_ns() // 1000)
                    wait(lambda s: s["cameras"] and s["cameras"][0]["rgb_packets"] > 0)
                    start = api("POST", "/api/record/start-all")["recording_start_us"]
                    offsets = [200000, 233333, 266666, 1266666, 1299999]
                    time.sleep(max(0, (start + offsets[-1]) / 1e6 - time.time()) + .02)
                    for i, offset in enumerate(offsets):
                        send(100+i, start+offset)
                    wait(lambda s: s["cameras"][0]["record_dequeued_packets"] >= len(offsets)
                         and not s["record_queue_total_bytes"])
                    end = api("POST", "/api/record/stop-all")["recording_end_global_us"]
                    send(200, end+1)
                    wait(lambda s: len(published("recording_ready.json")) == 1)
                files = published("frames.csv")
                assert len(files) == 1, files
                rows = [r for r in csv.DictReader(files[0].open(newline=""))
                        if r["stream_type"] == "rgb" and r["rgb_recorded"] == "1"]
                assert [int(r["rgb_video_frame_index"]) for r in rows] == list(range(len(offsets)))
                assert [int(r["global_timestamp_us"])-start for r in rows] == offsets
                media = files[0].parent / "rgb.mp4"
                probe = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-select_streams", "v:0",
                    "-show_packets", "-show_entries", "packet=pts_time,dts_time", "-of", "json", str(media)], timeout=10))
                pts = [float(p["pts_time"]) for p in probe["packets"]]
                assert len(pts) == len(offsets), pts
                assert all(abs(a-b/1e6) < .001 for a,b in zip(pts,offsets)), (pts, offsets)
                subprocess.run(["ffmpeg", "-v", "error", "-i", str(media), "-f", "null", "-"], check=True, timeout=10)
                if mode == "fragmented_mp4":
                    remux_dir = root / "upload_remux"
                    remux_dir.mkdir()
                    remux_media = remux_dir / "rgb.mp4"
                    shutil.copyfile(media, remux_media)
                    recording_uploader.finalize_rgb(remux_dir, "rgb.mp4", shutil.which("ffmpeg"),
                        expected_duration=offsets[-1]/1e6+1/30, rgb_output_mode="conventional_mp4")
                    remux_probe = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-select_streams", "v:0",
                        "-show_packets", "-show_entries", "packet=pts_time", "-of", "json", str(remux_media)], timeout=10))
                    remux_pts = [float(p["pts_time"]) for p in remux_probe["packets"]]
                    assert len(remux_pts) == len(pts) and all(abs(a-b) < .001 for a,b in zip(remux_pts,pts)), remux_pts
                print("PASS RGB real PTS, one-second gap, delayed start and CSV index", mode, recovery, pts, flush=True)
            except Exception:
                print((root / "stdout.log").read_text(errors="replace")[-6000:])
                for p in (root / "nas").rglob("ffmpeg.log"):
                    print(p.read_text(errors="replace")[-3000:])
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
    exercise(args.receiver)
    exercise(args.receiver, mode="conventional_mp4")
    exercise(args.receiver, recovery=True)
