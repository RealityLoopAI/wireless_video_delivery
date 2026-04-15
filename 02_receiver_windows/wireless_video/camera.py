from __future__ import annotations

import itertools
import time
from typing import Optional

import cv2
import numpy as np
from pyorbbecsdk import Config, OBError, OBFormat, OBSensorType, Pipeline

from .config import CameraConfig
from .models import VideoFrame


def _reshape_u8(data: np.ndarray, expected_size: int, shape: tuple[int, ...]) -> Optional[np.ndarray]:
    if data.size < expected_size:
        return None
    return data[:expected_size].reshape(shape)


def _frame_to_bgr_image(frame) -> Optional[np.ndarray]:
    width = frame.get_width()
    height = frame.get_height()
    color_format = frame.get_format()
    raw = frame.get_data()
    try:
        data = np.frombuffer(raw, dtype=np.uint8)
    except TypeError:
        data = np.asarray(raw, dtype=np.uint8).reshape(-1)
    if color_format == OBFormat.RGB:
        image = _reshape_u8(data, width * height * 3, (height, width, 3))
        if image is None:
            return None
        return cv2.cvtColor(image, cv2.COLOR_RGB2BGR)
    if color_format == OBFormat.BGR:
        image = _reshape_u8(data, width * height * 3, (height, width, 3))
        if image is None:
            return None
        return image
    if color_format == OBFormat.MJPG:
        return cv2.imdecode(data, cv2.IMREAD_COLOR)
    if color_format == OBFormat.YUYV:
        image = _reshape_u8(data, width * height * 2, (height, width, 2))
        if image is None:
            return None
        return cv2.cvtColor(image, cv2.COLOR_YUV2BGR_YUY2)
    if color_format == OBFormat.NV12:
        image = _reshape_u8(data, width * height * 3 // 2, (height * 3 // 2, width))
        if image is None:
            return None
        return cv2.cvtColor(image, cv2.COLOR_YUV2BGR_NV12)
    if color_format == OBFormat.NV21:
        image = _reshape_u8(data, width * height * 3 // 2, (height * 3 // 2, width))
        if image is None:
            return None
        return cv2.cvtColor(image, cv2.COLOR_YUV2BGR_NV21)
    if color_format == OBFormat.UYVY:
        image = _reshape_u8(data, width * height * 2, (height, width, 2))
        if image is None:
            return None
        return cv2.cvtColor(image, cv2.COLOR_YUV2BGR_UYVY)
    return None


class CameraSource:
    def __init__(self) -> None:
        self._pipeline: Optional[Pipeline] = None
        self._config = Config()
        self._frame_seq = itertools.count(1)

    def start(self, cfg: CameraConfig) -> bool:
        self._pipeline = Pipeline()
        profile_list = self._pipeline.get_stream_profile_list(OBSensorType.COLOR_SENSOR)
        selected = None
        formats = {
            "RGB": OBFormat.RGB,
            "MJPG": OBFormat.MJPG,
            "YUYV": OBFormat.YUYV,
            "NV12": OBFormat.NV12,
            "NV21": OBFormat.NV21,
            "UYVY": OBFormat.UYVY,
        }
        for name in cfg.format_priority:
            try:
                selected = profile_list.get_video_stream_profile(cfg.width, cfg.height, formats[name], cfg.fps)
                break
            except Exception:
                continue
        if selected is None:
            selected = profile_list.get_default_video_stream_profile()
        self._config.enable_stream(selected)
        self._pipeline.start(self._config)
        return True

    def read(self, timeout_ms: int = 100) -> Optional[VideoFrame]:
        if self._pipeline is None:
            return None
        frames = self._pipeline.wait_for_frames(timeout_ms)
        if frames is None:
            return None
        color_frame = frames.get_color_frame()
        if color_frame is None:
            return None
        image = _frame_to_bgr_image(color_frame)
        if image is None:
            return None
        capture_ts = int(time.time() * 1000)
        return VideoFrame(
            frame_id=next(self._frame_seq),
            capture_ts_ms=capture_ts,
            width=image.shape[1],
            height=image.shape[0],
            pixel_fmt="BGR8",
            data=image,
        )

    def stop(self) -> None:
        if self._pipeline is not None:
            try:
                self._pipeline.stop()
            except OBError:
                pass
        self._pipeline = None
