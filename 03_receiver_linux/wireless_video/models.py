from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

import numpy as np


@dataclass
class VideoFrame:
    frame_id: int
    capture_ts_ms: int
    width: int
    height: int
    pixel_fmt: str
    data: np.ndarray


@dataclass
class EncodedPacket:
    frame_id: int
    capture_ts_ms: int
    encode_ts_ms: int
    codec: str
    payload: bytes
    is_keyframe: bool = False


@dataclass
class StreamStat:
    fps_in: float = 0.0
    fps_out: float = 0.0
    packet_rate: float = 0.0
    queue_depth: int = 0
    bitrate_kbps: float = 0.0
    drop_count: int = 0
    tx_drop_count: int = 0
    reconnect_count: int = 0
    e2e_latency_ms_avg: float = 0.0
    packets_rx: int = 0
    packets_lost: int = 0
    state: str = "INIT"
    last_error: str = ""
    extra: dict = field(default_factory=dict)


@dataclass
class RtpPacket:
    seq: int
    timestamp: int
    marker: bool
    payload_type: int
    ssrc: int
    payload: bytes
    capture_ts_ms: int = 0
    frame_id: int = 0
    send_ts_ms: int = 0


@dataclass
class FrameAssembly:
    frame_id: int
    capture_ts_ms: int
    payload: bytes
    is_keyframe: bool
    arrival_ts_ms: int
    timestamp: int
    seq_end: Optional[int] = None
