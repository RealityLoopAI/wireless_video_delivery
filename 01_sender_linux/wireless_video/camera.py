from __future__ import annotations

import itertools
import logging
import time
from typing import Optional

import cv2
import numpy as np
from pyorbbecsdk import Config, OBError, OBFormat, OBPropertyID, OBSensorType, Pipeline

from .config import CameraConfig
from .models import VideoFrame

LOG = logging.getLogger(__name__)


def _reshape_u8(data: np.ndarray, expected_size: int, shape: tuple[int, ...]) -> Optional[np.ndarray]:
    if data.size < expected_size:
        return None
    return data[:expected_size].reshape(shape)


def _frame_to_image(frame) -> Optional[tuple[np.ndarray, str]]:
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
        # Keep RGB as-is to avoid per-frame color-space conversion cost.
        return image, "RGB8"
    if color_format == OBFormat.BGR:
        image = _reshape_u8(data, width * height * 3, (height, width, 3))
        if image is None:
            return None
        return image, "BGR8"
    if color_format == OBFormat.MJPG:
        image = cv2.imdecode(data, cv2.IMREAD_COLOR)
        if image is None:
            return None
        return image, "BGR8"
    if color_format == OBFormat.YUYV:
        image = _reshape_u8(data, width * height * 2, (height, width, 2))
        if image is None:
            return None
        return cv2.cvtColor(image, cv2.COLOR_YUV2BGR_YUY2), "BGR8"
    if color_format == OBFormat.NV12:
        image = _reshape_u8(data, width * height * 3 // 2, (height * 3 // 2, width))
        if image is None:
            return None
        return cv2.cvtColor(image, cv2.COLOR_YUV2BGR_NV12), "BGR8"
    if color_format == OBFormat.NV21:
        image = _reshape_u8(data, width * height * 3 // 2, (height * 3 // 2, width))
        if image is None:
            return None
        return cv2.cvtColor(image, cv2.COLOR_YUV2BGR_NV21), "BGR8"
    if color_format == OBFormat.UYVY:
        image = _reshape_u8(data, width * height * 2, (height, width, 2))
        if image is None:
            return None
        return cv2.cvtColor(image, cv2.COLOR_YUV2BGR_UYVY), "BGR8"
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
        self._apply_color_controls(cfg)
        return True

    def _apply_color_controls(self, cfg: CameraConfig) -> None:
        if self._pipeline is None:
            return
        try:
            device = self._pipeline.get_device()
            device.set_bool_property(OBPropertyID.OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, cfg.auto_exposure)
            if cfg.auto_exposure:
                return
            if cfg.exposure is not None:
                exposure = self._clamp_int_property(
                    device,
                    OBPropertyID.OB_PROP_COLOR_EXPOSURE_INT,
                    cfg.exposure,
                )
                device.set_int_property(OBPropertyID.OB_PROP_COLOR_EXPOSURE_INT, exposure)
            if cfg.gain is not None:
                gain = self._clamp_int_property(
                    device,
                    OBPropertyID.OB_PROP_COLOR_GAIN_INT,
                    cfg.gain,
                )
                device.set_int_property(OBPropertyID.OB_PROP_COLOR_GAIN_INT, gain)
        except Exception as exc:
            LOG.warning("apply color controls failed: %s", exc)

    @staticmethod
    def _clamp_int_property(device, property_id, requested: int) -> int:
        range_info = device.get_int_property_range(property_id)
        value = max(range_info.min, min(range_info.max, requested))
        if range_info.step > 1:
            value = range_info.min + ((value - range_info.min) // range_info.step) * range_info.step
        return int(value)

    def read(self, timeout_ms: int = 100) -> Optional[VideoFrame]:
        if self._pipeline is None:
            return None
        frames = self._pipeline.wait_for_frames(timeout_ms)
        if frames is None:
            return None
        color_frame = frames.get_color_frame()
        if color_frame is None:
            return None
        converted = _frame_to_image(color_frame)
        if converted is None:
            return None
        image, pixel_fmt = converted
        capture_ts = int(time.time() * 1000)
        return VideoFrame(
            frame_id=next(self._frame_seq),
            capture_ts_ms=capture_ts,
            width=image.shape[1],
            height=image.shape[0],
            pixel_fmt=pixel_fmt,
            data=image,
        )

    def stop(self) -> None:
        if self._pipeline is not None:
            try:
                self._pipeline.stop()
            except OBError:
                pass
        self._pipeline = None
