#!/usr/bin/env python3
"""Discover, mount, and health-check the single supplied GWV3 NAS."""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import re
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path


PROTOCOL_VERSION = "3.0"
STOP_REQUESTED = False
ID_PATTERN = re.compile(r"^[A-Za-z0-9_-]{1,64}$")


def request_stop(_signum: int, _frame: object) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True


def now_us() -> int:
    return time.time_ns() // 1_000


def atomic_json_write(path: Path, value: dict[str, object], mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            json.dump(value, output, ensure_ascii=True, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def load_json(path: Path) -> dict[str, object]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"JSON root must be an object: {path}")
    return value


def interface_broadcasts() -> set[str]:
    addresses = {"255.255.255.255"}
    try:
        result = subprocess.run(
            ["ip", "-j", "-4", "address", "show", "up"],
            check=True,
            capture_output=True,
            text=True,
            timeout=2,
        )
        for interface in json.loads(result.stdout):
            for address in interface.get("addr_info", []):
                broadcast = address.get("broadcast")
                if address.get("scope") == "global" and isinstance(broadcast, str):
                    addresses.add(broadcast)
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError, TypeError):
        pass
    return addresses


def discover_nas(port: int, timeout_ms: int, preferred_id: str) -> list[dict[str, str]]:
    sequence = now_us()
    request = json.dumps(
        {
            "protocol_version": PROTOCOL_VERSION,
            "message_type": "nas_discovery_request",
            "sequence": sequence,
            "preferred_nas_id": preferred_id,
        },
        separators=(",", ":"),
    ).encode("utf-8")
    candidates: dict[str, dict[str, str]] = {}
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind(("0.0.0.0", 0))
        for address in interface_broadcasts():
            try:
                sock.sendto(request, (address, port))
            except OSError:
                continue
        deadline = time.monotonic() + timeout_ms / 1000.0
        while time.monotonic() < deadline:
            sock.settimeout(max(0.01, deadline - time.monotonic()))
            try:
                payload, peer = sock.recvfrom(4096)
                response = json.loads(payload.decode("utf-8"))
            except socket.timeout:
                break
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            except OSError:
                break
            if not isinstance(response, dict):
                continue
            nas_id = str(response.get("nas_id") or "")
            share = str(response.get("smb_share") or "")
            if (
                response.get("protocol_version") != PROTOCOL_VERSION
                or response.get("message_type") != "nas_discovery_response"
                or response.get("sequence") != sequence
                or not ID_PATTERN.fullmatch(nas_id)
                or not re.fullmatch(r"[A-Za-z0-9_$.-]{1,80}", share)
            ):
                continue
            candidates[nas_id] = {"nas_id": nas_id, "host": peer[0], "share": share}
    return [candidates[key] for key in sorted(candidates)]


