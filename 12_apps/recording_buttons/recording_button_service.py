#!/usr/bin/env python3
import argparse
import json
import logging
import os
import queue
import signal
import threading
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, Optional
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


LOG = logging.getLogger("recording-buttons")


@dataclass(frozen=True)
class ButtonConfig:
    name: str
    action: str
    cue: str
    adc_path: Path


@dataclass(frozen=True)
class ServiceConfig:
    receiver_base_url: str
    speech_base_url: str
    hold_seconds: float
    poll_interval_seconds: float
    debounce_seconds: float
    pressed_below: int
    released_above: int
    request_timeout_seconds: float
    retry_count: int
    retry_delay_seconds: float
    buttons: tuple


def load_config(path: Path) -> ServiceConfig:
    with path.open("r", encoding="utf-8") as source:
        raw = json.load(source)

    buttons = tuple(
        ButtonConfig(
            name=str(item["name"]),
            action=str(item["action"]),
            cue=str(item["cue"]),
            adc_path=Path(item["adc_path"]),
        )
        for item in raw["buttons"]
    )
    if {button.action for button in buttons} != {"start", "stop"}:
        raise ValueError("buttons must define exactly one start and one stop action")

    config = ServiceConfig(
        receiver_base_url=str(raw["receiver_base_url"]).rstrip("/"),
        speech_base_url=str(raw["speech_base_url"]).rstrip("/"),
        hold_seconds=float(raw.get("hold_seconds", 2.0)),
        poll_interval_seconds=float(raw.get("poll_interval_ms", 20)) / 1000.0,
        debounce_seconds=float(raw.get("debounce_ms", 60)) / 1000.0,
        pressed_below=int(raw.get("pressed_below", 800)),
        released_above=int(raw.get("released_above", 2000)),
        request_timeout_seconds=float(raw.get("request_timeout_seconds", 2.0)),
        retry_count=int(raw.get("retry_count", 3)),
        retry_delay_seconds=float(raw.get("retry_delay_ms", 500)) / 1000.0,
        buttons=buttons,
    )
    if config.hold_seconds <= 0 or config.poll_interval_seconds <= 0:
        raise ValueError("hold and poll intervals must be positive")
    if config.debounce_seconds < 0:
        raise ValueError("debounce interval cannot be negative")
    if config.pressed_below >= config.released_above:
        raise ValueError("pressed_below must be lower than released_above")
    if config.retry_count < 1:
        raise ValueError("retry_count must be at least one")
    return config


class SysfsAdcReader:
    def __init__(self, path: Path):
        self.path = Path(path)
        self._fd: Optional[int] = None

    def read(self) -> int:
        try:
            if self._fd is None:
                self._fd = os.open(self.path, os.O_RDONLY | os.O_CLOEXEC)
            os.lseek(self._fd, 0, os.SEEK_SET)
            payload = os.read(self._fd, 64)
            return int(payload.strip())
        except Exception:
            self.close()
            raise

    def close(self):
        if self._fd is not None:
            try:
                os.close(self._fd)
            finally:
                self._fd = None


class LongPressButton:
    def __init__(
        self,
        config: ButtonConfig,
        pressed_below: int,
        released_above: int,
        debounce_seconds: float,
        hold_seconds: float,
    ):
        self.config = config
        self.pressed_below = pressed_below
        self.released_above = released_above
        self.debounce_seconds = debounce_seconds
        self.hold_seconds = hold_seconds
        self.raw_pressed = False
        self.candidate_pressed = False
        self.candidate_since = 0.0
        self.stable_pressed = False
        self.press_started_at: Optional[float] = None
        self.triggered = False
        self.armed = False
        self.initialized = False
        self.suppressed_until_release = False

    def update(self, value: int, now: float):
        if self.raw_pressed:
            raw_pressed = value < self.released_above
        else:
            raw_pressed = value <= self.pressed_below
        self.raw_pressed = raw_pressed

        if not self.initialized:
            self.initialized = True
            self.candidate_pressed = raw_pressed
            self.candidate_since = now
            self.stable_pressed = raw_pressed
            self.press_started_at = now if raw_pressed else None
            self.armed = not raw_pressed
            return

        if raw_pressed != self.candidate_pressed:
            self.candidate_pressed = raw_pressed
            self.candidate_since = now
            return
        if self.suppressed_until_release:
            if raw_pressed or now - self.candidate_since < self.debounce_seconds:
                return
            self.suppressed_until_release = False
            self.stable_pressed = False
            self.press_started_at = None
            self.triggered = False
            self.armed = True
            return
        if raw_pressed == self.stable_pressed:
            return
        if now - self.candidate_since < self.debounce_seconds:
            return

        self.stable_pressed = raw_pressed
        if raw_pressed:
            self.press_started_at = now
            self.triggered = False
        else:
            self.press_started_at = None
            self.triggered = False
            self.armed = True

    def trigger_due(self, now: float) -> bool:
        return bool(
            self.armed
            and self.stable_pressed
            and not self.triggered
            and self.press_started_at is not None
            and now - self.press_started_at >= self.hold_seconds
        )

    def consume(self):
        self.triggered = True

    def suppress_until_release(self):
        self.triggered = True
        self.armed = False
        self.suppressed_until_release = True


