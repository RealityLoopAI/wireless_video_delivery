#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import sys


SOURCE_ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = (
    SOURCE_ROOT
    / "12_apps"
    / "recording_buttons"
    / "recording_button_service.py"
)


def load_module():
    spec = importlib.util.spec_from_file_location("gwv3_recording_buttons", MODULE_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class ScriptedHttpClient:
    def __init__(self, responses):
        self.responses = list(responses)
        self.calls = []

    def request(self, method, url, payload=None):
        self.calls.append((method, url, payload))
        if not self.responses:
            raise AssertionError(f"unexpected request: {method} {url}")
        response = self.responses.pop(0)
        if isinstance(response, Exception):
            raise response
        return response


class FakeReader:
    def __init__(self, value=4095):
        self.value = value

    def read(self):
        return self.value

    def close(self):
        pass


class FakeWorker:
    def __init__(self):
        self.submitted = []

    def submit(self, button):
        self.submitted.append(button)
        return True


def make_button(module, action="start"):
    return module.LongPressButton(
        module.ButtonConfig(
            name=action,
            action=action,
            cue="ding" if action == "start" else "deng",
            adc_path=Path(f"/tmp/{action}"),
        ),
        pressed_below=800,
        released_above=2000,
        debounce_seconds=0.06,
        hold_seconds=2.0,
    )


def test_long_press_state_machine(module):
    button = make_button(module)
    button.update(4095, 0.0)
    assert button.armed is True

    button.update(0, 0.10)
    button.update(0, 0.17)
    assert button.stable_pressed is True
    assert button.trigger_due(2.16) is False
    assert button.trigger_due(2.18) is True
    button.consume()
    assert button.trigger_due(10.0) is False

    button.update(4095, 2.20)
    button.update(4095, 2.27)
    assert button.stable_pressed is False
    button.update(0, 3.00)
    button.update(0, 3.07)
    assert button.trigger_due(5.08) is True


def test_held_during_startup_requires_release(module):
    button = make_button(module)
    button.update(0, 0.0)
    assert button.stable_pressed is True
    assert button.armed is False
    assert button.trigger_due(10.0) is False

    button.update(4095, 10.1)
    button.update(4095, 10.2)
    assert button.armed is True
    button.update(0, 11.0)
    button.update(0, 11.1)
    assert button.trigger_due(13.11) is True


def test_controller_success_duplicate_and_retry_confirmation(module):
    http = ScriptedHttpClient(
        [
            {"recording_state": "idle", "recording_all": False},
            {"ok": True, "recording_all": True},
            {"accepted": True, "duplicate": False},
        ]
    )
    controller = module.RecordingController(
        "http://receiver",
        "http://speech",
        http,
        retry_count=3,
        retry_delay_seconds=0.0,
        sleep=lambda _seconds: None,
    )
    assert controller.perform("start", "ding") == "success"
    assert [call[0] for call in http.calls] == ["GET", "POST", "POST"]
    assert http.calls[-1][2]["cue"] == "ding"

    duplicate_http = ScriptedHttpClient(
        [{"recording_state": "recording", "recording_all": True}]
    )
    duplicate_controller = module.RecordingController(
        "http://receiver",
        "http://speech",
        duplicate_http,
        retry_count=3,
        retry_delay_seconds=0.0,
        sleep=lambda _seconds: None,
    )
    assert duplicate_controller.perform("start", "ding") == "ignored"
    assert len(duplicate_http.calls) == 1

    retry_http = ScriptedHttpClient(
        [
            {"recording_state": "idle", "recording_all": False},
            RuntimeError("response lost"),
            {"recording_state": "recording", "recording_all": True},
            {"accepted": True, "duplicate": False},
        ]
    )
    retry_controller = module.RecordingController(
        "http://receiver",
        "http://speech",
        retry_http,
        retry_count=3,
        retry_delay_seconds=0.0,
        sleep=lambda _seconds: None,
    )
    assert retry_controller.perform("start", "ding") == "success"
    assert len(retry_http.calls) == 4


def test_simultaneous_hold_is_ignored(module):
    now = [0.0]
    start_path = Path("/tmp/start-adc")
    stop_path = Path("/tmp/stop-adc")
    readers = {
        start_path: FakeReader(),
        stop_path: FakeReader(),
    }
    config = module.ServiceConfig(
        receiver_base_url="http://receiver",
        speech_base_url="http://speech",
        hold_seconds=2.0,
        poll_interval_seconds=0.02,
        debounce_seconds=0.06,
        pressed_below=800,
        released_above=2000,
        request_timeout_seconds=1.0,
        retry_count=3,
        retry_delay_seconds=0.5,
        buttons=(
            module.ButtonConfig("recovery", "start", "ding", start_path),
            module.ButtonConfig("maskrom", "stop", "deng", stop_path),
        ),
    )
    controller = module.RecordingController(
        "http://receiver",
        "http://speech",
        ScriptedHttpClient([]),
        retry_count=1,
        retry_delay_seconds=0.0,
    )
    service = module.ButtonService(
        config,
        controller,
        clock=lambda: now[0],
        sleep=lambda _seconds: None,
        reader_factory=lambda path: readers[path],
    )
    worker = FakeWorker()
    service.worker = worker

    service.poll_once()
    readers[start_path].value = 0
    now[0] = 0.10
    service.poll_once()
    now[0] = 0.17
    service.poll_once()
    readers[stop_path].value = 0
    now[0] = 2.18
    service.poll_once()
    assert worker.submitted == []
    assert service.buttons["recovery"].armed is False
    assert service.buttons["maskrom"].armed is False

    readers[start_path].value = 4095
    readers[stop_path].value = 4095
    now[0] = 2.30
    service.poll_once()
    now[0] = 2.37
    service.poll_once()
    assert service.buttons["recovery"].armed is True
    assert service.buttons["maskrom"].armed is True

    readers[start_path].value = 0
    now[0] = 3.00
    service.poll_once()
    now[0] = 3.07
    service.poll_once()
    now[0] = 5.08
    service.poll_once()
    assert [button.action for button in worker.submitted] == ["start"]


def main():
    module = load_module()
    test_long_press_state_machine(module)
    test_held_during_startup_requires_release(module)
    test_controller_success_duplicate_and_retry_confirmation(module)
    test_simultaneous_hold_is_ignored(module)
    print("recording button service tests passed")


if __name__ == "__main__":
    main()
