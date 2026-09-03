#!/usr/bin/env python3
from __future__ import annotations

import argparse
import importlib.util
import json
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path
from unittest import mock


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load module: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def run(beacon_path: Path, manager_path: Path, uploader_path: Path) -> None:
    manager_module = load_module("gwv3_nas_mount_manager_test", manager_path)
    uploader_module = load_module("gwv3_recording_uploader_nas_gate_test", uploader_path)
    with tempfile.TemporaryDirectory(prefix="gwv3_nas_discovery_test_") as temporary_text:
        temporary = Path(temporary_text)
        port = free_udp_port()
        beacon_config = temporary / "beacon.json"
        beacon_config.write_text(
            json.dumps(
                {
                    "enabled": True,
                    "nas_id": "test-nas",
                    "bind_ip": "0.0.0.0",
                    "port": port,
                    "share": "recordings",
                }
            ),
            encoding="utf-8",
        )
        beacon = subprocess.Popen(
            ["python3", str(beacon_path), "--config", str(beacon_config)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            deadline = time.monotonic() + 3
            candidates = []
            while time.monotonic() < deadline and not candidates:
                candidates = manager_module.discover_nas(port, 250, "test-nas")
                if not candidates:
                    time.sleep(0.05)
            assert len(candidates) == 1
            assert candidates[0]["nas_id"] == "test-nas"
            assert candidates[0]["share"] == "recordings"

            receiver_config = temporary / "receiver.json"
            status_path = temporary / "run" / "status.json"
            state_path = temporary / "state" / "target.json"
            receiver_config.write_text(
                json.dumps(
                    {
                        "nas_root": str(temporary / "nas"),
                        "nas_auto_mount": {
                            "enabled": True,
                            "discovery_port": port,
                            "status_path": str(status_path),
                            "state_path": str(state_path),
                            "credentials_file": str(temporary / "credentials"),
                        },
                    }
                ),
                encoding="utf-8",
            )
            mount_manager = manager_module.NasMountManager(receiver_config)
            assert mount_manager.select_target() is True
            assert mount_manager.preferred["nas_id"] == "test-nas"
            with mock.patch.object(mount_manager, "is_cifs_mount", return_value=True), mock.patch.object(
                manager_module.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)
            ) as probe_run:
                assert mount_manager.writable_probe() is True
                probe_commands = [call.args[0] for call in probe_run.call_args_list]
                assert probe_commands == [
                    [
                        "timeout",
                        str(mount_manager.probe_timeout_seconds),
                        "touch",
                        str(mount_manager.mount_point / ".gwv3_mount_health"),
                    ]
                ]
            mount_manager.write_status(False, "waiting", "test")
            status = json.loads(status_path.read_text(encoding="utf-8"))
            assert status["ready"] is False
            assert status["mount_point"] == str((temporary / "nas").absolute())
            assert json.loads(state_path.read_text(encoding="utf-8"))["nas_id"] == "test-nas"

            uploader_config = temporary / "uploader.json"
            uploader_config.write_text(
                json.dumps(
                    {
                        "nas_root": str(temporary / "nas"),
                        "nas_auto_mount": {
                            "enabled": True,
                            "status_path": str(status_path),
                            "status_max_age_ms": 10000,
                        },
                        "recording_staging": {
                            "enabled": True,
                            "root": str(temporary / "staging"),
                        },
                    }
                ),
                encoding="utf-8",
            )
            uploader = uploader_module.Uploader(uploader_config)
            assert uploader.nas_mount_ready() is False
            mount_manager.write_status(True, "ready")
            assert uploader.nas_mount_ready() is True
            stale = json.loads(status_path.read_text(encoding="utf-8"))
            stale["updated_us"] = 1
            status_path.write_text(json.dumps(stale), encoding="utf-8")
            assert uploader.nas_mount_ready() is False
        finally:
            beacon.send_signal(signal.SIGTERM)
            try:
                beacon.wait(timeout=3)
            except subprocess.TimeoutExpired:
                beacon.kill()
                beacon.wait(timeout=3)
            if beacon.returncode != 0:
                output = beacon.stdout.read() if beacon.stdout else ""
                raise AssertionError(f"NAS beacon exited with {beacon.returncode}\n{output}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--beacon", type=Path, required=True)
    parser.add_argument("--manager", type=Path, required=True)
    parser.add_argument("--uploader", type=Path, required=True)
    args = parser.parse_args()
    run(args.beacon, args.manager, args.uploader)
    print("NAS discovery integration test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
