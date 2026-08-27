#!/usr/bin/env python3
"""Relay receiver-side Annex-B H.264 streams to independent RTP/UDP ports."""

from __future__ import annotations

import argparse
import json
import logging
import signal
import struct
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402


LOG = logging.getLogger("gwv3.rgb_rtp_relay")
GWHP_MAGIC = b"GWHP"
GWHP_MIN_HEADER_SIZE = 40
GWHP_MAX_HEADER_SIZE = 256
DEFAULT_MAX_PAYLOAD_BYTES = 16 * 1024 * 1024


@dataclass(frozen=True)
class RelayRoute:
    sender_id: str
    camera_id: str
    port: int
    enabled: bool = True
    fps: int = 30

    @property
    def camera_key(self) -> str:
        return f"{self.sender_id}_{self.camera_id}"


@dataclass(frozen=True)
class RelayConfig:
    receiver_admin_url: str
    target_host: str
    payload_type: int
    mtu_bytes: int
    udp_buffer_bytes: int
    reconnect_delay_ms: int
    maximum_reconnect_delay_ms: int
    log_interval_seconds: int
    max_payload_bytes: int
    routes: tuple[RelayRoute, ...]


@dataclass(frozen=True)
class GwhpFrame:
    version: int
    flags: int
    width: int
    height: int
    timestamp_us: int
    sequence: int
    global_timestamp_us: int
    payload: bytes


