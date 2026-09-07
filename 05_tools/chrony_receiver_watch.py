#!/usr/bin/env python3
"""Keep a sender's Chrony source aligned with its discovered receiver."""

from __future__ import annotations

import argparse
import ipaddress
import json
import os
import signal
import socket
import subprocess
import time
from pathlib import Path


STOP = False


def request_stop(_signum: int, _frame: object) -> None:
    global STOP
    STOP = True


def valid_host(value: str) -> bool:
    if not value or len(value) > 253 or any(char.isspace() for char in value):
        return False
    try:
        ipaddress.ip_address(value)
        return True
    except ValueError:
        labels = value.rstrip(".").split(".")
        return all(
            label
            and len(label) <= 63
            and label[0].isalnum()
            and label[-1].isalnum()
            and all(char.isalnum() or char == "-" for char in label)
            for label in labels
        )


def discovered_host(path: Path) -> str:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return ""
    if not isinstance(value, dict):
        return ""
    return str(value.get("receiver_host") or value.get("host") or "")


def resolvable(host: str) -> bool:
    try:
        socket.getaddrinfo(host, 123, socket.AF_UNSPEC, socket.SOCK_DGRAM)
        return True
    except OSError:
        return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--fallback", required=True)
    parser.add_argument("--setup", type=Path, required=True)
    parser.add_argument("--applied-state", type=Path, default=Path("/var/lib/gwv3/chrony-target"))
    parser.add_argument("--interval", type=float, default=5.0)
    args = parser.parse_args()
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    args.applied_state.parent.mkdir(parents=True, exist_ok=True)
    try:
        applied = args.applied_state.read_text(encoding="utf-8").strip()
    except OSError:
        applied = ""

    while not STOP:
        candidate = discovered_host(args.state) or args.fallback
        if valid_host(candidate) and candidate != applied and resolvable(candidate):
            result = subprocess.run(
                [str(args.setup), candidate],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=90,
            )
            if result.returncode == 0:
                temporary = args.applied_state.with_suffix(f".tmp.{os.getpid()}")
                temporary.write_text(candidate + "\n", encoding="utf-8")
                os.replace(temporary, args.applied_state)
                applied = candidate
                print(f"chrony receiver target updated host={candidate}", flush=True)
            else:
                print(
                    f"chrony receiver target update failed host={candidate} output={result.stdout[-500:]}",
                    flush=True,
                )
        deadline = time.monotonic() + max(1.0, args.interval)
        while not STOP and time.monotonic() < deadline:
            time.sleep(min(0.2, deadline - time.monotonic()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
