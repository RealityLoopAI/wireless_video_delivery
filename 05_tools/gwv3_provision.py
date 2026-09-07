#!/usr/bin/env python3
"""Detect GWV3 hardware and render device-local deployment configuration."""

from __future__ import annotations

import argparse
import copy
import hashlib
import ipaddress
import json
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


ORBBEC_VENDOR_ID = "2bc5"
GEMINI_305_PRODUCT_IDS = {"0840"}
SV1301S_DEPTH_PRODUCT_IDS = {"0614"}
SV1301S_COLOR_PRODUCT_IDS = {"0511"}
ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]{2,63}$")
MAC_PATTERN = re.compile(r"^[0-9a-f]{2}(?::[0-9a-f]{2}){5}$")


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"JSON root must be an object: {path}")
    return value


def write_json(path: Path, value: dict[str, Any], mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temporary.open("w", encoding="utf-8") as output:
        json.dump(value, output, ensure_ascii=True, indent=2, sort_keys=False)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    temporary.chmod(mode)
    temporary.replace(path)


def read_text(path: Path, default: str = "") -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace").strip("\x00\n ")
    except OSError:
        return default


def command_output(command: list[str], timeout: float = 3.0) -> str:
    try:
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    return result.stdout


def parse_os_release(path: Path = Path("/etc/os-release")) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in read_text(path).splitlines():
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key] = value.strip().strip('"')
    return values


def usb_devices_from_sysfs(sys_root: Path = Path("/sys")) -> list[dict[str, str]]:
    devices: list[dict[str, str]] = []
    for entry in sorted((sys_root / "bus/usb/devices").glob("*")):
        vendor = read_text(entry / "idVendor").lower()
        product = read_text(entry / "idProduct").lower()
        if not vendor or not product:
            continue
        devices.append(
            {
                "vendor_id": vendor,
                "product_id": product,
                "product": read_text(entry / "product"),
                "manufacturer": read_text(entry / "manufacturer"),
                "serial": read_text(entry / "serial"),
                "speed_mbps": read_text(entry / "speed"),
                "sysfs_name": entry.name,
            }
        )
    return devices


def classify_camera(devices: list[dict[str, str]]) -> dict[str, Any]:
    orbbec = [item for item in devices if item["vendor_id"] == ORBBEC_VENDOR_ID]
    product_ids = {item["product_id"] for item in orbbec}
    if product_ids & GEMINI_305_PRODUCT_IDS:
        family = "gemini305"
        sdk = "v2"
        model = "Orbbec Gemini 305"
    elif product_ids & SV1301S_DEPTH_PRODUCT_IDS and product_ids & SV1301S_COLOR_PRODUCT_IDS:
        family = "sv1301s"
        sdk = "v1"
        model = "SV1301S_U3"
    elif orbbec:
        family = "unknown"
        sdk = ""
        model = "unknown Orbbec"
    else:
        family = "none"
        sdk = ""
        model = ""
    superspeed = any(
        float(item.get("speed_mbps") or 0) >= 5000
        for item in orbbec
        if str(item.get("speed_mbps") or "").replace(".", "", 1).isdigit()
    )
    return {
        "family": family,
        "model": model,
        "sdk": sdk,
        "usb3": superspeed,
        "usb_devices": orbbec,
    }


def classify_board(model: str, hostname: str) -> str:
    value = f"{model} {hostname}".lower()
    if "lubancat" in value or "rk3576" in value:
        return "lubancat"
    if "orange pi" in value or "orangepi5pro" in value or "opi 5 pro" in value:
        return "orangepi5pro"
    if "rk3588" in value:
        return "rk3588"
    return "unknown"


def audio_inventory() -> dict[str, Any]:
    capture = command_output(["arecord", "-l"])
    playback = command_output(["aplay", "-l"])
    supported_names = ("usb pnp sound device", "usb2.0 device", "sm15 m1 usb audio")
    capture_supported = any(name in capture.lower() for name in supported_names)
    playback_supported = any(name in playback.lower() for name in supported_names)
    return {
        "capture_available": capture_supported,
        "playback_available": playback_supported,
        "supported_pair": capture_supported and playback_supported,
        "capture_summary": [line.strip() for line in capture.splitlines() if line.startswith("card ")],
        "playback_summary": [line.strip() for line in playback.splitlines() if line.startswith("card ")],
    }