class NasMountManager:
    def __init__(self, receiver_config_path: Path):
        receiver = load_json(receiver_config_path)
        config = receiver.get("nas_auto_mount") or {}
        if not isinstance(config, dict):
            raise ValueError("nas_auto_mount must be an object")
        self.enabled = bool(config.get("enabled", False))
        self.mount_point = Path(str(receiver.get("nas_root") or "")).expanduser().absolute()
        self.port = int(config.get("discovery_port", 50008))
        self.discovery_timeout_ms = max(100, int(config.get("discovery_timeout_ms", 500)))
        self.probe_interval = max(0.5, int(config.get("probe_interval_ms", 2000)) / 1000.0)
        self.probe_timeout_seconds = max(1.0, int(config.get("probe_timeout_ms", 3000)) / 1000.0)
        self.credentials_file = Path(
            str(config.get("credentials_file") or "/etc/gwv3/nas-credentials")
        ).absolute()
        self.status_path = Path(
            str(config.get("status_path") or "/run/gwv3/nas-mount-status.json")
        ).absolute()
        self.state_path = Path(
            str(config.get("state_path") or "/var/lib/gwv3/nas-target.json")
        ).absolute()
        self.uid = int(config.get("uid", 1000))
        self.gid = int(config.get("gid", 1000))
        self.mount_options = str(
            config.get("mount_options")
            or "vers=3.1.1,iocharset=utf8,noserverino,soft,actimeo=1,_netdev"
        )
        self.remount_after_failures = max(2, int(config.get("remount_after_failures", 5)))
        self.preferred: dict[str, str] = {}
        self.consecutive_probe_failures = 0
        if not self.mount_point.is_absolute() or str(self.mount_point) == "/":
            raise ValueError("nas_root must be a non-root absolute path")
        if not 1 <= self.port <= 65535:
            raise ValueError("discovery_port must be in range [1, 65535]")
        try:
            persisted = load_json(self.state_path)
            if ID_PATTERN.fullmatch(str(persisted.get("nas_id") or "")):
                self.preferred = {
                    "nas_id": str(persisted["nas_id"]),
                    "host": str(persisted.get("host") or ""),
                    "share": str(persisted.get("share") or ""),
                }
        except (OSError, ValueError, json.JSONDecodeError):
            pass

    def write_status(self, ready: bool, state: str, error: str = "") -> None:
        atomic_json_write(
            self.status_path,
            {
                "schema": "gwv3_nas_mount_status_v1",
                "ready": ready,
                "state": state,
                "error": error,
                "mount_point": str(self.mount_point),
                "nas_id": self.preferred.get("nas_id", ""),
                "host": self.preferred.get("host", ""),
                "share": self.preferred.get("share", ""),
                "updated_us": now_us(),
            },
        )

    def is_cifs_mount(self) -> bool:
        try:
            result = subprocess.run(
                ["findmnt", "-rn", "-T", str(self.mount_point), "-o", "FSTYPE"],
                check=False,
                capture_output=True,
                text=True,
                timeout=2,
            )
            return result.returncode == 0 and result.stdout.strip() in {"cifs", "smb3"}
        except (OSError, subprocess.SubprocessError):
            return False

    def writable_probe(self) -> bool:
        if not self.is_cifs_mount():
            return False
        # Keep one probe in place. Deleting a fresh file on every poll causes
        # NAS recycle-bin implementations to accumulate thousands of entries.
        probe = self.mount_point / ".gwv3_mount_health"
        try:
            touch = subprocess.run(
                ["timeout", str(self.probe_timeout_seconds), "touch", str(probe)],
                check=False,
                capture_output=True,
                timeout=self.probe_timeout_seconds + 1,
            )
            return touch.returncode == 0
        except (OSError, subprocess.SubprocessError):
            return False

    def select_target(self) -> bool:
        preferred_id = self.preferred.get("nas_id", "")
        candidates = discover_nas(self.port, self.discovery_timeout_ms, preferred_id)
        if not candidates:
            return bool(self.preferred.get("host") and self.preferred.get("share"))
        selected = next((item for item in candidates if item["nas_id"] == preferred_id), candidates[0])
        self.preferred = selected
        atomic_json_write(self.state_path, {**selected, "updated_us": now_us()}, mode=0o600)
        return True

    def mount(self) -> tuple[bool, str]:
        if not self.credentials_file.is_file():
            return False, f"credentials file missing: {self.credentials_file}"
        if not self.select_target():
            return False, "supplied NAS not discovered"
        self.mount_point.mkdir(parents=True, exist_ok=True)
        source = f"//{self.preferred['host']}/{self.preferred['share']}"
        options = (
            f"credentials={self.credentials_file},uid={self.uid},gid={self.gid},"
            f"file_mode=0660,dir_mode=0770,{self.mount_options}"
        )
        try:
            result = subprocess.run(
                ["mount", "-t", "cifs", source, str(self.mount_point), "-o", options],
                check=False,
                capture_output=True,
                text=True,
                timeout=15,
            )
        except (OSError, subprocess.SubprocessError) as error:
            return False, f"mount command failed: {error}"
        if result.returncode != 0:
            message = (result.stderr or result.stdout).strip()
            return False, f"CIFS mount failed: {message[:300]}"
        return True, ""

    def detach_stale_mount(self) -> tuple[bool, str]:
        try:
            result = subprocess.run(
                ["umount", "-l", str(self.mount_point)],
                check=False,
                capture_output=True,
                text=True,
                timeout=10,
            )
        except (OSError, subprocess.SubprocessError) as error:
            return False, f"stale CIFS detach failed: {error}"
        if result.returncode != 0:
            message = (result.stderr or result.stdout).strip()
            return False, f"stale CIFS detach failed: {message[:300]}"
        return True, ""

    def run(self) -> int:
        if not self.enabled:
            self.write_status(False, "disabled")
            return 0
        self.write_status(False, "starting")
        while not STOP_REQUESTED:
            if self.writable_probe():
                self.consecutive_probe_failures = 0
                self.write_status(True, "ready")
            else:
                self.consecutive_probe_failures += 1
                self.write_status(False, "unavailable", "NAS mount is absent or not writable")
                mounted_now = self.is_cifs_mount()
                if mounted_now and self.consecutive_probe_failures >= self.remount_after_failures:
                    detached, error = self.detach_stale_mount()
                    if detached:
                        mounted_now = False
                        self.preferred = {}
                    else:
                        self.write_status(False, "waiting", error)
                if not mounted_now:
                    mounted, error = self.mount()
                    if mounted and self.writable_probe():
                        self.consecutive_probe_failures = 0
                        self.write_status(True, "ready")
                    elif error:
                        self.write_status(False, "waiting", error)
            deadline = time.monotonic() + self.probe_interval
            while not STOP_REQUESTED and time.monotonic() < deadline:
                time.sleep(min(0.2, deadline - time.monotonic()))
        self.write_status(False, "stopped")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True, help="Receiver JSON config")
    args = parser.parse_args()
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    lock_path = Path("/run/gwv3/nas-mount-manager.lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print("GWV3 NAS mount manager is already running", flush=True)
            return 0
        return NasMountManager(args.config).run()


if __name__ == "__main__":
    raise SystemExit(main())
