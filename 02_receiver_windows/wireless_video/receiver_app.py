from __future__ import annotations

import argparse
import heapq
import logging
import threading
import time
from itertools import count

from .config import ReceiverConfig, load_receiver_config
from .decoder import Decoder
from .frame_queue import FrameQueue
from .models import FrameAssembly, VideoFrame
from .renderer import Renderer
from .state import RuntimeState
from .stats import StatsTracker
from .transport import TransportRx


logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")


class ReceiverService:
    def __init__(self, cfg: ReceiverConfig) -> None:
        self.cfg = cfg
        self.transport = TransportRx()
        self.decoder = Decoder()
        self.renderer = Renderer(cfg.display.window_name, cfg.display.show_stats)
        self.decoded_queue: FrameQueue[VideoFrame] = FrameQueue(cfg.runtime.queue_size)
        self.assembly_heap: list[tuple[int, int, FrameAssembly]] = []
        self.heap_lock = threading.Lock()
        self._heap_seq = count()
        self.stats = StatsTracker()
        self.state = RuntimeState()
        self.stop_event = threading.Event()
        self._last_lost_packets = 0
        self._last_render_monotonic = time.monotonic()
        self._last_rx_for_stall = 0

    def _push_assembly(self, assembly: FrameAssembly) -> None:
        # Use a monotonic sequence as tie-breaker to avoid comparing FrameAssembly
        # objects when multiple frames have the same millisecond arrival timestamp.
        with self.heap_lock:
            heapq.heappush(self.assembly_heap, (assembly.arrival_ts_ms, next(self._heap_seq), assembly))

    def _pop_due_assembly(self, now_ms: int, jitter_ms: int) -> FrameAssembly | None:
        with self.heap_lock:
            if self.assembly_heap and now_ms - self.assembly_heap[0][0] >= jitter_ms:
                _, _, assembly = heapq.heappop(self.assembly_heap)
                return assembly
        return None

    def _open(self) -> None:
        self.transport.open(
            self.cfg.network.listen_ip,
            self.cfg.network.listen_port,
            self.cfg.network.socket_buffer_bytes,
            self.cfg.runtime.recv_timeout_ms,
        )
        self.decoder.open()
        self.state.mark_success()
        self.stats.set_state(self.state.name)

    def recv_thread(self) -> None:
        while not self.stop_event.is_set():
            try:
                pkt = self.transport.recv()
                if pkt is None:
                    self.state.mark_failure(self.cfg.runtime.degraded_threshold)
                    self.stats.set_state(self.state.name, "NET_RECV_TIMEOUT")
                    continue
                self.stats.add_packets_rx()
                self.stats.mark_packet(len(pkt.payload))
                assembly = self.transport.push(pkt)
                lost_delta = self.transport.lost_packets - self._last_lost_packets
                if lost_delta > 0:
                    self.stats.add_packets_lost(lost_delta)
                    self._last_lost_packets = self.transport.lost_packets
                if assembly is None:
                    continue
                self.stats.mark_in()
                self._push_assembly(assembly)
                self.state.mark_success()
                self.stats.set_state(self.state.name)
            except Exception as exc:
                logging.exception("receive failed")
                self.state.enter_reconnecting()
                self.stats.set_state(self.state.name, str(exc))
                time.sleep(self.state.next_backoff_ms(self.cfg.runtime.reconnect_backoff_ms) / 1000.0)

    def decode_thread(self) -> None:
        jitter_ms = self.cfg.network.jitter_ms
        while not self.stop_event.is_set():
            now_ms = int(time.time() * 1000)
            assembly = self._pop_due_assembly(now_ms=now_ms, jitter_ms=jitter_ms)
            if assembly is None:
                time.sleep(0.005)
                continue
            try:
                for frame in self.decoder.decode(assembly):
                    self.decoded_queue.push_latest(frame)
                    latency_ms = max(0, now_ms - frame.capture_ts_ms)
                    self.stats.mark_out(0, latency_ms=latency_ms)
                    self.stats.set_queue_depth(self.decoded_queue.size())
                    self.stats.set_drop_count(self.decoded_queue.drops)
            except Exception as exc:
                self.state.mark_failure(self.cfg.runtime.degraded_threshold)
                self.stats.set_state(self.state.name, f"DECODE_FAIL: {exc}")

    def watchdog_thread(self) -> None:
        last_log = 0.0
        while not self.stop_event.is_set():
            now = time.monotonic()
            if now - last_log >= self.cfg.runtime.log_interval_ms / 1000.0:
                stat = self.stats.snapshot()
                if stat.packets_rx > self._last_rx_for_stall and now - self._last_render_monotonic > 2.0:
                    logging.warning(
                        "display stalled: packets are still incoming (rx=%d) but no new frame rendered for %.1fs",
                        stat.packets_rx,
                        now - self._last_render_monotonic,
                    )
                self._last_rx_for_stall = stat.packets_rx
                logging.info(
                    "state=%s fps_in=%.1f fps_out=%.1f pkt_rate=%.1f latency=%.1fms rx=%d lost=%d queue=%d",
                    stat.state,
                    stat.fps_in,
                    stat.fps_out,
                    stat.packet_rate,
                    stat.e2e_latency_ms_avg,
                    stat.packets_rx,
                    stat.packets_lost,
                    stat.queue_depth,
                )
                last_log = now
            time.sleep(self.cfg.runtime.watchdog_interval_ms / 1000.0)

    def run(self) -> int:
        self._open()
        threads = [
            threading.Thread(target=self.recv_thread, name="recv_thread", daemon=True),
            threading.Thread(target=self.decode_thread, name="decode_thread", daemon=True),
            threading.Thread(target=self.watchdog_thread, name="watchdog_thread", daemon=True),
        ]
        for thread in threads:
            thread.start()
        try:
            while not self.stop_event.is_set():
                frame = self.decoded_queue.pop(50)
                if frame is not None:
                    stat = self.stats.snapshot()
                    keep_running = self.renderer.render(frame, stat)
                    self._last_render_monotonic = time.monotonic()
                    if not keep_running:
                        self.stop_event.set()
                        break
                else:
                    keep_running = self.renderer.poll_events()
                    if not keep_running:
                        self.stop_event.set()
                        break
        except KeyboardInterrupt:
            self.stop_event.set()
        finally:
            self.transport.close()
            self.decoder.close()
            self.renderer.close()
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Gemini RTP receiver")
    parser.add_argument("--config", required=True, help="receiver json config")
    args = parser.parse_args()
    service = ReceiverService(load_receiver_config(args.config))
    return service.run()