def detect_hardware(sys_root: Path = Path("/sys"), os_release: Path = Path("/etc/os-release")) -> dict[str, Any]:
    os_info = parse_os_release(os_release)
    hostname = socket.gethostname()
    board_model = read_text(sys_root / "firmware/devicetree/base/model") or read_text(
        sys_root / "firmware/devicetree/base/compatible"
    )
    camera = classify_camera(usb_devices_from_sysfs(sys_root))
    board = classify_board(board_model, hostname)
    audio = audio_inventory()
    button_paths = [
        sys_root / "bus/iio/devices/iio:device0/in_voltage0_raw",
        sys_root / "bus/iio/devices/iio:device0/in_voltage1_raw",
    ]
    return {
        "hostname": hostname,
        "architecture": os.uname().machine,
        "os_id": os_info.get("ID", ""),
        "os_version_id": os_info.get("VERSION_ID", ""),
        "os_pretty_name": os_info.get("PRETTY_NAME", ""),
        "kernel": os.uname().release,
        "board": board,
        "board_model": board_model,
        "camera": camera,
        "audio": audio,
        "recording_buttons_available": board == "lubancat" and all(path.exists() for path in button_paths),
        "power_button_available": board == "lubancat"
        and any(
            Path("/dev/input/by-path").glob(
                "platform-2ac40000.i2c-platform-rk805-pwrkey.*.auto-event"
            )
        ),
        "recording_led_available": board == "lubancat" and Path("/dev/gpiochip4").exists(),
    }


def valid_unicast_mac(value: str) -> bool:
    value = value.lower()
    if not MAC_PATTERN.fullmatch(value) or value == "00:00:00:00:00:00":
        return False
    first = int(value.split(":", 1)[0], 16)
    return not (first & 0x01) and not (first & 0x02)


def stable_hardware_suffix(sys_root: Path = Path("/sys")) -> tuple[str, str]:
    serial_candidates = [
        sys_root / "firmware/devicetree/base/serial-number",
        Path("/proc/device-tree/serial-number"),
    ]
    for path in serial_candidates:
        serial = re.sub(r"[^0-9a-z]", "", read_text(path).lower())
        if len(serial) >= 8 and set(serial) != {"0"}:
            return serial[-8:], f"serial:{path}"

    network_root = sys_root / "class/net"
    candidates: list[tuple[str, str]] = []
    for interface in sorted(network_root.glob("*")):
        if interface.name == "lo":
            continue
        permanent = command_output(["ethtool", "-P", interface.name], timeout=1.0)
        match = re.search(r"([0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5})", permanent)
        mac = match.group(1).lower() if match else read_text(interface / "address").lower()
        if valid_unicast_mac(mac):
            candidates.append((interface.name, mac))
    if candidates:
        name, mac = candidates[0]
        return mac.replace(":", "")[-8:], f"mac:{name}"

    machine_id = re.sub(r"[^0-9a-f]", "", read_text(Path("/etc/machine-id")).lower())
    if len(machine_id) >= 8:
        return machine_id[-8:], "machine-id"
    raise RuntimeError("no stable hardware identity source is available")


def sender_id_for(board: str, explicit: str = "") -> tuple[str, str]:
    if explicit:
        if not ID_PATTERN.fullmatch(explicit):
            raise ValueError("sender ID must match [a-z0-9][a-z0-9-]{2,63}")
        return explicit, "operator"
    prefix = {"orangepi5pro": "orangepi5pro", "lubancat": "lubancat", "rk3588": "rk3588"}.get(
        board, "sender"
    )
    suffix, source = stable_hardware_suffix()
    return f"{prefix}-{suffix}", source


def select_sender_profile(root: Path, manifest: Path, board: str, camera: str) -> dict[str, Any]:
    profiles = load_json(manifest).get("profiles") or []
    matches = [item for item in profiles if item.get("board") == board and item.get("camera") == camera]
    if len(matches) != 1:
        raise RuntimeError(f"no unique approved sender profile for board={board} camera={camera}")
    selected = dict(matches[0])
    selected["template_path"] = str((root / str(selected["template"])).resolve())
    return selected


def render_sender_config(
    root: Path,
    profile_manifest: Path,
    output: Path,
    sender_id: str,
    rotation: int | None,
    receiver_host: str,
    detection: dict[str, Any],
    board_override: str = "",
    camera_override: str = "",
) -> dict[str, Any]:
    board = board_override or str(detection["board"])
    camera_family = camera_override or str(detection["camera"]["family"])
    profile = select_sender_profile(
        root,
        profile_manifest,
        board,
        camera_family,
    )
    config = copy.deepcopy(load_json(Path(profile["template_path"])))
    config["sender_id"] = sender_id
    config.setdefault("receiver", {})["ip"] = receiver_host
    config.setdefault("clock_sync", {})["receiver_ip"] = receiver_host
    config.setdefault("receiver_discovery", {})
    config["receiver_discovery"].update(
        {
            "enabled": True,
            "port": 50009,
            "interval_ms": 1000,
            "sticky_timeout_ms": 120000,
        }
    )
    config.setdefault("hotplug", {})["enabled"] = True
    for index, camera in enumerate(config.get("cameras") or []):
        camera["camera_id"] = f"cam{index + 1:02d}"
        if rotation is not None:
            camera["rotation_degrees"] = rotation
        camera.pop("serial_number", None)
        camera.pop("uid", None)
    write_json(output, config)
    return profile


