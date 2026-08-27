#!/usr/bin/env python3
import importlib.util
import io
import json
import struct
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "05_tools" / "rgb_rtp_relay.py"


def load_module():
    spec = importlib.util.spec_from_file_location("rgb_rtp_relay", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def make_frame(payload: bytes, version: int = 2) -> bytes:
    header_size = 48 if version == 2 else 40
    header = struct.pack(
        "<4sHHIIIIQQ",
        b"GWHP",
        version,
        header_size,
        len(payload),
        7,
        1920,
        1080,
        123456,
        99,
    )
    if version == 2:
        header += struct.pack("<Q", 123999)
    return header + payload


def test_gwhp_parser(module):
    first = module.read_gwhp_frame(io.BytesIO(make_frame(b"abc", 2)))
    assert first.version == 2
    assert first.flags == 7
    assert first.width == 1920
    assert first.height == 1080
    assert first.timestamp_us == 123456
    assert first.global_timestamp_us == 123999
    assert first.sequence == 99
    assert first.payload == b"abc"
    legacy = module.read_gwhp_frame(io.BytesIO(make_frame(b"legacy", 1)))
    assert legacy.global_timestamp_us == 0
    assert legacy.payload == b"legacy"


def test_gwhp_parser_rejects_invalid_input(module):
    broken = bytearray(make_frame(b"abc", 2))
    broken[:4] = b"NOPE"
    try:
        module.read_gwhp_frame(io.BytesIO(broken))
    except ValueError as error:
        assert "magic" in str(error)
    else:
        raise AssertionError("invalid magic was accepted")

    oversized = bytearray(make_frame(b"abc", 2))
    struct.pack_into("<I", oversized, 8, 1000)
    try:
        module.read_gwhp_frame(io.BytesIO(oversized), max_payload_bytes=100)
    except ValueError as error:
        assert "payload size" in str(error)
    else:
        raise AssertionError("oversized payload was accepted")


def write_config(path: Path, routes: list[dict]) -> None:
    path.write_text(
        json.dumps(
            {
                "receiver_admin_url": "http://127.0.0.1:18080",
                "target_host": "192.168.66.226",
                "routes": routes,
            }
        ),
        encoding="utf-8",
    )


def test_config_validation(module):
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / "relay.json"
        write_config(
            path,
            [
                {"sender_id": "sender-a", "camera_id": "cam01", "port": 5600},
                {"sender_id": "sender-b", "camera_id": "cam01", "port": 5602},
            ],
        )
        config = module.load_config(path)
        assert config.target_host == "192.168.66.226"
        assert [route.port for route in config.routes] == [5600, 5602]

        write_config(
            path,
            [
                {"sender_id": "sender-a", "camera_id": "cam01", "port": 5600},
                {"sender_id": "sender-b", "camera_id": "cam01", "port": 5600},
            ],
        )
        try:
            module.load_config(path)
        except ValueError as error:
            assert "duplicate RTP port" in str(error)
        else:
            raise AssertionError("duplicate RTP port was accepted")


def main() -> int:
    module = load_module()
    test_gwhp_parser(module)
    test_gwhp_parser_rejects_invalid_input(module)
    test_config_validation(module)
    print("rgb RTP relay tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
