#!/usr/bin/env python3
import argparse
import json
import logging
import signal
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

LOG = logging.getLogger("recording-led")


@dataclass(frozen=True)
class LedConfig:
    sender_id: str
    receiver_base_url: str
    chip: str
    line_offset: int
    active_high: bool
    blink_interval_seconds: float
    status_poll_interval_seconds: float
    request_timeout_seconds: float
    status_stale_seconds: float


def load_config(path: Path) -> LedConfig:
    with path.open("r", encoding="utf-8") as source:
        raw = json.load(source)
    led = raw.get("recording_led", {})
    config = LedConfig(
        sender_id=str(raw["sender_id"]).strip(),
        receiver_base_url=str(raw["receiver_base_url"]).rstrip("/"),
        chip=str(led.get("chip", "gpiochip4")),
        line_offset=int(led.get("line_offset", 19)),
        active_high=bool(led.get("active_high", True)),
        blink_interval_seconds=float(led.get("blink_interval_ms", 500)) / 1000.0,
        status_poll_interval_seconds=float(
            led.get("status_poll_interval_ms", 250)
        ) / 1000.0,
        request_timeout_seconds=float(led.get("request_timeout_seconds", 1.0)),
        status_stale_seconds=float(led.get("status_stale_ms", 5000)) / 1000.0,
    )
    if not config.sender_id:
        raise ValueError("sender_id must not be empty")
    if config.line_offset < 0:
        raise ValueError("recording_led.line_offset must not be negative")
    if config.blink_interval_seconds <= 0:
        raise ValueError("recording_led.blink_interval_ms must be positive")
    if config.status_poll_interval_seconds <= 0:
        raise ValueError("recording_led.status_poll_interval_ms must be positive")
    if config.request_timeout_seconds <= 0:
        raise ValueError("recording_led.request_timeout_seconds must be positive")
    if config.status_stale_seconds <= 0:
        raise ValueError("recording_led.status_stale_ms must be positive")
    return config


class GpioLed:
    def __init__(self, chip_name: str, line_offset: int, active_high: bool):
        import gpiod

        self._active_high = active_high
        self._gpiod = gpiod
        self._chip = gpiod.Chip(chip_name)
        self._line = self._chip.get_line(line_offset)
        self._line.request(
            consumer="gwv3-recording-led",
            type=gpiod.LINE_REQ_DIR_OUT,
            default_vals=[self._physical_value(True)],
        )
        self._on = True

    def _physical_value(self, on: bool) -> int:
        return int(on if self._active_high else not on)

    def set(self, on: bool):
        if self._on == on:
            return
        self._line.set_value(self._physical_value(on))
        self._on = on

    def close(self):
        try:
            self._line.set_value(self._physical_value(False))
        finally:
            self._line.release()
            self._chip.close()


def fetch_recording_state(config: LedConfig) -> bool:
    request = Request(f"{config.receiver_base_url}/api/status", method="GET")
    try:
        with urlopen(request, timeout=config.request_timeout_seconds) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except (HTTPError, URLError, OSError, TimeoutError, json.JSONDecodeError) as exc:
        raise RuntimeError(str(exc)) from exc
    if not isinstance(payload, dict):
        raise RuntimeError("status endpoint returned a non-object JSON response")
    if payload.get("receiver_admin_stale") is True:
        raise RuntimeError("receiver status is stale")
    cameras = [
        camera
        for camera in payload.get("cameras", [])
        if isinstance(camera, dict) and camera.get("sender_id") == config.sender_id
    ]
    if not cameras:
        raise RuntimeError(f"sender not present in receiver status: {config.sender_id}")
    return any(bool(camera.get("recording")) for camera in cameras)


class RecordingLedService:
    def __init__(
        self,
        config: LedConfig,
        led,
        state_fetcher: Callable[[LedConfig], bool] = fetch_recording_state,
        clock: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], None] = time.sleep,
    ):
        self.config = config
        self.led = led
        self.state_fetcher = state_fetcher
        self.clock = clock
        self.sleep = sleep
        self.stop_event = threading.Event()
        self.indicating_recording = False
        self.led_on = True
        self.next_toggle = 0.0
        self.last_error_log = 0.0
        self._state_lock = threading.Lock()
        self._reported_recording = False
        self._last_status_success = None
        self._status_thread = threading.Thread(
            target=self._poll_status,
            name="recording-led-status",
            daemon=True,
        )

    def request_stop(self):
        self.stop_event.set()

    def _poll_status(self):
        while not self.stop_event.is_set():
            try:
                recording = self.state_fetcher(self.config)
                now = self.clock()
                with self._state_lock:
                    self._reported_recording = recording
                    self._last_status_success = now
                self.last_error_log = 0.0
            except RuntimeError as exc:
                now = self.clock()
                if self.last_error_log == 0.0 or now - self.last_error_log >= 10.0:
                    LOG.warning("recording status unavailable error=%s", exc)
                    self.last_error_log = now
            self.stop_event.wait(self.config.status_poll_interval_seconds)

    def _recording_state_is_fresh(self, now: float) -> bool:
        with self._state_lock:
            return bool(
                self._reported_recording
                and self._last_status_success is not None
                and now - self._last_status_success <= self.config.status_stale_seconds
            )

    def poll_once(self):
        now = self.clock()
        recording = self._recording_state_is_fresh(now)
        if recording and not self.indicating_recording:
            self.indicating_recording = True
            if not self.led_on:
                self.led.set(True)
                self.led_on = True
            self.next_toggle = now + self.config.blink_interval_seconds
            LOG.info("recording detected; LED blinking")
        elif not recording and self.indicating_recording:
            self.indicating_recording = False
            if not self.led_on:
                self.led.set(True)
            self.led_on = True
            LOG.info("recording stopped or status stale; LED steady on")

        if recording and now >= self.next_toggle:
            self.led_on = not self.led_on
            self.led.set(self.led_on)
            self.next_toggle = now + self.config.blink_interval_seconds
        elif not recording and not self.led_on:
            self.led_on = True
            self.led.set(True)

    def run(self):
        LOG.info(
            "recording LED service started sender=%s chip=%s line=%d active_high=%s "
            "blink_ms=%.0f status_poll_ms=%.0f stale_ms=%.0f",
            self.config.sender_id,
            self.config.chip,
            self.config.line_offset,
            self.config.active_high,
            self.config.blink_interval_seconds * 1000.0,
            self.config.status_poll_interval_seconds * 1000.0,
            self.config.status_stale_seconds * 1000.0,
        )
        self._status_thread.start()
        try:
            while not self.stop_event.is_set():
                self.poll_once()
                self.sleep(min(0.05, self.config.status_poll_interval_seconds))
        finally:
            self.stop_event.set()
            self._status_thread.join(timeout=self.config.request_timeout_seconds + 1.0)
            self.led.close()
            LOG.info("recording LED service stopped; LED off")


def main():
    parser = argparse.ArgumentParser(description="GWV3 recording status LED service")
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    config = load_config(args.config)
    led = GpioLed(config.chip, config.line_offset, config.active_high)
    service = RecordingLedService(config, led)
    signal.signal(signal.SIGINT, lambda _signum, _frame: service.request_stop())
    signal.signal(signal.SIGTERM, lambda _signum, _frame: service.request_stop())
    service.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
