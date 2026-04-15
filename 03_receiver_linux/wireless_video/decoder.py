from __future__ import annotations

import av

from .models import FrameAssembly, VideoFrame


class Decoder:
    def __init__(self) -> None:
        self._ctx: av.codec.context.CodecContext | None = None

    def open(self) -> bool:
        self._ctx = av.CodecContext.create("h264", "r")
        return True

    def decode(self, assembly: FrameAssembly) -> list[VideoFrame]:
        if self._ctx is None:
            return []
        packet = av.Packet(assembly.payload)
        frames = self._ctx.decode(packet)
        out = []
        for index, frame in enumerate(frames):
            image = frame.to_ndarray(format="bgr24")
            out.append(
                VideoFrame(
                    frame_id=assembly.frame_id if assembly.frame_id else index,
                    capture_ts_ms=assembly.capture_ts_ms,
                    width=image.shape[1],
                    height=image.shape[0],
                    pixel_fmt="BGR8",
                    data=image,
                )
            )
        return out

    def close(self) -> None:
        self._ctx = None
