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
    create_ffmpeg_test_wrappers,
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
        print("receiver direct NAS integration test skipped: ffmpeg/ffprobe unavailable")
        return

    with tempfile.TemporaryDirectory(prefix="gwv3_receiver_direct_nas_") as temporary_text:
        temporary = Path(temporary_text)
        external_nas_root = bool(args.nas_root)
        if external_nas_root:
            nas_parent = Path(args.nas_root)
            nas_parent.mkdir(parents=True, exist_ok=True)
            nas_root = Path(tempfile.mkdtemp(prefix=".gwv3_direct_nas_test_", dir=nas_parent))
        else:
            nas_root = temporary / "nas"
        hidden_name = ".gwv3_direct_inprogress"
        hidden_root = nas_root / hidden_name
        recovered_source = hidden_root / "recovery-test_cam01" / "2026-08-13" / "010203"
        recovered_source.mkdir(parents=True)
        (recovered_source / "recording_ready.json").write_text(
            json.dumps({"ready": True}), encoding="utf-8"
        )
        partial_source = hidden_root / "partial-test_cam01" / "2026-08-13" / "010204"
        partial_source.mkdir(parents=True)
        (partial_source / "rgb.mp4").write_bytes(b"partial")

        ports = {
            "status": free_port(socket.SOCK_DGRAM),
            "media": free_port(socket.SOCK_STREAM),
            "media_udp": free_port(socket.SOCK_DGRAM),
            "clock": free_port(socket.SOCK_DGRAM),
            "admin": free_port(socket.SOCK_STREAM),
        }
        ffmpeg_wrapper = create_ffmpeg_test_wrappers(temporary)
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
            "clock_sync": {
                "enabled": True,
                "bind_ip": "127.0.0.1",
                "port": ports["clock"],
                "model_timeout_ms": 10000,
            },
            "admin_bind_ip": "127.0.0.1",
            "admin_port": ports["admin"],
            "nas_root": str(nas_root),
            "recording_staging": {
                "enabled": False,
                "root": str(temporary / "unused-staging"),
                "defer_player_compatible_finalize": True,
                "rgb_output_mode": "fragmented_mp4",
                "idle_finalize_ms": 1000,
                "direct_publish_hidden_directory": hidden_name,
            },
            "log_directory": str(temporary / "logs"),
            "state_path": str(temporary / "state.json"),
            "ffmpeg_path": str(ffmpeg_wrapper),
            "segment_seconds": 30,
            "recording_start_lead_ms": 0,
            "depth_fps": 30,
            "max_payload_mb": 8,
            "record_queue_max_mb": 16,
            "record_queue_total_max_mb": 32,
            "record_finalize_max_pending_segments": 4,
            "record_finalize_workers": 2,
            "min_free_disk_mb": 0,
        }
        config_path = temporary / "receiver.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        receiver_log_path = temporary / "receiver.log"
        receiver_log = receiver_log_path.open("wb")
        receiver = subprocess.Popen(
            [args.receiver, "--config", str(config_path)],
            stdout=receiver_log,
            stderr=subprocess.STDOUT,
        )
        try:
            wait_http(ports["admin"])
            assert (nas_root / "recovery-test_cam01" / "2026-08-13" / "010203" / "recording_ready.json").is_file()
            assert not recovered_source.exists()
            assert partial_source.is_dir()
            assert not (nas_root / "partial-test_cam01").exists()

            status_message(
                ports["status"],
                {
                    "protocol_version": "3.0",
                    "message_type": "camera_announce",
                    "sender_id": "direct-test",
                    "camera_id": "cam01",
                    "rgb_profile": {"width": 64, "height": 48, "fps": 30},
                    "depth_profile": {"width": 64, "height": 48, "fps": 30, "depth_scale": 1},
                },
            )
            time.sleep(0.1)
            assert request(ports["admin"], "POST", "/api/record/start-all")[0] == 200
            fixture = generate_h264_fixture(1)
            timestamp = int(time.time() * 1_000_000)
            with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as media:
                for frame_id in range(90):
                    media.sendall(
                        rgb_packet(
                            "direct-test",
                            "cam01",
                            frame_id,
                            64,
                            48,
                            timestamp + frame_id * 33333,
                            fixture,
                        )
                    )
                for frame_id in range(90):
                    media.sendall(
                        depth_packet(
                            "direct-test",
                            "cam01",
                            frame_id,
                            64,
                            48,
                            timestamp + frame_id * 33333,
                        )
                    )

                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    current = json.loads(request(ports["admin"], "GET", "/api/status")[2])
                    camera = next(
                        (
                            item
                            for item in current.get("cameras", [])
                            if item.get("sender_id") == "direct-test" and item.get("camera_id") == "cam01"
                        ),
                        None,
                    )
                    if (
                        camera is not None
                        and camera.get("segment_active")
                        and int(camera.get("record_queue_packets") or 0) == 0
                        and int(camera.get("record_active_writes") or 0) == 0
                    ):
                        break
                    time.sleep(0.05)
                else:
                    raise AssertionError("receiver did not drain direct NAS test media")

            assert list(hidden_root.rglob("rgb.mp4")), "active direct recording was not hidden"
            assert not (nas_root / "direct-test_cam01").exists(), "active direct recording leaked into final path"
            assert request(ports["admin"], "POST", "/api/record/stop-all", timeout=10)[0] == 200

            deadline = time.monotonic() + 20
            ready_files = []
            while time.monotonic() < deadline:
                current = json.loads(request(ports["admin"], "GET", "/api/status")[2])
                ready_files = list((nas_root / "direct-test_cam01").rglob("recording_ready.json"))
                if int(current.get("record_finalize_outstanding_segments") or 0) == 0 and ready_files:
                    break
                assert receiver.poll() is None, "receiver exited during direct NAS publication"
                time.sleep(0.1)
            else:
                raise AssertionError("direct NAS recording was not atomically published")

            assert_recording_output(
                nas_root / "direct-test_cam01",
                minimum_rgb_duration=2.0,
                expected_rgb_container="fragmented_mp4",
            )
            assert not list(hidden_root.rglob("recording_ready.json"))
            status = json.loads(request(ports["admin"], "GET", "/api/status")[2])
            assert status["recording_staging_enabled"] is False
            assert status["recording_write_root"] == str(hidden_root)
            assert status["recording_staging"]["direct_publish_hidden_directory"] == hidden_name
        except Exception:
            receiver_log.flush()
            print(receiver_log_path.read_text(encoding="utf-8", errors="replace"))
            raise
        finally:
            receiver.terminate()
            receiver.wait(timeout=15)
            receiver_log.close()
            if receiver.returncode not in {0, -15}:
                print(receiver_log_path.read_text(encoding="utf-8", errors="replace"))
            if external_nas_root:
                shutil.rmtree(nas_root)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--receiver", required=True)
    parser.add_argument("--nas-root", help="optional mounted NAS directory used for the direct publication test")
    return parser.parse_args()


if __name__ == "__main__":
    run(parse_args())