def replace_home(value: Any, old_home: str, new_home: str) -> Any:
    if isinstance(value, dict):
        return {key: replace_home(item, old_home, new_home) for key, item in value.items()}
    if isinstance(value, list):
        return [replace_home(item, old_home, new_home) for item in value]
    if isinstance(value, str) and value.startswith(old_home):
        return new_home + value[len(old_home) :]
    return value


def render_receiver_config(template: Path, output: Path, home: Path, uid: int, gid: int) -> None:
    config = replace_home(copy.deepcopy(load_json(template)), "/home/loop", str(home))
    config.setdefault("nas_auto_mount", {})["uid"] = uid
    config.setdefault("nas_auto_mount", {})["gid"] = gid
    write_json(output, config)


def render_button_configs(root: Path, sender_id: str, button_output: Path, power_output: Path) -> None:
    base = root / "12_apps/recording_buttons"
    button = load_json(base / "config_lubancat-52d2ef0c.json")
    power = load_json(base / "config_lubancat-52d2ef0c_power.json")
    button["sender_id"] = sender_id
    event_devices = sorted(
        Path("/dev/input/by-path").glob("platform-2ac40000.i2c-platform-rk805-pwrkey.*.auto-event")
    )
    if event_devices:
        power["event_device"] = str(event_devices[0])
    write_json(button_output, button)
    write_json(power_output, power)


def direct_neighbor_mac(host: str) -> tuple[str, str]:
    try:
        address = socket.gethostbyname(host)
        ipaddress.ip_address(address)
    except (OSError, ValueError) as error:
        raise RuntimeError(f"cannot resolve NAS host {host}: {error}") from error
    route = command_output(["ip", "route", "get", address])
    if not route:
        raise RuntimeError(f"NAS host has no route: {address}")
    if re.search(r"\bvia\b", route.splitlines()[0]):
        return address, ""
    subprocess.run(["ping", "-c", "1", "-W", "1", address], check=False, capture_output=True)
    neighbor = command_output(["ip", "neigh", "show", address])
    match = re.search(r"\blladdr\s+([0-9a-fA-F:]{17})\b", neighbor)
    mac = match.group(1).lower() if match else ""
    return address, mac if MAC_PATTERN.fullmatch(mac) else ""


def render_nas_target(output: Path, host: str, share: str) -> dict[str, Any]:
    if not re.fullmatch(r"[A-Za-z0-9_$.-]{1,80}", share):
        raise ValueError("invalid SMB share name")
    address, mac = direct_neighbor_mac(host)
    suffix = mac.replace(":", "") if mac else hashlib.sha256(address.encode()).hexdigest()[:12]
    target = {
        "nas_id": f"manual-{suffix}",
        "host": address,
        "share": share,
        "mac": mac,
        "source": "bootstrap",
        "updated_us": time.time_ns() // 1000,
    }
    write_json(output, target, mode=0o600)
    return target


