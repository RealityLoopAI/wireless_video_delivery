#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import signal
import socket
import subprocess
import tempfile
import time
import urllib.request
from pathlib import Path


def free_port(sock_type: int) -> int:
    with socket.socket(socket.AF_INET, sock_type) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_http(url: str, timeout: float = 5.0) -> dict[str, object]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=0.5) as response:
                return json.loads(response.read().decode("utf-8"))
        except Exception:
            time.sleep(0.05)
    raise AssertionError(f"receiver HTTP did not become ready: {url}")


def post_json(url: str) -> dict[str, object]:
    request = urllib.request.Request(url, data=b"{}", method="POST")
    with urllib.request.urlopen(request, timeout=2) as response:
        return json.loads(response.read().decode("utf-8"))


def run(receiver: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="gwv3_discovery_test_") as temporary_text:
        temporary = Path(temporary_text)
        ports = {
            "status": free_port(socket.SOCK_DGRAM),
            "media": free_port(socket.SOCK_STREAM),
            "clock": free_port(socket.SOCK_DGRAM),
            "discovery": free_port(socket.SOCK_DGRAM),
            "admin": free_port(socket.SOCK_STREAM),
        }
        config = {
            "status_bind_ip": "127.0.0.1",
            "status_port": ports["status"],
            "media_bind_ip": "127.0.0.1",
            "media_port": ports["media"],
            "admin_bind_ip": "127.0.0.1",
            "admin_port": ports["admin"],
            "receiver_discovery": {
                "enabled": True,
                "bind_ip": "127.0.0.1",
                "port": ports["discovery"],
                "receiver_id": "test-receiver",
            },
            "clock_sync": {
                "enabled": True,
                "bind_ip": "127.0.0.1",
                "port": ports["clock"],
                "model_timeout_ms": 10000,
            },
            "nas_root": str(temporary / "nas"),
            "nas_auto_mount": {
                "enabled": True,
                "status_path": str(temporary / "nas-status.json"),
                "status_max_age_ms": 10000,
                "require_for_new_recording": True,
            },
            "log_directory": str(temporary / "logs"),
            "state_path": str(temporary / "state.json"),
            "segment_seconds": 30,
            "min_free_disk_mb": 0,
        }
        config_path = temporary / "receiver.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        process = subprocess.Popen(
            [str(receiver), "--config", str(config_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            status_url = f"http://127.0.0.1:{ports['admin']}/api/status"
            initial_status = wait_http(status_url)
            assert initial_status["recording_start_ready"] is False
            assert initial_status["recording_storage"]["available"] is True
            assert initial_status["recording_storage"]["free_bytes"] > 0
            assert initial_status["recording_storage"]["hard_limit"] is False
            blocked = post_json(f"http://127.0.0.1:{ports['admin']}/api/record/start-all")
            assert blocked["ok"] is False
            assert "NAS is not mounted" in blocked["error"]
            (temporary / "nas-status.json").write_text(
                json.dumps(
                    {
                        "ready": True,
                        "updated_us": time.time_ns() // 1_000,
                        "mount_point": str(temporary / "nas"),
                    }
                ),
                encoding="utf-8",
            )
            ready = post_json(f"http://127.0.0.1:{ports['admin']}/api/record/start-all")
            assert ready["ok"] is True
            stopped = post_json(f"http://127.0.0.1:{ports['admin']}/api/record/stop-all")
            assert stopped["ok"] is True
            sequence = 9123
            request = json.dumps(
                {
                    "protocol_version": "3.0",
                    "message_type": "receiver_discovery_request",
                    "sender_id": "discovery-test-sender",
                    "sequence": sequence,
                    "preferred_receiver_id": "test-receiver",
                },
                separators=(",", ":"),
            ).encode("utf-8")
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.settimeout(2)
                sock.sendto(request, ("127.0.0.1", ports["discovery"]))
                payload, peer = sock.recvfrom(4096)
            response = json.loads(payload.decode("utf-8"))
            assert peer[0] == "127.0.0.1"
            assert response["message_type"] == "receiver_discovery_response"
            assert response["receiver_id"] == "test-receiver"
            assert response["sequence"] == sequence
            assert response["media_port"] == ports["media"]
            assert response["status_port"] == ports["status"]
            assert response["clock_sync_port"] == ports["clock"]

            deadline = time.monotonic() + 2
            while True:
                status = wait_http(status_url)
                discovery = status["receiver_discovery"]
                if discovery["requests_received"] >= 1:
                    break
                if time.monotonic() >= deadline:
                    raise AssertionError("receiver discovery counters did not update")
                time.sleep(0.05)
            assert discovery["healthy"] is True
            assert discovery["responses_sent"] >= 1
        finally:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            if process.returncode != 0:
                output = process.stdout.read() if process.stdout else ""
                raise AssertionError(f"receiver exited with {process.returncode}\n{output}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--receiver", type=Path, required=True)
    args = parser.parse_args()
    run(args.receiver)
    print("receiver discovery integration test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
