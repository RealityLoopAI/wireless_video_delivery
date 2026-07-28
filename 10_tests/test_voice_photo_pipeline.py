#!/usr/bin/env python3
import argparse
import http.client
import json
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import time


MEDIA_MAGIC = 0x33565747
HEADER_VERSION = 2
HEADER_SIZE = 134
STREAM_RGB_SNAPSHOT = 4
PIXEL_ENCODED_VIDEO = 1
FLAG_SYSTEM_TIMESTAMP = 1 << 3
FLAG_RGB_DIAGNOSTICS = 1 << 4
FLAG_PIPELINE_DIAGNOSTICS = 1 << 5


def free_port(sock_type: int) -> int:
    with socket.socket(socket.AF_INET, sock_type) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_admin(port: int, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
            connection.request("GET", "/api/status")
            response = connection.getresponse()
            response.read()
            connection.close()
            if response.status == 200:
                return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError("receiver admin endpoint did not start")


def snapshot_packet(
    sender_id: str,
    camera_id: str,
    request_id: str,
    frame_id: int,
    system_timestamp_us: int,
    jpeg: bytes,
) -> bytes:
    sender = sender_id.encode("ascii")
    camera = camera_id.encode("ascii")
    codec = f"mjpeg;request_id={request_id}".encode("ascii")
    flags = FLAG_SYSTEM_TIMESTAMP | FLAG_RGB_DIAGNOSTICS | FLAG_PIPELINE_DIAGNOSTICS
    header = struct.pack(
        "<IHHBBIHHHQQQQIIHQQiiiiQQQQQ",
        MEDIA_MAGIC,
        HEADER_VERSION,
        HEADER_SIZE,
        STREAM_RGB_SNAPSHOT,
        0,
        flags,
        len(sender),
        len(camera),
        len(codec),
        frame_id,
        frame_id * 33333,
        system_timestamp_us,
        0,
        1280,
        800,
        PIXEL_ENCODED_VIDEO,
        len(jpeg),
        len(jpeg),
        312,
        80,
        0,
        30,
        system_timestamp_us,
        system_timestamp_us + 100,
        0,
        0,
        system_timestamp_us + 200,
    )
    assert len(header) == HEADER_SIZE
    return header + sender + camera + codec + jpeg


def run(args: argparse.Namespace) -> None:
    assert args.receiver.is_file(), f"receiver binary not found: {args.receiver}"
    ports = {
        "status": free_port(socket.SOCK_DGRAM),
        "media": free_port(socket.SOCK_STREAM),
        "media_udp": free_port(socket.SOCK_DGRAM),
        "preview_udp": free_port(socket.SOCK_DGRAM),
        "clock": free_port(socket.SOCK_DGRAM),
        "admin": free_port(socket.SOCK_STREAM),
    }
    with tempfile.TemporaryDirectory(prefix="gwv3_voice_photo_pipeline_") as temporary_text:
        temporary = Path(temporary_text)
        staging_root = temporary / "staging" / ".gwv3_photo_queue"
        nas_root = temporary / "nas"
        nas_root.mkdir(parents=True)
        config_path = temporary / "receiver.json"
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
            "preview_udp_bind_ip": "127.0.0.1",
            "preview_udp_port": ports["preview_udp"],
            "clock_sync": {
                "enabled": False,
                "bind_ip": "127.0.0.1",
                "port": ports["clock"],
                "model_timeout_ms": 10000,
            },
            "admin_bind_ip": "127.0.0.1",
            "admin_port": ports["admin"],
            "nas_root": str(nas_root),
            "recording_staging": {"enabled": False, "idle_finalize_ms": 5000},
            "photo_capture": {
                "enabled": True,
                "staging_root": str(staging_root),
                "nas_subdirectory": "voice_photos",
                "max_jpeg_mb": 8,
                "queue_max_items": 8,
            },
            "segment_seconds": 900,
            "recording_start_lead_ms": 0,
            "depth_fps": 30,
            "ffmpeg_path": "ffmpeg",
            "log_directory": str(temporary / "logs"),
            "state_path": str(temporary / "state.json"),
            "max_payload_mb": 16,
            "record_queue_max_mb": 16,
            "record_queue_total_max_mb": 32,
            "record_finalize_max_pending_segments": 2,
            "min_free_disk_mb": 0,
        }
        config_path.write_text(json.dumps(config), encoding="utf-8")
        stdout_path = temporary / "receiver-stdout.log"
        with stdout_path.open("wb") as receiver_stdout:
            process = subprocess.Popen(
                [str(args.receiver), "--config", str(config_path)],
                stdout=receiver_stdout,
                stderr=subprocess.STDOUT,
            )
        try:
            wait_admin(ports["admin"])
            sender_id = "photo-test-sender"
            camera_id = "cam01"
            request_id = "integration_photo_20260728_120000_000001"
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as status:
                status.bind(("127.0.0.1", 0))
                status.settimeout(5)
                heartbeat = {
                    "protocol_version": "3.0",
                    "message_type": "heartbeat",
                    "sender_id": sender_id,
                    "camera_id": camera_id,
                    "online": True,
                    "timestamp_us": time.time_ns() // 1000,
                }
                status.sendto(
                    (json.dumps(heartbeat, separators=(",", ":")) + "\n").encode(),
                    ("127.0.0.1", ports["status"]),
                )
                time.sleep(0.1)

                frame_time_us = time.time_ns() // 1000
                jpeg = b"\xff\xd8camera-original-mjpeg-payload\xff\xd9"
                packet = snapshot_packet(sender_id, camera_id, request_id, 42, frame_time_us, jpeg)
                with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as media:
                    media.sendall(packet)
                    deadline = time.monotonic() + 5
                    acknowledgement = None
                    while time.monotonic() < deadline:
                        payload, _peer = status.recvfrom(8192)
                        candidate = json.loads(payload)
                        if (
                            candidate.get("control") == "rgb_snapshot_result"
                            and candidate.get("request_id") == request_id
                        ):
                            acknowledgement = candidate
                            break
                    assert acknowledgement is not None
                    assert acknowledgement["ok"] is True
                    assert acknowledgement["status"] == "captured"

                job_directory = staging_root / request_id
                assert (job_directory / "photo.jpg").read_bytes() == jpeg
                marker = json.loads((job_directory / "photo_ready.json").read_text(encoding="utf-8"))
                assert marker["ready"] is True
                assert marker["format"] == "original_mjpeg"
                assert marker["frame_system_timestamp_us"] == frame_time_us
                assert marker["relative_path"].startswith(f"voice_photos/{sender_id}_{camera_id}/")
                assert acknowledgement["image_path"] == str(nas_root / marker["relative_path"])

            connection = http.client.HTTPConnection("127.0.0.1", ports["admin"], timeout=2)
            connection.request("GET", "/api/status")
            response = connection.getresponse()
            status_payload = json.loads(response.read())
            connection.close()
            assert status_payload["photo_capture"]["completed"] == 1
            assert status_payload["photo_capture"]["failures"] == 0
        finally:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        if process.returncode != 0:
            raise AssertionError(stdout_path.read_text(encoding="utf-8", errors="replace"))
    print("voice photo receiver pipeline test passed")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--receiver",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "build-snapshot-check" / "bin" / "gemini_receiver",
    )
    return parser.parse_args()


if __name__ == "__main__":
    run(parse_args())