def discover_receiver(port: int, timeout_ms: int) -> dict[str, Any] | None:
    sequence = time.time_ns() // 1000
    payload = json.dumps(
        {
            "protocol_version": "3.0",
            "message_type": "receiver_discovery_request",
            "sender_id": "provisioning",
            "sequence": sequence,
            "preferred_receiver_id": "",
        },
        separators=(",", ":"),
    ).encode()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind(("0.0.0.0", 0))
        sock.settimeout(timeout_ms / 1000.0)
        broadcasts = {"255.255.255.255"}
        try:
            interfaces = json.loads(command_output(["ip", "-j", "-4", "address", "show", "up"]))
            for interface in interfaces:
                for address in interface.get("addr_info", []):
                    broadcast = address.get("broadcast")
                    if address.get("scope") == "global" and isinstance(broadcast, str):
                        broadcasts.add(broadcast)
        except (json.JSONDecodeError, TypeError):
            pass
        for broadcast in broadcasts:
            try:
                sock.sendto(payload, (broadcast, port))
            except OSError:
                continue
        deadline = time.monotonic() + timeout_ms / 1000.0
        while time.monotonic() < deadline:
            try:
                response_payload, peer = sock.recvfrom(8192)
                response = json.loads(response_payload.decode())
            except socket.timeout:
                return None
            except (OSError, UnicodeDecodeError, json.JSONDecodeError):
                continue
            if (
                isinstance(response, dict)
                and response.get("protocol_version") == "3.0"
                and response.get("message_type") == "receiver_discovery_response"
                and int(response.get("sequence", -1)) == sequence
            ):
                response["host"] = peer[0]
                return response
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    detect_parser = subparsers.add_parser("detect")
    detect_parser.add_argument("--json", action="store_true")

    identity_parser = subparsers.add_parser("identity")
    identity_parser.add_argument("--board", required=True)
    identity_parser.add_argument("--sender-id", default="")

    sender_parser = subparsers.add_parser("sender-config")
    sender_parser.add_argument("--root", type=Path, required=True)
    sender_parser.add_argument("--profiles", type=Path, required=True)
    sender_parser.add_argument("--output", type=Path, required=True)
    sender_parser.add_argument("--sender-id", required=True)
    sender_parser.add_argument("--rotation", choices=("auto", "0", "180"), default="auto")
    sender_parser.add_argument("--receiver-host", required=True)
    sender_parser.add_argument("--board", choices=("rk3588", "orangepi5pro", "lubancat"), default="")
    sender_parser.add_argument("--camera", choices=("sv1301s", "gemini305"), default="")

    receiver_parser = subparsers.add_parser("receiver-config")
    receiver_parser.add_argument("--template", type=Path, required=True)
    receiver_parser.add_argument("--output", type=Path, required=True)
    receiver_parser.add_argument("--home", type=Path, required=True)
    receiver_parser.add_argument("--uid", type=int, required=True)
    receiver_parser.add_argument("--gid", type=int, required=True)

    relocate_parser = subparsers.add_parser("relocate-json")
    relocate_parser.add_argument("--template", type=Path, required=True)
    relocate_parser.add_argument("--output", type=Path, required=True)
    relocate_parser.add_argument("--old-prefix", required=True)
    relocate_parser.add_argument("--new-prefix", required=True)

    nas_parser = subparsers.add_parser("nas-target")
    nas_parser.add_argument("--output", type=Path, required=True)
    nas_parser.add_argument("--host", required=True)
    nas_parser.add_argument("--share", required=True)

    discovery_parser = subparsers.add_parser("discover-receiver")
    discovery_parser.add_argument("--port", type=int, default=50009)
    discovery_parser.add_argument("--timeout-ms", type=int, default=1500)

    button_parser = subparsers.add_parser("button-config")
    button_parser.add_argument("--root", type=Path, required=True)
    button_parser.add_argument("--sender-id", required=True)
    button_parser.add_argument("--button-output", type=Path, required=True)
    button_parser.add_argument("--power-output", type=Path, required=True)

    args = parser.parse_args()
    if args.command == "detect":
        result = detect_hardware()
        print(json.dumps(result, ensure_ascii=False, indent=2) if args.json else result)
        return 0
    if args.command == "identity":
        sender_id, source = sender_id_for(args.board, args.sender_id)
        print(json.dumps({"sender_id": sender_id, "source": source}))
        return 0
    if args.command == "sender-config":
        detection = detect_hardware()
        profile = render_sender_config(
            args.root.resolve(),
            args.profiles.resolve(),
            args.output.resolve(),
            args.sender_id,
            None if args.rotation == "auto" else int(args.rotation),
            args.receiver_host,
            detection,
            args.board,
            args.camera,
        )
        print(json.dumps({"config": str(args.output.resolve()), "profile": profile}, ensure_ascii=False))
        return 0
    if args.command == "receiver-config":
        render_receiver_config(args.template, args.output, args.home, args.uid, args.gid)
        print(args.output.resolve())
        return 0
    if args.command == "relocate-json":
        value = replace_home(load_json(args.template), args.old_prefix, args.new_prefix)
        write_json(args.output, value)
        print(args.output.resolve())
        return 0
    if args.command == "nas-target":
        print(json.dumps(render_nas_target(args.output, args.host, args.share)))
        return 0
    if args.command == "discover-receiver":
        result = discover_receiver(args.port, args.timeout_ms)
        if result is None:
            return 1
        print(json.dumps(result))
        return 0
    if args.command == "button-config":
        if not ID_PATTERN.fullmatch(args.sender_id):
            raise ValueError("invalid sender ID")
        render_button_configs(args.root, args.sender_id, args.button_output, args.power_output)
        return 0
    return 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        print(f"provisioning error: {error}", file=sys.stderr)
        raise SystemExit(2)
