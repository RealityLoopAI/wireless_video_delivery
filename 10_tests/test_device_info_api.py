#!/usr/bin/env python3
import ast
from pathlib import Path


def load_builder(source_path: Path):
    source = source_path.read_text(encoding="utf-8")
    tree = ast.parse(source)
    function = next(
        node for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name == "_build_device_info"
    )
    module = ast.Module(body=[function], type_ignores=[])
    namespace = {"Any": object, "datetime": __import__("datetime").datetime, "time": __import__("time")}
    exec(compile(module, str(source_path), "exec"), namespace)
    return namespace["_build_device_info"]


def main():
    root = Path(__file__).resolve().parents[1]
    build = load_builder(root / "09_web_monitor" / "server.py")
    status = {
        "cameras": [
            {
                "sender_id": "sender-a",
                "camera_id": "cam02",
                "sender_source_ip": "192.168.1.11",
                "online": False,
                "device_info_version": 1,
                "host_name": "sender-a-host",
                "wifi_interface": "wlan0",
                "wifi_permanent_mac": "AA:BB:CC:DD:EE:01",
                "mac_is_permanent": True,
                "mac_source": "ethtool_perm_addr",
                "device_date": "2026-09-08",
                "device_time": "2026-09-08T12:00:00+08:00",
                "device_system_time_us": 1788840000000000,
                "timezone": "Asia/Hong_Kong",
                "device_info_received_us": 1788840000000100,
            },
            {
                "sender_id": "sender-a",
                "camera_id": "cam01",
                "sender_source_ip": "192.168.1.11",
                "online": True,
                "device_info_version": 1,
                "wifi_permanent_mac": "aa:bb:cc:dd:ee:01",
                "device_info_received_us": 1788839999999999,
            },
            {
                "sender_id": "legacy-sender",
                "camera_id": "cam01",
                "online": True,
            },
        ]
    }

    result = build(status, None, None)
    assert result["ok"] is True
    assert result["device_count"] == 2
    sender = next(item for item in result["devices"] if item["sender_id"] == "sender-a")
    assert sender["online"] is True
    assert sender["camera_ids"] == ["cam01", "cam02"]
    assert sender["wifi_permanent_mac"] == "aa:bb:cc:dd:ee:01"
    assert sender["mac_is_permanent"] is True
    assert sender["device_date"] == "2026-09-08"

    filtered = build(status, "sender-a", None)
    assert filtered["device_count"] == 1
    filtered = build(status, None, "AA:BB:CC:DD:EE:01")
    assert filtered["device_count"] == 1
    missing = build(status, "missing", None)
    assert missing["devices"] == []
    print("device info API unit test passed")


if __name__ == "__main__":
    main()
