#!/usr/bin/env python3
import argparse
import json
import logging
import os
import select
import signal
import struct
import subprocess
import threading
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen


LOG = logging.getLogger("power-button")
EV_KEY = 0x01
KEY_POWER = 116
INPUT_EVENT = struct.Struct("@llHHI")


@dataclass(frozen=True)
class ServiceConfig:
    event_device: Path
    receiver_base_url: str
    speech_base_url: str
    hold_seconds: float
    request_timeout_seconds: float
    retry_count: int
    retry_delay_seconds: float
    boot_cue: str
    shutdown_cue: str
    boot_cue_wait_seconds: float
    shutdown_audio_failure_seconds: float
    speech_status_poll_seconds: float
    boot_marker_path: Path
    poweroff_command: tuple


def load_config(path: Path) -> ServiceConfig:
    with path.open("r", encoding="utf-8") as source:
        raw = json.load(source)
    config = ServiceConfig(
        event_device=Path(raw["event_device"]),
        receiver_base_url=str(raw["receiver_base_url"]).rstrip("/"),
        speech_base_url=str(raw["speech_base_url"]).rstrip("/"),
        hold_seconds=float(raw.get("hold_seconds", 5.0)),
        request_timeout_seconds=float(raw.get("request_timeout_seconds", 2.0)),
        retry_count=int(raw.get("retry_count", 3)),
        retry_delay_seconds=float(raw.get("retry_delay_ms", 500)) / 1000.0,
        boot_cue=str(raw.get("boot_cue", "startup")),
        shutdown_cue=str(raw.get("shutdown_cue", "shutdown")),
        boot_cue_wait_seconds=float(raw.get("boot_cue_wait_seconds", 30.0)),
        shutdown_audio_failure_seconds=float(
            raw.get("shutdown_audio_failure_seconds", 3.0)
        ),
        speech_status_poll_seconds=float(
            raw.get("speech_status_poll_ms", 100)
        )
        / 1000.0,
        boot_marker_path=Path(
            raw.get(
                "boot_marker_path",
                "/run/gwv3-recording-buttons/boot-cue-attempted",
            )
        ),
        poweroff_command=tuple(
            str(part)
            for part in raw.get(
                "poweroff_command", ["/usr/bin/systemctl", "poweroff"]
            )
        ),
    )
    if config.hold_seconds <= 0:
        raise ValueError("hold_seconds must be positive")
    if config.request_timeout_seconds <= 0:
        raise ValueError("request_timeout_seconds must be positive")
    if config.retry_count < 1:
        raise ValueError("retry_count must be at least one")
    if config.retry_delay_seconds < 0:
        raise ValueError("retry_delay_ms cannot be negative")
    if config.boot_cue_wait_seconds < 0:
        raise ValueError("boot_cue_wait_seconds cannot be negative")
    if config.shutdown_audio_failure_seconds < 0:
        raise ValueError("shutdown_audio_failure_seconds cannot be negative")
    if config.speech_status_poll_seconds <= 0:
        raise ValueError("speech_status_poll_ms must be positive")
    if not config.poweroff_command:
        raise ValueError("poweroff_command cannot be empty")
    return config


class JsonHttpClient:
    def __init__(self, timeout_seconds: float):
        self.timeout_seconds = timeout_seconds

    def request(self, method: str, url: str, payload=None, timeout_seconds=None):
        body = None
        headers = {}
        if payload is not None:
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            headers["Content-Type"] = "application/json"
        request = Request(url, data=body, headers=headers, method=method)
        timeout = (
            self.timeout_seconds
            if timeout_seconds is None
            else max(0.05, float(timeout_seconds))
        )
        try:
            with urlopen(request, timeout=timeout) as response:
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


class PowerKeyState:
    def __init__(self, hold_seconds: float):
        self.hold_seconds = hold_seconds
        self.pressed = False
        self.armed = True
        self.triggered = False
        self.press_started_at: Optional[float] = None

    def update(self, pressed: bool, now: float):
        if pressed:
            if not self.pressed:
                self.pressed = True
                if self.armed:
                    self.press_started_at = now
            return
        self.pressed = False
        self.armed = True
        self.triggered = False
        self.press_started_at = None

    def trigger_due(self, now: float) -> bool:
        return bool(
            self.armed
            and self.pressed
            and not self.triggered
            and self.press_started_at is not None
            and now - self.press_started_at >= self.hold_seconds
        )

    def consume(self):
        self.triggered = True
        self.armed = False

    def reset_after_read_error(self):
        self.pressed = False
        self.armed = True
        self.triggered = False
        self.press_started_at = None


