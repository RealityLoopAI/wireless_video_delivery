#!/usr/bin/env python3
"""Advertise the supplied GWV3 NAS to receivers on the local LAN."""

from __future__ import annotations

import argparse
import json
import re
import signal
import socket
import time
from pathlib import Path


PROTOCOL_VERSION = "3.0"
STOP_REQUESTED = False
ID_PATTERN = re.compile(r"^[A-Za-z0-9_-]{1,64}$")


def request_stop(_signum: int, _frame: object) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True


def stable_nas_id() -> str:
    hostname = re.sub(r"[^A-Za-z0-9_-]+", "-", socket.gethostname()).strip("-") or "gwv3-nas"
    try:
        machine_id = re.sub(r"[^A-Fa-f0-9]+", "", Path("/etc/machine-id").read_text()).lower()[-8:]
    except OSError:
        machine_id = "default"
    prefix = hostname[: max(1, 63 - len(machine_id))]
    return f"{prefix}-{machine_id}"


def load_config(path: Path) -> dict[str, object]:
    if not path.exists():
        return {}
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError("NAS beacon config must be a JSON object")
    return value


def run(config: dict[str, object]) -> int:
    enabled = bool(config.get("enabled", True))
    if not enabled:
        print("GWV3 NAS discovery beacon disabled", flush=True)
        return 0
    nas_id = str(config.get("nas_id") or "auto")
    if nas_id == "auto":
        nas_id = stable_nas_id()
    share = str(config.get("share") or "recordings")
    port = int(config.get("port", 50008))
    if not ID_PATTERN.fullmatch(nas_id):
        raise ValueError("nas_id must be 1-64 ASCII letters/digits/_/-")
    if not re.fullmatch(r"[A-Za-z0-9_$.-]{1,80}", share) or share in {".", ".."}:
        raise ValueError("share contains unsupported characters")
    if not 1 <= port <= 65535:
        raise ValueError("port must be in range [1, 65535]")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((str(config.get("bind_ip") or "0.0.0.0"), port))
    sock.settimeout(0.5)
    print(f"GWV3 NAS discovery beacon ready nas_id={nas_id} share={share} port={port}", flush=True)
    while not STOP_REQUESTED:
        try:
            payload, peer = sock.recvfrom(4096)
        except socket.timeout:
            continue
        except InterruptedError:
            continue
        try:
            request = json.loads(payload.decode("utf-8"))
            if (
                not isinstance(request, dict)
                or request.get("protocol_version") != PROTOCOL_VERSION
                or request.get("message_type") != "nas_discovery_request"
                or not isinstance(request.get("sequence"), int)
            ):
                continue
            response = {
                "protocol_version": PROTOCOL_VERSION,
                "message_type": "nas_discovery_response",
                "nas_id": nas_id,
                "sequence": request["sequence"],
                "smb_share": share,
            }
            sock.sendto(json.dumps(response, separators=(",", ":")).encode("utf-8"), peer)
        except (UnicodeDecodeError, json.JSONDecodeError, OSError, TypeError, ValueError):
            continue
    sock.close()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path("/etc/gwv3/nas-beacon.json"))
    args = parser.parse_args()
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    return run(load_config(args.config))


if __name__ == "__main__":
    raise SystemExit(main())
