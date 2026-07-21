#!/usr/bin/env python3
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

from test_receiver_hardening import (
    assert_recording_output,
    depth_packet,
    free_port,
    generate_h264_fixture,
    request,
    rgb_packet,
    status_message,
    wait_http,
)


def run(args: argparse.Namespace) -> None:
    ffmpeg = shutil.which("ffmpeg")
    ffprobe = shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        print("receiver staging integration test skipped: ffmpeg/ffprobe unavailable")
        return

    with tempfile.TemporaryDirectory(prefix="gwv3_receiver_staging_test_") as temporary_text:
        temporary = Path(temporary_text)
        staging_root = temporary / "staging"
        nas_root = temporary / "nas"
        ports = {
            "status": free_port(socket.SOCK_DGRAM),
            "media": free_port(socket.SOCK_STREAM),
            "media_udp": free_port(socket.SOCK_DGRAM),
            "clock": free_port(socket.SOCK_DGRAM),
            "admin": free_port(socket.SOCK_STREAM),
        }
        config = {
            "status_bind_ip": "127.0.0.1",
            "status_port": ports["status"],
            "media_bind_ip": "127.0.0.1",
            "media_port": ports["media"],
            "preview_enabled": False,
            "media_udp_enabled": False,
            "media_udp_bind_ip": "127.0.0.1",
            "media_udp_port": ports["media_udp"],
            "preview_udp_enabled": False,
            "clock_sync": {"enabled": True, "bind_ip": "127.0.0.1", "port": ports["clock"], "model_timeout_ms": 10000},
            "admin_bind_ip": "127.0.0.1",
            "admin_port": ports["admin"],
            "nas_root": str(nas_root),
            "recording_staging": {
                "enabled": True,
                "root": str(staging_root),
                "defer_player_compatible_finalize": True,
                "idle_finalize_ms": 1000,
                "upload_interval_ms": 100,
                "delete_after_upload": True,
            },
            "log_directory": str(temporary / "logs"),
            "state_path": str(temporary / "state.json"),
            "ffmpeg_path": ffmpeg,
            "segment_seconds": 30,
            "depth_fps": 30,
            "max_payload_mb": 8,
            "record_queue_max_mb": 16,
            "record_queue_total_max_mb": 32,
            "record_finalize_max_pending_segments": 4,
            "min_free_disk_mb": 0,
        }
        config_path = temporary / "receiver.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        receiver_log = (temporary / "receiver.log").open("wb")
        uploader_log = (temporary / "uploader.log").open("wb")
        receiver = subprocess.Popen(
            [args.receiver, "--config", str(config_path)],
            stdout=receiver_log,
            stderr=subprocess.STDOUT,
        )
        uploader = subprocess.Popen(
            [args.python, args.uploader, "--config", str(config_path)],
            stdout=uploader_log,
            stderr=subprocess.STDOUT,
        )
        try:
            wait_http(ports["admin"])
            status_message(
                ports["status"],
                {
                    "protocol_version": "3.0",
                    "message_type": "camera_announce",
                    "sender_id": "staging-test",
                    "camera_id": "cam01",
                    "rgb_profile": {"width": 64, "height": 48, "fps": 30},
                    "depth_profile": {"width": 64, "height": 48, "fps": 30, "depth_scale": 1},
                },
            )
            time.sleep(0.1)
            assert request(ports["admin"], "POST", "/api/record/start-all")[0] == 200
            fixture = generate_h264_fixture(90)
            timestamp = int(time.time() * 1_000_000)
            with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as media:
                media.sendall(rgb_packet("staging-test", "cam01", 1, 64, 48, timestamp, fixture))
                for frame_id in range(75):
                    media.sendall(depth_packet("staging-test", "cam01", frame_id, 64, 48, timestamp + frame_id * 33333))
            assert request(ports["admin"], "POST", "/api/record/stop-all", timeout=10)[0] == 200

            deadline = time.monotonic() + 30
            ready_files = []
            while time.monotonic() < deadline:
                ready_files = list(nas_root.rglob("recording_ready.json"))
                if ready_files:
                    break
                assert receiver.poll() is None, "receiver exited during staged recording"
                assert uploader.poll() is None, "uploader exited during staged recording"
                time.sleep(0.1)
            else:
                raise AssertionError("staged recording was not finalized and published to NAS")

            assert_recording_output(nas_root, minimum_rgb_duration=2.0)
            assert not list(staging_root.rglob("recording_staged.json")), "published staged marker was not drained"
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                status = json.loads(request(ports["admin"], "GET", "/api/status")[2])
                if status["recording_uploader"].get("pending_segments") == 0:
                    break
                time.sleep(0.1)
            else:
                raise AssertionError("receiver did not refresh the drained uploader backlog status")
            assert status["recording_staging_enabled"] is True
            assert status["recording_write_root"] == str(staging_root)
        except Exception:
            receiver_log.flush()
            uploader_log.flush()
            print("receiver staging test log:\n" + (temporary / "receiver.log").read_text(encoding="utf-8", errors="replace"))
            print("uploader staging test log:\n" + (temporary / "uploader.log").read_text(encoding="utf-8", errors="replace"))
            raise
        finally:
            receiver.terminate()
            receiver.wait(timeout=10)
            uploader.terminate()
            uploader.wait(timeout=10)
            receiver_log.close()
            uploader_log.close()
            if receiver.returncode != 0:
                print((temporary / "receiver.log").read_text(encoding="utf-8", errors="replace"))
            if uploader.returncode not in {0, -15}:
                print((temporary / "uploader.log").read_text(encoding="utf-8", errors="replace"))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--receiver", required=True)
    parser.add_argument("--uploader", required=True)
    parser.add_argument("--python", default=os.sys.executable)
    return parser.parse_args()


if __name__ == "__main__":
    run(parse_args())