def _required_string(value: object, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{field} must be a non-empty string")
    return value.strip()


def _integer(value: object, field: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum or value > maximum:
        raise ValueError(f"{field} must be an integer in [{minimum}, {maximum}]")
    return value


def load_config(path: Path) -> RelayConfig:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError("config root must be an object")

    receiver_admin_url = _required_string(
        raw.get("receiver_admin_url", "http://127.0.0.1:18080"), "receiver_admin_url"
    ).rstrip("/")
    parsed_admin = urllib.parse.urlsplit(receiver_admin_url)
    if parsed_admin.scheme not in ("http", "https") or not parsed_admin.hostname:
        raise ValueError("receiver_admin_url must be an HTTP URL")

    target_host = _required_string(raw.get("target_host"), "target_host")
    payload_type = _integer(raw.get("payload_type", 96), "payload_type", 96, 127)
    mtu_bytes = _integer(raw.get("mtu_bytes", 1200), "mtu_bytes", 576, 9000)
    udp_buffer_bytes = _integer(
        raw.get("udp_buffer_bytes", 4 * 1024 * 1024), "udp_buffer_bytes", 64 * 1024, 64 * 1024 * 1024
    )
    reconnect_delay_ms = _integer(raw.get("reconnect_delay_ms", 1000), "reconnect_delay_ms", 100, 60_000)
    maximum_reconnect_delay_ms = _integer(
        raw.get("maximum_reconnect_delay_ms", 10_000), "maximum_reconnect_delay_ms", reconnect_delay_ms, 120_000
    )
    log_interval_seconds = _integer(raw.get("log_interval_seconds", 30), "log_interval_seconds", 1, 3600)
    max_payload_bytes = _integer(
        raw.get("max_payload_bytes", DEFAULT_MAX_PAYLOAD_BYTES),
        "max_payload_bytes",
        1024,
        256 * 1024 * 1024,
    )

    raw_routes = raw.get("routes")
    if not isinstance(raw_routes, list) or not raw_routes:
        raise ValueError("routes must be a non-empty array")
    routes: list[RelayRoute] = []
    camera_keys: set[str] = set()
    ports: set[int] = set()
    for index, item in enumerate(raw_routes):
        if not isinstance(item, dict):
            raise ValueError(f"routes[{index}] must be an object")
        route = RelayRoute(
            sender_id=_required_string(item.get("sender_id"), f"routes[{index}].sender_id"),
            camera_id=_required_string(item.get("camera_id"), f"routes[{index}].camera_id"),
            port=_integer(item.get("port"), f"routes[{index}].port", 1, 65535),
            enabled=item.get("enabled", True),
            fps=_integer(item.get("fps", 30), f"routes[{index}].fps", 1, 240),
        )
        if not isinstance(route.enabled, bool):
            raise ValueError(f"routes[{index}].enabled must be a boolean")
        if route.camera_key in camera_keys:
            raise ValueError(f"duplicate camera route: {route.camera_key}")
        if route.port in ports:
            raise ValueError(f"duplicate RTP port: {route.port}")
        camera_keys.add(route.camera_key)
        ports.add(route.port)
        routes.append(route)

    return RelayConfig(
        receiver_admin_url=receiver_admin_url,
        target_host=target_host,
        payload_type=payload_type,
        mtu_bytes=mtu_bytes,
        udp_buffer_bytes=udp_buffer_bytes,
        reconnect_delay_ms=reconnect_delay_ms,
        maximum_reconnect_delay_ms=maximum_reconnect_delay_ms,
        log_interval_seconds=log_interval_seconds,
        max_payload_bytes=max_payload_bytes,
        routes=tuple(routes),
    )


def read_exact(stream: BinaryIO, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        block = stream.read(size - len(data))
        if not block:
            raise EOFError("unexpected end of GWHP stream")
        data.extend(block)
    return bytes(data)


def read_gwhp_frame(stream: BinaryIO, max_payload_bytes: int = DEFAULT_MAX_PAYLOAD_BYTES) -> GwhpFrame:
    prefix = read_exact(stream, 8)
    magic, version, header_size = struct.unpack("<4sHH", prefix)
    if magic != GWHP_MAGIC:
        raise ValueError("invalid GWHP magic")
    if version not in (1, 2):
        raise ValueError(f"unsupported GWHP version: {version}")
    if header_size < GWHP_MIN_HEADER_SIZE or header_size > GWHP_MAX_HEADER_SIZE:
        raise ValueError(f"invalid GWHP header size: {header_size}")
    header = prefix + read_exact(stream, header_size - len(prefix))
    payload_size, flags, width, height = struct.unpack_from("<IIII", header, 8)
    timestamp_us, sequence = struct.unpack_from("<QQ", header, 24)
    global_timestamp_us = struct.unpack_from("<Q", header, 40)[0] if header_size >= 48 else 0
    if payload_size <= 0 or payload_size > max_payload_bytes:
        raise ValueError(f"invalid GWHP payload size: {payload_size}")
    return GwhpFrame(
        version=version,
        flags=flags,
        width=width,
        height=height,
        timestamp_us=timestamp_us,
        sequence=sequence,
        global_timestamp_us=global_timestamp_us,
        payload=read_exact(stream, payload_size),
    )


class RtpPipeline:
    def __init__(self, config: RelayConfig, route: RelayRoute):
        target = config.target_host.replace("\\", "\\\\").replace('"', '\\"')
        description = (
            "appsrc name=source is-live=true format=time block=true max-bytes=16777216 "
            'caps="video/x-h264,stream-format=(string)byte-stream,alignment=(string)au" '
            "! queue max-size-buffers=16 max-size-bytes=0 max-size-time=0 "
            f"! rtph264pay pt={config.payload_type} mtu={config.mtu_bytes} config-interval=1 aggregate-mode=none "
            f'! udpsink host="{target}" port={route.port} sync=false async=false '
            f"buffer-size={config.udp_buffer_bytes}"
        )
        self._pipeline = Gst.parse_launch(description)
        self._source = self._pipeline.get_by_name("source")
        if self._source is None:
            raise RuntimeError("failed to create RTP appsrc")
        self._bus = self._pipeline.get_bus()
        self._frame_duration_ns = Gst.SECOND // route.fps
        self._frame_index = 0
        result = self._pipeline.set_state(Gst.State.PLAYING)
        if result == Gst.StateChangeReturn.FAILURE:
            self.close()
            raise RuntimeError("failed to start RTP pipeline")

    def push(self, payload: bytes) -> None:
        message = self._bus.pop_filtered(Gst.MessageType.ERROR | Gst.MessageType.EOS)
        if message is not None:
            if message.type == Gst.MessageType.ERROR:
                error, debug = message.parse_error()
                raise RuntimeError(f"GStreamer RTP pipeline error: {error}; {debug or ''}")
            raise RuntimeError("GStreamer RTP pipeline reached EOS")
        buffer = Gst.Buffer.new_allocate(None, len(payload), None)
        buffer.fill(0, payload)
        buffer.pts = self._frame_index * self._frame_duration_ns
        buffer.dts = buffer.pts
        buffer.duration = self._frame_duration_ns
        self._frame_index += 1
        result = self._source.emit("push-buffer", buffer)
        if result != Gst.FlowReturn.OK:
            raise RuntimeError(f"GStreamer RTP push failed: {result.value_nick}")

    def close(self) -> None:
        source = getattr(self, "_source", None)
        if source is not None:
            try:
                source.emit("end-of-stream")
            except Exception:
                pass
        pipeline = getattr(self, "_pipeline", None)
        if pipeline is not None:
            pipeline.set_state(Gst.State.NULL)


class RelayWorker:
    def __init__(self, config: RelayConfig, route: RelayRoute, stop_event: threading.Event):
        self.config = config
        self.route = route
        self.stop_event = stop_event
        self.frames = 0
        self.bytes = 0
        self.reconnects = 0
        self.last_sequence = 0
        self.last_frame_monotonic = 0.0
        self._thread = threading.Thread(target=self.run, name=f"rtp-{route.camera_key}", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def join(self) -> None:
        self._thread.join()

    def stream_url(self) -> str:
        query = urllib.parse.urlencode(
            {
                "sender_id": self.route.sender_id,
                "camera_id": self.route.camera_id,
                "quality": "main",
                "metadata": "global",
            }
        )
        return f"{self.config.receiver_admin_url}/api/preview/rgb-h264-frames?{query}"

    def run(self) -> None:
        pipeline: RtpPipeline | None = None
        delay_ms = self.config.reconnect_delay_ms
        next_warning = 0.0
        next_report = time.monotonic() + self.config.log_interval_seconds
        try:
            while not self.stop_event.is_set():
                if pipeline is None:
                    try:
                        pipeline = RtpPipeline(self.config, self.route)
                        LOG.info(
                            "RTP route ready camera=%s target=%s:%d",
                            self.route.camera_key,
                            self.config.target_host,
                            self.route.port,
                        )
                    except Exception as error:
                        now = time.monotonic()
                        if now >= next_warning:
                            LOG.warning("RTP pipeline unavailable camera=%s error=%s", self.route.camera_key, error)
                            next_warning = now + max(5.0, self.config.log_interval_seconds)
                        if self.stop_event.wait(delay_ms / 1000.0):
                            break
                        delay_ms = min(delay_ms * 2, self.config.maximum_reconnect_delay_ms)
                        continue
                try:
                    request = urllib.request.Request(self.stream_url(), headers={"Connection": "close"})
                    with urllib.request.urlopen(request, timeout=5) as response:
                        self.reconnects += 1
                        delay_ms = self.config.reconnect_delay_ms
                        LOG.info("main stream connected camera=%s reconnects=%d", self.route.camera_key, self.reconnects)
                        while not self.stop_event.is_set():
                            frame = read_gwhp_frame(response, self.config.max_payload_bytes)
                            pipeline.push(frame.payload)
                            self.frames += 1
                            self.bytes += len(frame.payload)
                            self.last_sequence = frame.sequence
                            self.last_frame_monotonic = time.monotonic()
                            now = time.monotonic()
                            if now >= next_report:
                                LOG.info(
                                    "RTP route stats camera=%s target=%s:%d frames=%d bytes=%d last_seq=%d",
                                    self.route.camera_key,
                                    self.config.target_host,
                                    self.route.port,
                                    self.frames,
                                    self.bytes,
                                    self.last_sequence,
                                )
                                next_report = now + self.config.log_interval_seconds
                except (EOFError, OSError, TimeoutError, ValueError, urllib.error.URLError) as error:
                    now = time.monotonic()
                    if now >= next_warning:
                        LOG.warning("RTP route unavailable camera=%s error=%s", self.route.camera_key, error)
                        next_warning = now + max(5.0, self.config.log_interval_seconds)
                    if self.stop_event.wait(delay_ms / 1000.0):
                        break
                    delay_ms = min(delay_ms * 2, self.config.maximum_reconnect_delay_ms)
                except Exception as error:
                    LOG.exception("RTP route pipeline failed camera=%s error=%s", self.route.camera_key, error)
                    if pipeline is not None:
                        pipeline.close()
                        pipeline = None
                    if self.stop_event.wait(delay_ms / 1000.0):
                        break
        finally:
            if pipeline is not None:
                pipeline.close()
            LOG.info("RTP route stopped camera=%s frames=%d bytes=%d", self.route.camera_key, self.frames, self.bytes)


def run(config: RelayConfig) -> int:
    Gst.init(None)
    missing_elements = [
        name for name in ("appsrc", "queue", "rtph264pay", "udpsink") if Gst.ElementFactory.find(name) is None
    ]
    if missing_elements:
        raise RuntimeError(f"missing required GStreamer elements: {', '.join(missing_elements)}")
    stop_event = threading.Event()

    def request_stop(_signum: int, _frame: object) -> None:
        stop_event.set()

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    workers = [RelayWorker(config, route, stop_event) for route in config.routes if route.enabled]
    if not workers:
        raise ValueError("no enabled RTP routes")
    LOG.info(
        "starting RGB RTP relay routes=%d target=%s payload_type=%d mtu=%d",
        len(workers),
        config.target_host,
        config.payload_type,
        config.mtu_bytes,
    )
    for worker in workers:
        worker.start()
    while not stop_event.wait(1):
        pass
    for worker in workers:
        worker.join()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--check-config", action="store_true")
    parser.add_argument("--log-level", default="INFO")
    args = parser.parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    config = load_config(args.config)
    if args.check_config:
        LOG.info("configuration valid routes=%d target=%s", len(config.routes), config.target_host)
        return 0
    return run(config)


if __name__ == "__main__":
    raise SystemExit(main())
