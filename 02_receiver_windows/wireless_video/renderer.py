from __future__ import annotations

import time

import cv2

from .models import StreamStat, VideoFrame


class Renderer:
    def __init__(self, window_name: str, show_stats: bool) -> None:
        self._window_name = window_name
        self._show_stats = show_stats

    def render(self, frame: VideoFrame, overlay_stat: StreamStat) -> bool:
        image = frame.data.copy()
        if self._show_stats:
            lines = [
                f"state={overlay_stat.state}",
                f"fps_in={overlay_stat.fps_in:.1f} fps_out={overlay_stat.fps_out:.1f} pkt={overlay_stat.packet_rate:.1f}",
                f"bitrate={overlay_stat.bitrate_kbps:.0f}kbps queue={overlay_stat.queue_depth}",
                f"latency={overlay_stat.e2e_latency_ms_avg:.1f}ms drop={overlay_stat.drop_count}",
                f"rx={overlay_stat.packets_rx} lost={overlay_stat.packets_lost}",
            ]
            for idx, line in enumerate(lines):
                cv2.putText(image, line, (20, 30 + idx * 28), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
        cv2.imshow(self._window_name, image)
        return self.poll_events()

    def poll_events(self) -> bool:
        key = cv2.waitKey(1)
        return key not in (27, ord("q"))

    def close(self) -> None:
        cv2.destroyAllWindows()
