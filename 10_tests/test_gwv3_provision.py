#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import tempfile
from pathlib import Path


def load_module(path: Path):
    spec = importlib.util.spec_from_file_location("gwv3_provision_test", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    module = load_module(root / "05_tools/gwv3_provision.py")

    sv = module.classify_camera(
        [
            {"vendor_id": "2bc5", "product_id": "0511", "speed_mbps": "480"},
            {"vendor_id": "2bc5", "product_id": "0614", "speed_mbps": "5000"},
        ]
    )
    assert sv["family"] == "sv1301s"
    assert sv["sdk"] == "v1"
    assert sv["usb3"] is True

    gemini = module.classify_camera(
        [{"vendor_id": "2bc5", "product_id": "0840", "speed_mbps": "5000"}]
    )
    assert gemini["family"] == "gemini305"
    assert gemini["sdk"] == "v2"

    assert module.classify_board("LubanCat RK3576", "test") == "lubancat"
    assert module.classify_board("Orange Pi 5 Pro", "test") == "orangepi5pro"
    assert module.classify_board("Rockchip RK3588 EVB", "test") == "rk3588"

    detection = {"board": "rk3588", "camera": {"family": "sv1301s"}}
    with tempfile.TemporaryDirectory(prefix="gwv3-provision-test-") as temporary_text:
        output = Path(temporary_text) / "sender.json"
        profile = module.render_sender_config(
            root,
            root / "06_configs/deployment/sender-profiles.json",
            output,
            "rk3588-test1234",
            None,
            "192.168.1.196",
            detection,
        )
        config = json.loads(output.read_text(encoding="utf-8"))
        assert profile["sdk"] == "v1"
        assert config["sender_id"] == "rk3588-test1234"
        assert config["receiver"]["ip"] == "192.168.1.196"
        assert config["clock_sync"]["receiver_ip"] == "192.168.1.196"
        assert config["receiver_discovery"]["enabled"] is True
        assert config["hotplug"]["enabled"] is True
        assert "deployment" not in config
        assert "serial_number" not in config["cameras"][0]
        assert "uid" not in config["cameras"][0]

    print("GWV3 provisioning unit test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