class JsonHttpClient:
    def __init__(self, timeout_seconds: float):
        self.timeout_seconds = timeout_seconds

    def request(self, method: str, url: str, payload=None):
        body = None
        headers = {}
        if payload is not None:
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            headers["Content-Type"] = "application/json"
        request = Request(url, data=body, headers=headers, method=method)
        try:
            with urlopen(request, timeout=self.timeout_seconds) as response:
                response_body = response.read()
        except HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"HTTP {exc.code}: {detail}") from exc
        except (URLError, OSError, TimeoutError) as exc:
            raise RuntimeError(str(exc)) from exc
        try:
            result = json.loads(response_body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise RuntimeError("endpoint returned invalid JSON") from exc
        if not isinstance(result, dict):
            raise RuntimeError("endpoint returned a non-object JSON response")
        return result


class RecordingController:
    def __init__(
        self,
        receiver_base_url: str,
        speech_base_url: str,
        http_client: JsonHttpClient,
        retry_count: int,
        retry_delay_seconds: float,
        sleep: Callable[[float], None] = time.sleep,
    ):
        self.receiver_base_url = receiver_base_url
        self.speech_base_url = speech_base_url
        self.http_client = http_client
        self.retry_count = retry_count
        self.retry_delay_seconds = retry_delay_seconds
        self.sleep = sleep

    def perform(self, action: str, cue: str) -> str:
        command_attempted = False
        last_error = ""
        for attempt in range(1, self.retry_count + 1):
            try:
                status = self.http_client.request(
                    "GET", f"{self.receiver_base_url}/api/status"
                )
                state = str(status.get("recording_state", ""))
                recording_all = bool(status.get("recording_all", False))
                desired_state = (
                    action == "start"
                    and (recording_all or state in {"starting", "recording"})
                ) or (
                    action == "stop"
                    and not recording_all
                    and state in {"idle", "faulted"}
                )
                if desired_state:
                    if not command_attempted:
                        LOG.info(
                            "recording action ignored action=%s state=%s recording_all=%s",
                            action,
                            state,
                            recording_all,
                        )
                        return "ignored"
                    LOG.info(
                        "recording action confirmed after retry action=%s state=%s",
                        action,
                        state,
                    )
                    self._queue_cue(cue, action)
                    return "success"

                command_attempted = True
                endpoint = "start-all" if action == "start" else "stop-all"
                response = self.http_client.request(
                    "POST", f"{self.receiver_base_url}/api/record/{endpoint}"
                )
                if response.get("ok") is not True:
                    raise RuntimeError(str(response.get("error", "request rejected")))
                LOG.info(
                    "recording action accepted action=%s attempt=%d response=%s",
                    action,
                    attempt,
                    json.dumps(response, ensure_ascii=False, separators=(",", ":")),
                )
                self._queue_cue(cue, action)
                return "success"
            except Exception as exc:
                last_error = str(exc)
                LOG.warning(
                    "recording action failed action=%s attempt=%d/%d error=%s",
                    action,
                    attempt,
                    self.retry_count,
                    last_error,
                )
                if attempt < self.retry_count:
                    self.sleep(self.retry_delay_seconds)
        LOG.error("recording action abandoned action=%s error=%s", action, last_error)
        return "failed"

    def _queue_cue(self, cue: str, action: str):
        request_id = f"recording-button-{action}-{uuid.uuid4().hex}"
        last_error = ""
        for attempt in range(1, self.retry_count + 1):
            try:
                response = self.http_client.request(
                    "POST",
                    f"{self.speech_base_url}/api/audio/cue",
                    {"request_id": request_id, "cue": cue},
                )
                if response.get("accepted") is not True:
                    raise RuntimeError(str(response.get("error", "cue rejected")))
                LOG.info(
                    "recording cue accepted action=%s cue=%s duplicate=%s",
                    action,
                    cue,
                    bool(response.get("duplicate", False)),
                )
                return
            except Exception as exc:
                last_error = str(exc)
                if attempt < self.retry_count:
                    self.sleep(self.retry_delay_seconds)
        LOG.error(
            "recording succeeded but cue failed action=%s cue=%s error=%s",
            action,
            cue,
            last_error,
        )


class ActionWorker:
    def __init__(self, controller: RecordingController):
        self.controller = controller
        self._queue = queue.Queue(maxsize=1)
        self._busy = threading.Event()
        self._stop = threading.Event()
        self._thread = threading.Thread(
            target=self._run,
            name="recording-button-actions",
            daemon=True,
        )

    def start(self):
        self._thread.start()

    def submit(self, button: ButtonConfig) -> bool:
        if self._busy.is_set():
            return False
        self._busy.set()
        try:
            self._queue.put_nowait(button)
            return True
        except queue.Full:
            self._busy.clear()
            return False

    def stop(self):
        self._stop.set()
        try:
            self._queue.put_nowait(None)
        except queue.Full:
            pass
        self._thread.join(timeout=5.0)

    def _run(self):
        while not self._stop.is_set():
            try:
                button = self._queue.get(timeout=0.2)
            except queue.Empty:
                continue
            if button is None:
                break
            try:
                self.controller.perform(button.action, button.cue)
            finally:
                self._busy.clear()


class ButtonService:
    def __init__(
        self,
        config: ServiceConfig,
        controller: RecordingController,
        clock: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], None] = time.sleep,
        reader_factory=SysfsAdcReader,
    ):
        self.config = config
        self.clock = clock
        self.sleep = sleep
        self.readers: Dict[str, SysfsAdcReader] = {
            item.name: reader_factory(item.adc_path) for item in config.buttons
        }
        self.buttons = {
            item.name: LongPressButton(
                item,
                config.pressed_below,
                config.released_above,
                config.debounce_seconds,
                config.hold_seconds,
            )
            for item in config.buttons
        }
        self.worker = ActionWorker(controller)
        self.stop_event = threading.Event()
        self._last_read_error_log = {}

    def request_stop(self):
        self.stop_event.set()

    def run(self):
        self.worker.start()
        LOG.info(
            "recording button service started hold_s=%.3f poll_ms=%.1f "
            "debounce_ms=%.1f pressed_below=%d released_above=%d",
            self.config.hold_seconds,
            self.config.poll_interval_seconds * 1000.0,
            self.config.debounce_seconds * 1000.0,
            self.config.pressed_below,
            self.config.released_above,
        )
        for item in self.config.buttons:
            LOG.info(
                "button configured name=%s action=%s cue=%s adc=%s",
                item.name,
                item.action,
                item.cue,
                item.adc_path,
            )
        try:
            while not self.stop_event.is_set():
                self.poll_once()
                self.sleep(self.config.poll_interval_seconds)
        finally:
            self.worker.stop()
            for reader in self.readers.values():
                reader.close()
            LOG.info("recording button service stopped")

    def poll_once(self):
        now = self.clock()
        for name, button in self.buttons.items():
            try:
                value = self.readers[name].read()
                button.update(value, now)
            except (OSError, ValueError) as exc:
                last_log = self._last_read_error_log.get(name, 0.0)
                if now - last_log >= 5.0:
                    LOG.warning("ADC read failed button=%s error=%s", name, exc)
                    self._last_read_error_log[name] = now

        raw_pressed = [button for button in self.buttons.values() if button.raw_pressed]
        due = [
            button
            for button in self.buttons.values()
            if button.trigger_due(now)
        ]
        if not due:
            return
        if len(raw_pressed) > 1:
            for button in raw_pressed:
                button.suppress_until_release()
            LOG.warning(
                "simultaneous button hold ignored buttons=%s",
                ",".join(sorted(button.config.name for button in raw_pressed)),
            )
            return

        button = due[0]
        button.consume()
        if self.worker.submit(button.config):
            LOG.info(
                "long press accepted button=%s action=%s hold_s=%.3f",
                button.config.name,
                button.config.action,
                now - button.press_started_at,
            )
        else:
            LOG.warning(
                "long press ignored while another action is active button=%s",
                button.config.name,
            )


def configure_logging(verbose: bool):
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )


def main():
    parser = argparse.ArgumentParser(description="SARADC recording button service")
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--probe", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    configure_logging(args.verbose)
    config = load_config(args.config)

    if args.probe:
        for button in config.buttons:
            reader = SysfsAdcReader(button.adc_path)
            try:
                print(f"{button.name} {button.adc_path} value={reader.read()}")
            finally:
                reader.close()
        return 0

    http_client = JsonHttpClient(config.request_timeout_seconds)
    controller = RecordingController(
        config.receiver_base_url,
        config.speech_base_url,
        http_client,
        config.retry_count,
        config.retry_delay_seconds,
    )
    service = ButtonService(config, controller)
    signal.signal(signal.SIGINT, lambda _signum, _frame: service.request_stop())
    signal.signal(signal.SIGTERM, lambda _signum, _frame: service.request_stop())
    service.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
