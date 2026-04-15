from __future__ import annotations

import time
from fractions import Fraction
from typing import List

import av

from .config import CodecConfig
from .models import EncodedPacket, VideoFrame


class Encoder:
    def __init__(self) -> None:
        self._ctx: av.codec.context.CodecContext | None = None

    def open(self, cfg: CodecConfig, width: int, height: int, fps: int) -> bool:
        self._ctx = av.CodecContext.create(cfg.codec, "w")
        self._ctx.width = width
        self._ctx.height = height
        self._ctx.pix_fmt = "yuv420p"
        self._ctx.time_base = Fraction(1, fps)
        self._ctx.framerate = Fraction(fps, 1)
        self._ctx.bit_rate = cfg.bitrate_kbps * 1000
        self._ctx.gop_size = cfg.gop
        self._ctx.max_b_frames = cfg.max_b_frames
        self._ctx.options = {
            "preset": cfg.preset,
            "tune": cfg.tune,
            "profile": "baseline",
            "annexb": "1",
        }
        self._ctx.open()
        return True

    def encode(self, frame: VideoFrame) -> List[EncodedPacket]:
        if self._ctx is None:
            return []
        video_frame = av.VideoFrame.from_ndarray(frame.data, format="bgr24")
        packets = self._ctx.encode(video_frame)
        out = []
        encode_ts_ms = int(time.time() * 1000)
        for pkt in packets:
            out.append(
                EncodedPacket(
                    frame_id=frame.frame_id,
                    capture_ts_ms=frame.capture_ts_ms,
                    encode_ts_ms=encode_ts_ms,
                    codec="H264",
                    payload=bytes(pkt),
                    is_keyframe=bool(pkt.is_keyframe),
                )
            )
        return out

    def flush(self) -> List[EncodedPacket]:
        if self._ctx is None:
            return []
        packets = self._ctx.encode(None)
        out = []
        now_ms = int(time.time() * 1000)
        for pkt in packets:
            out.append(
                EncodedPacket(
                    frame_id=0,
                    capture_ts_ms=now_ms,
                    encode_ts_ms=now_ms,
                    codec="H264",
                    payload=bytes(pkt),
                    is_keyframe=bool(pkt.is_keyframe),
                )
            )
        return out

    def close(self) -> None:
        self._ctx = None