class EvdevPowerKeyReader:
    def __init__(self, path: Path):
        self.path = Path(path)
        self._fd: Optional[int] = None

    def open(self):
        if self._fd is None:
            self._fd = os.open(self.path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)

    def close(self):
        if self._fd is not None:
            try:
                os.close(self._fd)
            finally:
                self._fd = None

    def read_events(self, timeout_seconds: float):
        self.open()
        ready, _, _ = select.select([self._fd], [], [], max(0.0, timeout_seconds))
        if not ready:
            return []
        payload = os.read(self._fd, INPUT_EVENT.size * 32)
        if not payload:
            raise OSError("power key input returned EOF")
        events = []
        complete_size = len(payload) - (len(payload) % INPUT_EVENT.size)
        for offset in range(0, complete_size, INPUT_EVENT.size):
            _seconds, _micros, event_type, code, value = INPUT_EVENT.unpack_from(
                payload, offset
            )
            events.append((event_type, code, value))
        return events


class PowerController:
    def __init__(
        self,
        config: ServiceConfig,
        http_client: JsonHttpClient,
        sleep: Callable[[float], None] = time.sleep,
        monotonic: Callable[[], float] = time.monotonic,
        run_command: Callable = subprocess.run,
    ):
        self.config = config
        self.http_client = http_client
        self.sleep = sleep
        self.monotonic = monotonic
        self.run_command = run_command

    def queue_boot_cue_once(self) -> str:
        marker = self.config.boot_marker_path
        try:
            marker.parent.mkdir(parents=True, exist_ok=True)
            fd = os.open(marker, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
            os.close(fd)
        except FileExistsError:
            LOG.info("boot cue already attempted this boot")
            return "already_attempted"
        except OSError as exc:
            LOG.warning("cannot create boot cue marker path=%s error=%s", marker, exc)
        accepted = self._queue_cue(
            self.config.boot_cue,
            self.config.boot_cue_wait_seconds,
        )
        if accepted is None:
            LOG.warning(
                "boot cue abandoned after %.1fs cue=%s",
                self.config.boot_cue_wait_seconds,
                self.config.boot_cue,
            )
            return "failed"
        LOG.info("boot cue accepted cue=%s", self.config.boot_cue)
        return "accepted"

    def shutdown(self) -> bool:
        LOG.info("power shutdown sequence started")
        self.stop_recording_if_needed()
        request_id = self._queue_cue(
            self.config.shutdown_cue,
            self.config.shutdown_audio_failure_seconds,
        )
        if request_id is not None:
            played = self._wait_for_cue(request_id)
            LOG.info(
                "shutdown cue finished cue=%s success=%s",
                self.config.shutdown_cue,
                str(played).lower(),
            )
        else:
            LOG.warning(
                "shutdown cue unavailable after %.1fs; continuing poweroff",
                self.config.shutdown_audio_failure_seconds,
            )
        try:
            result = self.run_command(
                list(self.config.poweroff_command),
                check=False,
                timeout=5.0,
            )
            return_code = int(getattr(result, "returncode", 0))
        except Exception as exc:
            LOG.error("poweroff command failed error=%s", exc)
            return False
        if return_code != 0:
            LOG.error("poweroff command exited return_code=%d", return_code)
            return False
        LOG.info("poweroff command accepted")
        return True

    def stop_recording_if_needed(self) -> str:
        last_error = ""
        for attempt in range(1, self.config.retry_count + 1):
            try:
                status = self.http_client.request(
                    "GET", f"{self.config.receiver_base_url}/api/status"
                )
                state = str(status.get("recording_state", ""))
                recording_all = bool(status.get("recording_all", False))
                if not recording_all and state in {"idle", "faulted"}:
                    LOG.info("recording already stopped before poweroff state=%s", state)
                    return "idle"
                response = self.http_client.request(
                    "POST",
                    f"{self.config.receiver_base_url}/api/record/stop-all",
                )
                if response.get("ok") is not True:
                    raise RuntimeError(
                        str(response.get("error", "stop-all request rejected"))
                    )
                LOG.info(
                    "recording stop accepted before poweroff attempt=%d response=%s",
                    attempt,
                    json.dumps(response, ensure_ascii=False, separators=(",", ":")),
                )
                return "stopped"
            except Exception as exc:
                last_error = str(exc)
                LOG.warning(
                    "recording stop failed before poweroff attempt=%d/%d error=%s",
                    attempt,
                    self.config.retry_count,
                    last_error,
                )
                if attempt < self.config.retry_count:
                    self.sleep(self.config.retry_delay_seconds)
        LOG.error("recording stop abandoned before poweroff error=%s", last_error)
        return "failed"

    def _queue_cue(self, cue: str, availability_seconds: float):
        request_id = f"power-{cue}-{uuid.uuid4().hex}"
        deadline = self.monotonic() + availability_seconds
        last_error = ""
        while True:
            remaining = deadline - self.monotonic()
            if remaining <= 0:
                LOG.warning("cue submission failed cue=%s error=%s", cue, last_error)
                return None
            try:
                response = self.http_client.request(
                    "POST",
                    f"{self.config.speech_base_url}/api/audio/cue",
                    {"request_id": request_id, "cue": cue},
                    timeout_seconds=min(
                        self.config.request_timeout_seconds,
                        remaining,
                    ),
                )
                if response.get("accepted") is not True:
                    raise RuntimeError(str(response.get("error", "cue rejected")))
                return request_id
            except Exception as exc:
                last_error = str(exc)
                remaining = deadline - self.monotonic()
                if remaining <= 0:
                    LOG.warning("cue submission failed cue=%s error=%s", cue, last_error)
                    return None
                self.sleep(min(0.2, remaining))

    def _wait_for_cue(self, request_id: str) -> bool:
        unavailable_since = None
        encoded_id = quote(request_id, safe="")
        url = (
            f"{self.config.speech_base_url}/api/audio/status"
            f"?request_id={encoded_id}"
        )
        while True:
            attempt_started = self.monotonic()
            if unavailable_since is None:
                request_timeout = self.config.request_timeout_seconds
            else:
                remaining = (
                    self.config.shutdown_audio_failure_seconds
                    - (attempt_started - unavailable_since)
                )
                if remaining <= 0:
                    return False
                request_timeout = min(
                    self.config.request_timeout_seconds,
                    remaining,
                )
            try:
                status = self.http_client.request(
                    "GET",
                    url,
                    timeout_seconds=request_timeout,
                )
                unavailable_since = None
                state = str(status.get("state", ""))
                if state == "completed":
                    return True
                if state == "failed":
                    return False
                if state != "queued":
                    raise RuntimeError(f"unexpected cue state: {state}")
            except Exception as exc:
                now = self.monotonic()
                if unavailable_since is None:
                    unavailable_since = attempt_started
                    LOG.warning(
                        "cue status temporarily unavailable request_id=%s error=%s",
                        request_id,
                        exc,
                    )
                elif (
                    now - unavailable_since
                    >= self.config.shutdown_audio_failure_seconds
                ):
                    return False
            self.sleep(self.config.speech_status_poll_seconds)


class PowerButtonService:
    def __init__(
        self,
        config: ServiceConfig,
        controller: PowerController,
        reader: Optional[EvdevPowerKeyReader] = None,
        monotonic: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], None] = time.sleep,
    ):
        self.config = config
        self.controller = controller
        self.reader = reader or EvdevPowerKeyReader(config.event_device)
        self.monotonic = monotonic
        self.sleep = sleep
        self.state = PowerKeyState(config.hold_seconds)
        self.stop_event = threading.Event()
        self._last_read_error_log = 0.0

    def request_stop(self):
        self.stop_event.set()

    def run(self):
        boot_thread = threading.Thread(
            target=self.controller.queue_boot_cue_once,
            name="power-boot-cue",
            daemon=True,
        )
        boot_thread.start()
        LOG.info(
            "power button service started event=%s hold_s=%.3f key_code=%d",
            self.config.event_device,
            self.config.hold_seconds,
            KEY_POWER,
        )
        try:
            while not self.stop_event.is_set():
                try:
                    events = self.reader.read_events(0.1)
                    for event_type, code, value in events:
                        if event_type != EV_KEY or code != KEY_POWER:
                            continue
                        if value == 1:
                            self.state.update(True, self.monotonic())
                            LOG.info("power key pressed")
                        elif value == 0:
                            elapsed = 0.0
                            if self.state.press_started_at is not None:
                                elapsed = self.monotonic() - self.state.press_started_at
                            self.state.update(False, self.monotonic())
                            LOG.info("power key released hold_s=%.3f", elapsed)
                    now = self.monotonic()
                    if self.state.trigger_due(now):
                        self.state.consume()
                        LOG.info(
                            "power key long press accepted hold_s=%.3f",
                            now - self.state.press_started_at,
                        )
                        self.controller.shutdown()
                except OSError as exc:
                    now = self.monotonic()
                    if now - self._last_read_error_log >= 5.0:
                        LOG.warning("power key read failed error=%s", exc)
                        self._last_read_error_log = now
                    self.reader.close()
                    self.state.reset_after_read_error()
                    self.sleep(0.5)
        finally:
            self.reader.close()
            LOG.info("power button service stopped")


def configure_logging(verbose: bool):
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )


def main():
    parser = argparse.ArgumentParser(description="RK805 power button service")
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--probe", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    configure_logging(args.verbose)
    config = load_config(args.config)
    if args.probe:
        reader = EvdevPowerKeyReader(config.event_device)
        try:
            reader.open()
            print(
                f"event_device={config.event_device} event_size={INPUT_EVENT.size} "
                f"key_code={KEY_POWER} hold_seconds={config.hold_seconds}"
            )
        finally:
            reader.close()
        return 0

    controller = PowerController(
        config,
        JsonHttpClient(config.request_timeout_seconds),
    )
    service = PowerButtonService(config, controller)
    signal.signal(signal.SIGINT, lambda _signum, _frame: service.request_stop())
    signal.signal(signal.SIGTERM, lambda _signum, _frame: service.request_stop())
    service.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
