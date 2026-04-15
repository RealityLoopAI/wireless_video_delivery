from __future__ import annotations

import argparse
import logging
import threading
import time

from .camera import CameraSource
from .config import SenderConfig, load_sender_config
from .encoder import Encoder
from .frame_queue import FrameQueue
from .models import VideoFrame
from .rtp import RtpH264Packetizer
from .state import RuntimeState
from .stats import StatsTracker
from .transport import TransportTx


logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")


class SenderService:
    def __init__(self, cfg: SenderConfig) -> None:
        self.cfg = cfg
        self.camera = CameraSource()
        self.encoder = Encoder()
        self.packetizer = RtpH264Packetizer(cfg.network.mtu)
        self.transport = TransportTx()
        self.capture_queue: FrameQueue[VideoFrame] = FrameQueue(cfg.runtime.queue_size)
        self.stats = StatsTracker()
        self.state = RuntimeState()
        self.stop_event = threading.Event()
        self.capture_started = threading.Event()
        self._threads: list[threading.Thread] = []
        self._base_drop_divisor = max(1, cfg.runtime.drop_frame_divisor)
        self._active_drop_divisor = self._base_drop_divisor
        self._healthy_windows = 0
        self._reconnect_lock = threading.Lock()
        self._reconnect_done = threading.Event()
        self._reconnect_done.set()

    def _open_pipeline(self) -> None:
        self.transport.open(
            self.cfg.network.remote_ip,
            self.cfg.network.remote_port,
            self.cfg.network.ttl,
            self.cfg.network.socket_buffer_bytes,
        )
        self.camera.start(self.cfg.camera)
        self.state.mark_success()
        self.stats.set_state(self.state.name)

    def _reconnect(self, error: Exception) -> None:
        if self.stop_event.is_set():
            return
        if not self._reconnect_lock.acquire(blocking=False):
            self._reconnect_done.wait(timeout=1.0)
            return
        self._reconnect_done.clear()
        try:
            self.camera.stop()
            self.transport.close()
            self.state.enter_reconnecting()
            self.stats.set_state(self.state.name, str(error))
            self.stats.add_reconnect()
            while not self.stop_event.is_set():
                wait_ms = self.state.next_backoff_ms(self.cfg.runtime.reconnect_backoff_ms)
                if self.stop_event.wait(wait_ms / 1000.0):
                    return
                try:
                    self._open_pipeline()
                    return
                except Exception as exc:
                    logging.exception("reconnect attempt failed")
                    self.state.enter_reconnecting()
                    self.stats.set_state(self.state.name, str(exc))
                    self.stats.add_reconnect()
        finally:
            self._reconnect_done.set()
            self._reconnect_lock.release()

    def _update_adaptive_drop(self, queue_depth: int, send_drop_count: int) -> None:
        if not self.cfg.runtime.adaptive_drop_enabled:
            self.stats.update_extra(drop_divisor=self._active_drop_divisor)
            return
        max_divisor = max(self._base_drop_divisor, self.cfg.runtime.adaptive_drop_max_divisor)
        # Only treat a full queue as congestion; near-full transient spikes
        # should not trigger aggressive frame dropping in quality-first mode.
        high_watermark = max(1, self.cfg.runtime.queue_size)
        congested = send_drop_count > 0 or queue_depth >= high_watermark
        if congested:
            self._active_drop_divisor = min(max_divisor, self._active_drop_divisor + 1)
            self._healthy_windows = 0
        else:
            self._healthy_windows += 1
            if (
                self._active_drop_divisor > self._base_drop_divisor
                and self._healthy_windows >= max(1, self.cfg.runtime.adaptive_drop_recover_window)
            ):
                self._active_drop_divisor -= 1
                self._healthy_windows = 0
        self.stats.update_extra(drop_divisor=self._active_drop_divisor)

    def capture_thread(self) -> None:
        while not self.stop_event.is_set():
            try:
                frame = self.camera.read(self.cfg.runtime.camera_timeout_ms)
                if frame is None:
                    self.state.mark_failure(self.cfg.runtime.degraded_threshold)
                    self.stats.set_state(self.state.name, "CAM_TIMEOUT")
                    continue
                self.capture_started.set()
                self.capture_queue.push_latest(frame)
                self.stats.mark_in()
                self.stats.set_queue_depth(self.capture_queue.size())
                self.stats.set_drop_count(self.capture_queue.drops)
                self.state.mark_success()
                self.stats.set_state(self.state.name)
            except Exception as exc:
                logging.exception("capture failed")
                if self.stop_event.is_set():
                    break
                self._reconnect(exc)

    def encode_send_thread(self) -> None:
        opened = False
        frame_count = 0
        while not self.stop_event.is_set():
            frame = self.capture_queue.pop(100)
            if frame is None:
                continue
            if not opened:
                self.encoder.open(self.cfg.codec, frame.width, frame.height, self.cfg.camera.fps)
                opened = True
            frame_count += 1
            divisor = self._active_drop_divisor
            if divisor > 1 and frame_count % divisor != 0:
                continue
            try:
                packet_count = 0
                send_drop_count = 0
                for packet in self.encoder.encode(frame):
                    rtp_packets = self.packetizer.packetize(packet)
                    for raw in rtp_packets:
                        if self.transport.send(raw):
                            self.stats.mark_packet(len(raw))
                            packet_count += 1
                        else:
                            send_drop_count += 1
                self.stats.add_tx_drop_count(send_drop_count)
                if packet_count > 0:
                    # fps_out tracks encoded frame output rate.
                    self.stats.mark_out(0)
                queue_depth = self.capture_queue.size()
                self.stats.set_queue_depth(queue_depth)
                self._update_adaptive_drop(queue_depth, send_drop_count)
            except Exception as exc:
                logging.exception("encode/send failed")
                if self.stop_event.is_set():
                    break
                self._reconnect(exc)
                opened = False

    def watchdog_thread(self) -> None:
        last_log = 0.0
        while not self.stop_event.is_set():
            now = time.monotonic()
            if now - last_log >= self.cfg.runtime.log_interval_ms / 1000.0:
                stat = self.stats.snapshot()
                active_divisor = int(stat.extra.get("drop_divisor", self._active_drop_divisor))
                logging.info(
                    "state=%s fps_in=%.1f fps_out=%.1f pkt_rate=%.1f bitrate=%.0fkbps queue=%d drops=%d tx_drop=%d div=%d reconnect=%d",
                    stat.state,
                    stat.fps_in,
                    stat.fps_out,
                    stat.packet_rate,
                    stat.bitrate_kbps,
                    stat.queue_depth,
                    stat.drop_count,
                    stat.tx_drop_count,
                    active_divisor,
                    stat.reconnect_count,
                )
                last_log = now
            time.sleep(self.cfg.runtime.watchdog_interval_ms / 1000.0)

    def run(self) -> int:
        self._open_pipeline()
        self._threads = [
            threading.Thread(target=self.capture_thread, name="capture_thread"),
            threading.Thread(target=self.encode_send_thread, name="encode_send_thread"),
            threading.Thread(target=self.watchdog_thread, name="watchdog_thread"),
        ]
        for thread in self._threads:
            thread.start()
        try:
            while True:
                time.sleep(0.5)
        except KeyboardInterrupt:
            self.stop_event.set()
        finally:
            self.stop_event.set()
            for thread in self._threads:
                thread.join(timeout=2.0)
            self.camera.stop()
            self.transport.close()
            self.encoder.close()
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Gemini RTP sender")
    parser.add_argument("--config", required=True, help="sender json config")
    args = parser.parse_args()
    service = SenderService(load_sender_config(args.config))
    return service.run()
