#!/usr/bin/env python3
import argparse
import json
import os
import re
from pathlib import Path
from urllib.parse import urlsplit


HOST_PATTERN = re.compile(r"^[A-Za-z0-9_.:-]{1,253}$")


def _state_paths():
    configured = os.environ.get("GWV3_RECEIVER_TARGET_STATE", "").strip()
    if configured:
        yield Path(configured)
    xdg = os.environ.get("XDG_STATE_HOME", "").strip()
    if xdg:
        yield Path(xdg) / "gwv3" / "receiver_target.json"
    yield Path.home() / ".local" / "state" / "gwv3" / "receiver_target.json"


def discovered_receiver_host() -> str:
    for path in _state_paths():
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError, TypeError):
            continue
        host = str(payload.get("receiver_host") or payload.get("host") or "").strip()
        if HOST_PATTERN.fullmatch(host) and "/" not in host:
            return host
    return ""


def resolve_receiver_base_url(configured: str) -> str:
    configured = str(configured or "").strip().rstrip("/")
    if configured.lower() in {"", "auto"}:
        fallback = "http://gwv3-receiver.local:8080"
    else:
        fallback = configured
    parsed = urlsplit(fallback)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname:
        raise ValueError(f"invalid receiver_base_url: {configured}")
    host = discovered_receiver_host()
    if not host:
        return fallback
    if ":" in host and not host.startswith("["):
        host = f"[{host}]"
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    return f"{parsed.scheme}://{host}:{port}"


def main():
    parser = argparse.ArgumentParser(description="Resolve the current GWV3 receiver endpoint")
    parser.add_argument("configured", nargs="?", default="auto")
    parser.add_argument("--host-only", action="store_true")
    args = parser.parse_args()
    url = resolve_receiver_base_url(args.configured)
    print(urlsplit(url).hostname if args.host_only else url)


if __name__ == "__main__":
    main()
