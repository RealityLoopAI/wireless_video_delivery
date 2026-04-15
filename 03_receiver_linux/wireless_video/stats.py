from __future__ import annotations

import threading
import time
from collections import deque

from .models import StreamStat


class StatsTracker:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._stat = StreamStat()
        self._in_times: deque[float] = deque(maxlen=120)
        self._out_times: deque[float] = deque(maxlen=120)
        self._packet_times: deque[float] = deque(maxlen=240)
        self._latencies: deque[float] = deque(maxlen=120)
        self._bitrate_bytes = 0
        self._bitrate_started = time.monotonic()

    def _fps(self, values: deque[float]) -> float:
        if len(values) < 2:
            return 0.0
        duration = values[-1] - values[0]
        if duration <= 0:
            return 0.0
        return (len(values) - 1) / duration

    def mark_in(self) -> None:
        now = time.monotonic()
        with self._lock:
            self._in_times.append(now)
            self._stat.fps_in = self._fps(self._in_times)

    def mark_out(self, payload_size: int, latency_ms: float | None = None) -> None:
        now = time.monotonic()
        with self._lock:
            self._out_times.append(now)
            self._stat.fps_out = self._fps(self._out_times)
            self._bitrate_bytes += payload_size
            elapsed = now - self._bitrate_started
            if elapsed >= 1.0:
                self._stat.bitrate_kbps = (self._bitrate_bytes * 8) / 1000.0 / elapsed
                self._bitrate_started = now
                self._bitrate_bytes = 0
            if latency_ms is not None:
                self._latencies.append(latency_ms)
                self._stat.e2e_latency_ms_avg = sum(self._latencies) / len(self._latencies)

    def mark_packet(self, payload_size: int) -> None:
        now = time.monotonic()
        with self._lock:
            self._packet_times.append(now)
            self._stat.packet_rate = self._fps(self._packet_times)
            self._bitrate_bytes += payload_size
            elapsed = now - self._bitrate_started
            if elapsed >= 1.0:
                self._stat.bitrate_kbps = (self._bitrate_bytes * 8) / 1000.0 / elapsed
                self._bitrate_started = now
                self._bitrate_bytes = 0

    def set_queue_depth(self, depth: int) -> None:
        with self._lock:
            self._stat.queue_depth = depth

    def set_drop_count(self, count: int) -> None:
        with self._lock:
            self._stat.drop_count = count

    def add_tx_drop_count(self, count: int = 1) -> None:
        if count <= 0:
            return
        with self._lock:
            self._stat.tx_drop_count += count

    def add_reconnect(self) -> None:
        with self._lock:
            self._stat.reconnect_count += 1

    def add_packets_rx(self, count: int = 1) -> None:
        with self._lock:
            self._stat.packets_rx += count

    def add_packets_lost(self, count: int = 1) -> None:
        with self._lock:
            self._stat.packets_lost += count

    def set_state(self, state: str, error: str = "") -> None:
        with self._lock:
            self._stat.state = state
            self._stat.last_error = error

    def update_extra(self, **kwargs: object) -> None:
        with self._lock:
            self._stat.extra.update(kwargs)

    def snapshot(self) -> StreamStat:
        with self._lock:
            stat = StreamStat(**self._stat.__dict__)
            stat.extra = dict(self._stat.extra)
            return stat
