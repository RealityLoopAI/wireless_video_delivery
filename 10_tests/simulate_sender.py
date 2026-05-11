#!/usr/bin/env python3
import argparse
import json
import socket
import struct
import time


MAGIC = 0x33565747
HEADER_VERSION = 1
HEADER_SIZE = 94
STREAM_DEPTH = 2
PIXEL_DEPTH_U16 = 2
FLAG_HAS_SYSTEM_TIMESTAMP = 1 << 3


def status(sock: socket.socket, host: str, port: int, payload: dict) -> None:
    sock.sendto((json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8"), (host, port))


def depth_payload(width: int, height: int, frame_id: int) -> bytes:
    data = bytearray(width * height * 2)
    for y in range(height):
        base = y * width
        for x in range(width):
            value = (x + y + frame_id * 3) % 5000
            struct.pack_into("<H", data, (base + x) * 2, value)
    return bytes(data)


def media_packet(sender_id: str, camera_id: str, frame_id: int, width: int, height: int, payload: bytes) -> bytes:
    sender = sender_id.encode("ascii")
    camera = camera_id.encode("ascii")
    codec = b"none"
    timestamp_us = int(time.time() * 1_000_000)
    header = struct.pack(
        "<IHHBBIHHHQQQQIIHQQ16s",
        MAGIC,
        HEADER_VERSION,
        HEADER_SIZE,
        STREAM_DEPTH,
        0,
        FLAG_HAS_SYSTEM_TIMESTAMP,
        len(sender),
        len(camera),
        len(codec),
        frame_id,
        timestamp_us,
        timestamp_us,
        0,
        width,
        height,
        PIXEL_DEPTH_U16,
        len(payload),
        len(payload),
        b"\x00" * 16,
    )
    assert len(header) == HEADER_SIZE
    return header + sender + camera + codec + payload


def main() -> int:
    parser = argparse.ArgumentParser(description="Simulate a Gemini v3 sender for receiver testing")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--status-port", type=int, default=50011)
    parser.add_argument("--media-port", type=int, default=50010)
    parser.add_argument("--sender-id", default="orangepi5pro-01")
    parser.add_argument("--camera-id", default="cam01")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=400)
    parser.add_argument("--frames", type=int, default=30)
    parser.add_argument("--fps", type=float, default=30.0)
    args = parser.parse_args()

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    now = int(time.time() * 1_000_000)
    status(
        udp,
        args.host,
        args.status_port,
        {
            "protocol_version": "3.0",
            "message_type": "sender_hello",
            "sender_id": args.sender_id,
            "timestamp_us": now,
            "sender_version": "simulator",
            "host_name": "simulator",
            "os": "linux",
            "arch": "x86_64",
            "capabilities": {"rgb_encoding": ["h264"], "depth_compression": ["none"]},
        },
    )
    status(
        udp,
        args.host,
        args.status_port,
        {
            "protocol_version": "3.0",
            "message_type": "camera_announce",
            "sender_id": args.sender_id,
            "camera_id": args.camera_id,
            "timestamp_us": now,
            "device": {"vendor": "orbbec", "model": "simulated"},
            "rgb_profile": {"width": 640, "height": 480, "fps": 30, "pixel_format": "encoded_video", "codec": "h264"},
            "depth_profile": {
                "width": args.width,
                "height": args.height,
                "fps": int(args.fps),
                "pixel_format": "uint16",
                "compression": "none",
                "depth_scale": 1,
            },
            "calibration": {"available": False, "source": "simulator", "data": {}},
        },
    )

    with socket.create_connection((args.host, args.media_port), timeout=5) as tcp:
        delay = 1.0 / args.fps if args.fps > 0 else 0
        for frame_id in range(args.frames):
            payload = depth_payload(args.width, args.height, frame_id)
            tcp.sendall(media_packet(args.sender_id, args.camera_id, frame_id, args.width, args.height, payload))
            status(
                udp,
                args.host,
                args.status_port,
                {
                    "protocol_version": "3.0",
                    "message_type": "heartbeat",
                    "sender_id": args.sender_id,
                    "timestamp_us": int(time.time() * 1_000_000),
                    "uptime_ms": frame_id * int(delay * 1000),
                    "cameras": [{"camera_id": args.camera_id, "online": True}],
                },
            )
            if delay:
                time.sleep(delay)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
