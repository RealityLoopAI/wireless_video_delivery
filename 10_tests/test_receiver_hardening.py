#!/usr/bin/env python3
import argparse
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time


MEDIA_MAGIC = 0x33565747
UDP_MAGIC = 0x31505547
HEADER_VERSION = 1
HEADER_SIZE = 94
STREAM_DEPTH = 2
STREAM_RGB = 1
PIXEL_ENCODED_VIDEO = 1
PIXEL_DEPTH_U16 = 2


def free_port(sock_type: int) -> int:
    with socket.socket(socket.AF_INET, sock_type) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def request(port: int, method: str, path: str, headers=None, timeout=5):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    connection.request(method, path, headers=headers or {})
    response = connection.getresponse()
    body = response.read()
    result = response.status, dict(response.getheaders()), body
    connection.close()
    return result


def wait_http(port: int, path: str = "/api/status", timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if request(port, "GET", path, timeout=1)[0] in {200, 401}:
                return
        except OSError:
            pass
        time.sleep(0.05)
    raise RuntimeError(f"HTTP service on port {port} did not start")


def status_message(port: int, payload: dict) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        data = (json.dumps(payload, separators=(",", ":")) + "\n").encode()
        sock.sendto(data, ("127.0.0.1", port))


def depth_packet(sender_id: str, camera_id: str, frame_id: int, width: int, height: int, timestamp_us: int, pair_id: int = 0) -> bytes:
    sender = sender_id.encode("ascii")
    camera = camera_id.encode("ascii")
    codec = b"none"
    payload = struct.pack("<H", 1000 + frame_id % 100) * (width * height)
    header = struct.pack(
        "<IHHBBIHHHQQQQIIHQQ16s",
        MEDIA_MAGIC,
        HEADER_VERSION,
        HEADER_SIZE,
        STREAM_DEPTH,
        pair_id,
        1 << 3,
        len(sender),
        len(camera),
        len(codec),
        frame_id,
        frame_id * 33333,
        timestamp_us,
        0,
        width,
        height,
        PIXEL_DEPTH_U16,
        len(payload),
        len(payload),
        b"\0" * 16,
    )
    return header + sender + camera + codec + payload


def rgb_packet(sender_id: str, camera_id: str, frame_id: int, width: int, height: int, timestamp_us: int, payload: bytes) -> bytes:
    sender = sender_id.encode("ascii")
    camera = camera_id.encode("ascii")
    codec = b"h264"
    header = struct.pack(
        "<IHHBBIHHHQQQQIIHQQ16s",
        MEDIA_MAGIC,
        HEADER_VERSION,
        HEADER_SIZE,
        STREAM_RGB,
        0,
        (1 << 0) | (1 << 3),
        len(sender),
        len(camera),
        len(codec),
        frame_id,
        frame_id * 33333,
        timestamp_us,
        0,
        width,
        height,
        PIXEL_ENCODED_VIDEO,
        len(payload),
        len(payload),
        b"\0" * 16,
    )
    return header + sender + camera + codec + payload


def generate_h264_fixture() -> bytes:
    result = subprocess.run(
        [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=64x48:rate=30",
            "-t", "2", "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
            "-x264-params", "repeat-headers=1:keyint=30", "-f", "h264", "pipe:1",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=20,
        check=False,
    )
    assert result.returncode == 0 and result.stdout, result.stderr.decode(errors="replace")
    return result.stdout


def compressed_depth_bomb_packet(sender_id: str, camera_id: str, timestamp_us: int) -> bytes:
    sender = sender_id.encode("ascii")
    camera = camera_id.encode("ascii")
    codec = b"pq8zlib"
    width, height = 64, 48
    payload = struct.pack("<IHHIHHI", 0x5A385150, 1, 4, width * height, 257, 0, 0)
    header = struct.pack(
        "<IHHBBIHHHQQQQIIHQQ16s",
        MEDIA_MAGIC, HEADER_VERSION, HEADER_SIZE, STREAM_DEPTH, 0, 1 << 3,
        len(sender), len(camera), len(codec), 1, 33333, timestamp_us, 0,
        width, height, PIXEL_DEPTH_U16, len(payload), width * height * 2, b"\0" * 16,
    )
    return header + sender + camera + codec + payload


def overlapping_plz4_packet(sender_id: str, camera_id: str, timestamp_us: int) -> bytes:
    sender = sender_id.encode("ascii")
    camera = camera_id.encode("ascii")
    codec = b"plz4"
    width, height = 64, 48
    raw_size = width * height * 2
    payload = struct.pack("<IHHII", 0x345A4C50, 1, 2, raw_size, 0)
    payload += struct.pack("<III", 0, raw_size // 2, 1)
    payload += struct.pack("<III", 0, raw_size // 2, 1)
    payload += b"xx"
    header = struct.pack(
        "<IHHBBIHHHQQQQIIHQQ16s",
        MEDIA_MAGIC, HEADER_VERSION, HEADER_SIZE, STREAM_DEPTH, 0, 1 << 3,
        len(sender), len(camera), len(codec), 2, 66666, timestamp_us, 0,
        width, height, PIXEL_DEPTH_U16, len(payload), raw_size, b"\0" * 16,
    )
    return header + sender + camera + codec + payload


def exercise_clock_sync(clock_port: int, status_port: int, admin_port: int) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.settimeout(2)
        t1 = int(time.time() * 1_000_000)
        message = {
            "protocol_version": "3.0",
            "message_type": "clock_sync_probe",
            "sender_id": "test-sender",
            "sequence": 1,
            "t1_sender_send_us": t1,
        }
        probe.sendto((json.dumps(message) + "\n").encode(), ("127.0.0.1", clock_port))
        response = json.loads(probe.recv(4096))
        t4 = int(time.time() * 1_000_000)
    offset = ((int(response["t2_receiver_recv_us"]) - t1) + (int(response["t3_receiver_send_us"]) - t4)) // 2
    delay = (t4 - t1) - (int(response["t3_receiver_send_us"]) - int(response["t2_receiver_recv_us"]))
    status_message(
        status_port,
        {
            "protocol_version": "3.0",
            "message_type": "clock_sync_report",
            "sender_id": "test-sender",
            "clock_sync_valid": True,
            "clock_offset_us": offset,
            "clock_delay_us": max(0, delay),
            "clock_drift_ppm": 0.5,
            "clock_last_sync_us": t4,
        },
    )
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        current = json.loads(request(admin_port, "GET", "/api/status")[2])
        models = [model for model in current.get("clock_sync", []) if model.get("sender_id") == "test-sender"]
        if models and models[0].get("clock_sync_valid"):
            return
        time.sleep(0.05)
    raise AssertionError("clock sync model was not accepted")


def send_incomplete_udp_assemblies(port: int, count: int = 300) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        for sequence in range(1, count + 1):
            header = struct.pack("<IHHIHHIIH6x", UDP_MAGIC, 1, 32, sequence, 0, 2, 64, 0, 1)
            sock.sendto(header + b"x", ("127.0.0.1", port))


def assert_recording_output(nas_root: Path) -> None:
    rgb_files = list(nas_root.rglob("rgb.mp4"))
    assert rgb_files, "rgb.mp4 was not finalized"
    for rgb_file in rgb_files:
        probe = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=codec_name:format=duration",
             "-of", "json", str(rgb_file)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )
        assert probe.returncode == 0, probe.stderr
        metadata = json.loads(probe.stdout)
        assert metadata.get("streams", [{}])[0].get("codec_name") == "h264"
        assert float(metadata.get("format", {}).get("duration", 0)) > 1.0
        seek = subprocess.run(
            ["ffmpeg", "-v", "error", "-ss", "1", "-i", str(rgb_file), "-frames:v", "1", "-f", "null", "-"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )
        assert seek.returncode == 0, "finalized rgb.mp4 is not seekable"
        assert not (rgb_file.parent / "rgb_debug.h264").exists(), "validated RGB recovery sidecar was not removed"
        assert not list(rgb_file.parent.glob("rgb_finalized*.mp4")), "realtime faststart temporary file must not be created"

    depth_files = list(nas_root.rglob("depth.mkv"))
    assert depth_files, "depth.mkv was not finalized"
    for depth_file in depth_files:
        probe = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=codec_name", "-of", "csv=p=0", str(depth_file)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )
        assert probe.returncode == 0 and "ffv1" in probe.stdout, probe.stderr
        assert not list(depth_file.parent.glob("depth_part_*.mkv")), "finalized depth parts were not removed"

    frames_files = list(nas_root.rglob("frames.csv"))
    assert frames_files, "frames.csv missing"
    header = frames_files[0].read_text(encoding="utf-8").splitlines()[0].split(",")
    for field in ("global_timestamp_us", "pair_delta_us", "pair_delta_source", "pair_id_valid"):
        assert field in header, f"missing CSV field {field}"


def run(args) -> None:
    with tempfile.TemporaryDirectory(prefix="gwv3_receiver_test_") as temporary_text:
        temporary = Path(temporary_text)
        ports = {
            "status": free_port(socket.SOCK_DGRAM),
            "media": free_port(socket.SOCK_STREAM),
            "media_udp": free_port(socket.SOCK_DGRAM),
            "clock": free_port(socket.SOCK_DGRAM),
            "admin": free_port(socket.SOCK_STREAM),
            "web": free_port(socket.SOCK_STREAM),
        }
        config = {
            "status_bind_ip": "127.0.0.1",
            "status_port": ports["status"],
            "media_bind_ip": "127.0.0.1",
            "media_port": ports["media"],
            "preview_enabled": False,
            "media_udp_enabled": True,
            "media_udp_bind_ip": "127.0.0.1",
            "media_udp_port": ports["media_udp"],
            "preview_udp_enabled": False,
            "clock_sync": {"enabled": True, "bind_ip": "127.0.0.1", "port": ports["clock"], "model_timeout_ms": 10000},
            "admin_bind_ip": "127.0.0.1",
            "admin_port": ports["admin"],
            "nas_root": str(temporary / "nas"),
            "log_directory": str(temporary / "logs"),
            "state_path": str(temporary / "state.json"),
            "ffmpeg_path": shutil.which("ffmpeg") or "ffmpeg",
            "segment_seconds": 60,
            "depth_fps": 30,
            "max_payload_mb": 8,
            "record_queue_max_mb": 16,
        }
        config_path = temporary / "receiver.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        receiver_log = (temporary / "receiver.log").open("wb")
        receiver = subprocess.Popen([args.receiver, "--config", str(config_path)], stdout=receiver_log, stderr=subprocess.STDOUT)
        web = None
        idle_client = None
        try:
            wait_http(ports["admin"])

            duplicate_log = (temporary / "duplicate_receiver.log").open("wb")
            duplicate = subprocess.Popen(
                [args.receiver, "--config", str(config_path)], stdout=duplicate_log, stderr=subprocess.STDOUT
            )
            try:
                duplicate.wait(timeout=8)
                assert duplicate.returncode != 0, "a second receiver unexpectedly accepted the same listener ports"
            finally:
                if duplicate.poll() is None:
                    duplicate.kill()
                    duplicate.wait(timeout=3)
                duplicate_log.close()

            status_message(
                ports["status"],
                {"protocol_version": "3.0", "message_type": "camera_announce", "sender_id": "../escape", "camera_id": "cam01"},
            )
            status_message(
                ports["status"],
                {
                    "protocol_version": "3.0",
                    "message_type": "camera_announce",
                    "sender_id": "test-sender",
                    "camera_id": "cam01",
                    "rgb_profile": {"width": 10**100, "height": 480},
                    "depth_profile": {"width": 64, "height": 48, "fps": 30, "depth_scale": 1},
                },
            )
            time.sleep(0.1)
            admin_status = json.loads(request(ports["admin"], "GET", "/api/status")[2])
            source_hash = str(admin_status.get("build_source_hash", ""))
            assert len(source_hash) == 16 and all(ch in "0123456789abcdef" for ch in source_hash)
            assert receiver.poll() is None, "receiver crashed on malformed status JSON"
            assert all(camera.get("sender_id") != "../escape" for camera in admin_status.get("cameras", []))
            exercise_clock_sync(ports["clock"], ports["status"], ports["admin"])

            send_incomplete_udp_assemblies(ports["media_udp"])
            time.sleep(0.1)
            udp_status = json.loads(request(ports["admin"], "GET", "/api/status")[2])
            assert udp_status["media_udp_stats"]["active_assemblies"] <= 256

            assert request(ports["admin"], "POST", "/api/record/start-all")[0] == 200
            stop_one = json.loads(
                request(
                    ports["admin"],
                    "POST",
                    "/api/record/stop?sender_id=test-sender&camera_id=cam01",
                )[2]
            )
            assert stop_one.get("ok") is False, "single-camera stop must not race an active start-all recording"
            with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as media:
                media.sendall(compressed_depth_bomb_packet("test-sender", "cam01", int(time.time() * 1_000_000)))
                media.sendall(overlapping_plz4_packet("test-sender", "cam01", int(time.time() * 1_000_000)))
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                current = json.loads(request(ports["admin"], "GET", "/api/status")[2])
                cameras = [camera for camera in current.get("cameras", []) if camera.get("sender_id") == "test-sender"]
                if cameras and int(cameras[0].get("record_write_errors", 0)) >= 2:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("malicious depth chunk layouts were not rejected")
            assert request(ports["admin"], "POST", "/api/record/stop-all", timeout=10)[0] == 200
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                current = json.loads(request(ports["admin"], "GET", "/api/status")[2])
                if all(not camera.get("segment_finalizing") for camera in current.get("cameras", [])):
                    break
                time.sleep(0.05)

            if shutil.which("ffmpeg") and shutil.which("ffprobe"):
                assert request(ports["admin"], "POST", "/api/record/start-all")[0] == 200
                start_timestamp = int(time.time() * 1_000_000)
                h264_fixture = generate_h264_fixture()
                with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as media:
                    media.sendall(rgb_packet("test-sender", "cam01", 1, 64, 48, start_timestamp, h264_fixture))
                    for frame_id in range(70):
                        media.sendall(depth_packet("test-sender", "cam01", frame_id, 64, 48, start_timestamp + frame_id * 33333))
                assert request(ports["admin"], "POST", "/api/record/stop-all", timeout=20)[0] == 200
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    current = json.loads(request(ports["admin"], "GET", "/api/status")[2])
                    cameras = current.get("cameras", [])
                    if all(not camera.get("segment_active") and not camera.get("segment_finalizing") for camera in cameras):
                        break
                    time.sleep(0.1)
                else:
                    raise AssertionError("recording did not finalize")
                assert_recording_output(temporary / "nas")
            else:
                print("recording finalization check skipped: ffmpeg/ffprobe unavailable")

            try:
                __import__("uvicorn")
            except ImportError:
                pass
            else:
                environment = os.environ.copy()
                environment.update(
                    {
                        "GWV3_WEB_AUTH_TOKEN": "integration-secret",
                        "GWV3_RECEIVER_ADMIN": f"http://127.0.0.1:{ports['admin']}",
                    }
                )
                web = subprocess.Popen(
                    [os.sys.executable, "-m", "uvicorn", "server:app", "--app-dir", str(Path(args.source_root) / "09_web_monitor"),
                     "--host", "127.0.0.1", "--port", str(ports["web"]), "--no-access-log"],
                    env=environment,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                wait_http(ports["web"], "/")
                assert request(ports["web"], "GET", "/api/status")[0] == 401
                auth_status, auth_headers, _ = request(
                    ports["web"], "POST", "/api/auth", {"X-GWV3-Token": "integration-secret"}
                )
                assert auth_status == 200
                cookie = auth_headers.get("Set-Cookie", "").split(";", 1)[0]
                assert cookie and request(ports["web"], "GET", "/api/status", {"Cookie": cookie})[0] == 200

            for index in range(40):
                status_message(
                    ports["status"],
                    {
                        "protocol_version": "3.0",
                        "message_type": "camera_announce",
                        "sender_id": "capacity-test",
                        "camera_id": f"cam{index:02d}",
                    },
                )
            time.sleep(0.2)
            assert receiver.poll() is None, "receiver crashed when the tracked-camera limit was exceeded"
            final_status = json.loads(request(ports["admin"], "GET", "/api/status")[2])
            assert int(final_status.get("record_queue_total_bytes", -1)) == 0

            idle_client = socket.create_connection(("127.0.0.1", ports["media"]), timeout=3)
            receiver.terminate()
            receiver.wait(timeout=8)
            assert receiver.returncode == 0, f"receiver shutdown failed: {receiver.returncode}"
        finally:
            if idle_client:
                idle_client.close()
            if web and web.poll() is None:
                web.terminate()
                web.wait(timeout=5)
            if receiver.poll() is None:
                receiver.kill()
                receiver.wait(timeout=5)
            receiver_log.close()
            if receiver.returncode not in {0, -15}:
                print((temporary / "receiver.log").read_text(encoding="utf-8", errors="replace"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--receiver", required=True)
    parser.add_argument("--source-root", required=True)
    run(parser.parse_args())
    print("receiver hardening integration test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
