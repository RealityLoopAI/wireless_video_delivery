from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import List


@dataclass
class CameraConfig:
    width: int = 1280
    height: int = 720
    fps: int = 30
    format_priority: List[str] = field(default_factory=lambda: ["RGB", "MJPG", "YUYV", "NV12"])


@dataclass
class CodecConfig:
    codec: str = "h264"
    bitrate_kbps: int = 3000
    gop: int = 30
    max_b_frames: int = 0
    preset: str = "ultrafast"
    tune: str = "zerolatency"


@dataclass
class NetworkTxConfig:
    remote_ip: str = "127.0.0.1"
    remote_port: int = 5600
    mtu: int = 1200
    ttl: int = 64
    socket_buffer_bytes: int = 4 * 1024 * 1024


@dataclass
class NetworkRxConfig:
    listen_ip: str = "0.0.0.0"
    listen_port: int = 5600
    jitter_ms: int = 80
    socket_buffer_bytes: int = 4 * 1024 * 1024


@dataclass
class RuntimeConfig:
    queue_size: int = 4
    camera_timeout_ms: int = 100
    recv_timeout_ms: int = 100
    watchdog_interval_ms: int = 1000
    degraded_threshold: int = 10
    reconnect_backoff_ms: List[int] = field(default_factory=lambda: [1000, 2000, 5000, 10000])
    log_interval_ms: int = 2000
    drop_frame_divisor: int = 1


@dataclass
class DisplayConfig:
    window_name: str = "Gemini Receiver"
    show_stats: bool = True


@dataclass
class SenderConfig:
    camera: CameraConfig = field(default_factory=CameraConfig)
    codec: CodecConfig = field(default_factory=CodecConfig)
    network: NetworkTxConfig = field(default_factory=NetworkTxConfig)
    runtime: RuntimeConfig = field(default_factory=RuntimeConfig)


@dataclass
class ReceiverConfig:
    network: NetworkRxConfig = field(default_factory=NetworkRxConfig)
    runtime: RuntimeConfig = field(default_factory=RuntimeConfig)
    display: DisplayConfig = field(default_factory=DisplayConfig)


def _load_json(path: str | Path) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _camera_config(data: dict) -> CameraConfig:
    return CameraConfig(**data)


def _codec_config(data: dict) -> CodecConfig:
    return CodecConfig(**data)


def _network_tx_config(data: dict) -> NetworkTxConfig:
    return NetworkTxConfig(**data)


def _network_rx_config(data: dict) -> NetworkRxConfig:
    return NetworkRxConfig(**data)


def _runtime_config(data: dict) -> RuntimeConfig:
    return RuntimeConfig(**data)


def _display_config(data: dict) -> DisplayConfig:
    return DisplayConfig(**data)


def load_sender_config(path: str | Path) -> SenderConfig:
    raw = _load_json(path)
    return SenderConfig(
        camera=_camera_config(raw.get("camera", {})),
        codec=_codec_config(raw.get("codec", {})),
        network=_network_tx_config(raw.get("network", {})),
        runtime=_runtime_config(raw.get("runtime", {})),
    )


def load_receiver_config(path: str | Path) -> ReceiverConfig:
    raw = _load_json(path)
    return ReceiverConfig(
        network=_network_rx_config(raw.get("network", {})),
        runtime=_runtime_config(raw.get("runtime", {})),
        display=_display_config(raw.get("display", {})),
    )

