#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace


SOURCE_ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = (
    SOURCE_ROOT
    / "12_apps"
    / "recording_buttons"
    / "power_button_service.py"
)


def load_module():
    spec = importlib.util.spec_from_file_location("gwv3_power_button", MODULE_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class ScriptedHttpClient:
    def __init__(self, responses):
        self.responses = list(responses)
        self.calls = []

    def request(self, method, url, payload=None, timeout_seconds=None):
        self.calls.append((method, url, payload, timeout_seconds))
        if not self.responses:
            raise AssertionError(f"unexpected request: {method} {url}")
        response = self.responses.pop(0)
        if isinstance(response, Exception):
            raise response
        return response


def make_config(module, marker: Path):
    return module.ServiceConfig(
        event_device=Path("/dev/input/power"),
        receiver_base_url="http://receiver",
        speech_base_url="http://speech",
        hold_seconds=5.0,
        request_timeout_seconds=2.0,
        retry_count=3,
        retry_delay_seconds=0.5,
        boot_cue="startup",
        shutdown_cue="shutdown",
        boot_cue_wait_seconds=30.0,
        shutdown_audio_failure_seconds=3.0,
        speech_status_poll_seconds=0.1,
        boot_marker_path=marker,
        poweroff_command=("/usr/bin/systemctl", "poweroff"),
    )


def test_power_key_state(module):
    state = module.PowerKeyState(5.0)
    state.update(True, 1.0)
    assert state.trigger_due(5.99) is False
    state.update(False, 5.99)
    assert state.trigger_due(10.0) is False

    state.update(True, 20.0)
    assert state.trigger_due(25.0) is True
    state.consume()
    assert state.trigger_due(40.0) is False
    state.update(False, 40.1)
    state.update(True, 41.0)
    assert state.trigger_due(46.01) is True


def test_shutdown_stops_recording_waits_for_cue_and_powers_off(module):
    with tempfile.TemporaryDirectory(prefix="gwv3_power_test_") as temporary:
        config = make_config(module, Path(temporary) / "boot-marker")
        http = ScriptedHttpClient(
            [
                {"recording_state": "recording", "recording_all": True},
                {"ok": True, "recording_all": False, "finalizing": True},
                {"accepted": True, "request_id": "shutdown-1"},
                {"state": "queued", "queue_position": 1},
                {"state": "completed", "queue_position": 0},
            ]
        )
        now = [0.0]
        commands = []

        def sleep(seconds):
            now[0] += seconds

        def run_command(command, **kwargs):
            commands.append((command, kwargs))
            return SimpleNamespace(returncode=0)

        controller = module.PowerController(
            config,
            http,
            sleep=sleep,
            monotonic=lambda: now[0],
            run_command=run_command,
        )
        assert controller.shutdown() is True
        assert [call[0] for call in http.calls] == [
            "GET",
            "POST",
            "POST",
            "GET",
            "GET",
        ]
        assert http.calls[1][1].endswith("/api/record/stop-all")
        assert http.calls[2][2]["cue"] == "shutdown"
        assert commands[0][0] == ["/usr/bin/systemctl", "poweroff"]


def test_idle_shutdown_skips_receiver_stop(module):
    with tempfile.TemporaryDirectory(prefix="gwv3_power_idle_") as temporary:
        config = make_config(module, Path(temporary) / "boot-marker")
        http = ScriptedHttpClient(
            [
                {"recording_state": "idle", "recording_all": False},
                {"accepted": True},
                {"state": "completed", "queue_position": 0},
            ]
        )
        commands = []
        controller = module.PowerController(
            config,
            http,
            run_command=lambda command, **kwargs: (
                commands.append((command, kwargs)) or SimpleNamespace(returncode=0)
            ),
        )
        assert controller.shutdown() is True
        assert not any("stop-all" in call[1] for call in http.calls)
        assert len(commands) == 1


def test_boot_cue_runs_once(module):
    with tempfile.TemporaryDirectory(prefix="gwv3_power_boot_") as temporary:
        config = make_config(module, Path(temporary) / "boot-marker")
        http = ScriptedHttpClient([{"accepted": True}])
        controller = module.PowerController(config, http)
        assert controller.queue_boot_cue_once() == "accepted"
        assert controller.queue_boot_cue_once() == "already_attempted"
        assert len(http.calls) == 1
        assert http.calls[0][2]["cue"] == "startup"


def test_audio_unavailable_still_powers_off(module):
    with tempfile.TemporaryDirectory(prefix="gwv3_power_no_audio_") as temporary:
        config = make_config(module, Path(temporary) / "boot-marker")
        now = [0.0]
        commands = []

        class AudioUnavailableClient:
            def __init__(self):
                self.cue_attempts = 0

            def request(self, method, url, payload=None, timeout_seconds=None):
                if url.endswith("/api/status"):
                    return {"recording_state": "idle", "recording_all": False}
                self.cue_attempts += 1
                now[0] += min(float(timeout_seconds or 0), 0.2)
                raise RuntimeError("speech unavailable")

        http = AudioUnavailableClient()

        def sleep(seconds):
            now[0] += seconds

        controller = module.PowerController(
            config,
            http,
            sleep=sleep,
            monotonic=lambda: now[0],
            run_command=lambda command, **kwargs: (
                commands.append((command, kwargs)) or SimpleNamespace(returncode=0)
            ),
        )
        assert controller.shutdown() is True
        assert now[0] <= config.shutdown_audio_failure_seconds + 0.001
        assert http.cue_attempts > 1
        assert len(commands) == 1


def test_queued_shutdown_cue_reaches_wait_deadline_and_powers_off(module):
    config = SimpleNamespace(**vars(make_config(module, Path("unused-marker"))))
    config.shutdown_audio_wait_seconds = 0.25
    now = [0.0]
    commands = []
    status_timeouts = []

    class QueuedClient:
        def request(self, method, url, payload=None, timeout_seconds=None):
            if url.endswith("/api/status"):
                return {"recording_state": "idle", "recording_all": False}
            if method == "POST":
                return {"accepted": True}
            status_timeouts.append((now[0], timeout_seconds))
            return {"state": "queued"}

    def sleep(seconds):
        now[0] += seconds
        assert now[0] <= config.shutdown_audio_wait_seconds + 1e-9, (
            "cue wait exceeded configured total deadline"
        )

    controller = module.PowerController(
        config,
        QueuedClient(),
        sleep=sleep,
        monotonic=lambda: now[0],
        run_command=lambda command, **kwargs: (
            commands.append(command) or SimpleNamespace(returncode=0)
        ),
    )
    assert controller.shutdown() is True
    assert commands == [["/usr/bin/systemctl", "poweroff"]]
    assert abs(now[0] - config.shutdown_audio_wait_seconds) < 1e-9
    assert status_timeouts
    assert all(
        0 < timeout <= config.shutdown_audio_wait_seconds - started + 1e-9
        for started, timeout in status_timeouts
    )


def test_cue_wait_http_timeout_respects_remaining_deadline(module):
    config = SimpleNamespace(**vars(make_config(module, Path("unused-marker"))))
    config.shutdown_audio_wait_seconds = 0.025
    now = [0.0]
    timeouts = []
    original_urlopen = module.urlopen

    def stalled_urlopen(request, timeout):
        timeouts.append(timeout)
        now[0] += timeout
        raise TimeoutError("injected status timeout")

    def sleep(seconds):
        now[0] += seconds
        assert now[0] <= config.shutdown_audio_wait_seconds + 1e-9, (
            "HTTP timeout or poll exceeded the cue deadline"
        )

    module.urlopen = stalled_urlopen
    try:
        controller = module.PowerController(
            config,
            module.JsonHttpClient(config.request_timeout_seconds),
            sleep=sleep,
            monotonic=lambda: now[0],
        )
        assert controller._wait_for_cue("timeout-test") is False
    finally:
        module.urlopen = original_urlopen
    assert timeouts == [config.shutdown_audio_wait_seconds]
    assert now[0] <= config.shutdown_audio_wait_seconds + 1e-9


def test_cue_wait_network_failure_keeps_short_deadline(module):
    config = make_config(module, Path("unused-marker"))
    now = [0.0]

    class UnavailableClient:
        def request(self, method, url, payload=None, timeout_seconds=None):
            now[0] += timeout_seconds
            raise RuntimeError("status unavailable")

    def sleep(seconds):
        now[0] += seconds

    controller = module.PowerController(
        config,
        UnavailableClient(),
        sleep=sleep,
        monotonic=lambda: now[0],
    )
    assert controller._wait_for_cue("network-test") is False
    assert abs(now[0] - config.shutdown_audio_failure_seconds) < 1e-9


def test_queued_cue_can_complete_after_network_failure_window(module):
    config = make_config(module, Path("unused-marker"))
    now = [0.0]

    class DelayedCueClient:
        def request(self, method, url, payload=None, timeout_seconds=None):
            return {"state": "completed" if now[0] >= 4.0 else "queued"}

    def sleep(seconds):
        now[0] += seconds

    controller = module.PowerController(
        config,
        DelayedCueClient(),
        sleep=sleep,
        monotonic=lambda: now[0],
    )
    assert controller._wait_for_cue("delayed-test") is True
    assert config.shutdown_audio_failure_seconds < now[0] < 5.0


def test_load_config_shutdown_audio_wait_seconds(module):
    with tempfile.TemporaryDirectory(prefix="gwv3_power_wait_config_") as temporary:
        path = Path(temporary) / "config.json"
        raw = {
            "event_device": "/dev/input/power",
            "receiver_base_url": "http://receiver",
            "speech_base_url": "http://speech",
        }
        path.write_text(json.dumps(raw), encoding="utf-8")
        assert module.load_config(path).shutdown_audio_wait_seconds == 30.0
        raw["shutdown_audio_wait_seconds"] = 7.5
        path.write_text(json.dumps(raw), encoding="utf-8")
        assert module.load_config(path).shutdown_audio_wait_seconds == 7.5
        for invalid in (0, -1, float("inf"), float("nan")):
            raw["shutdown_audio_wait_seconds"] = invalid
            path.write_text(json.dumps(raw), encoding="utf-8")
            try:
                module.load_config(path)
            except ValueError as exc:
                assert "shutdown_audio_wait_seconds" in str(exc)
            else:
                raise AssertionError("invalid cue wait deadline was accepted")


def main():
    module = load_module()
    assert module.INPUT_EVENT.size == 24
    test_power_key_state(module)
    test_shutdown_stops_recording_waits_for_cue_and_powers_off(module)
    test_idle_shutdown_skips_receiver_stop(module)
    test_boot_cue_runs_once(module)
    test_audio_unavailable_still_powers_off(module)
    test_queued_shutdown_cue_reaches_wait_deadline_and_powers_off(module)
    test_cue_wait_http_timeout_respects_remaining_deadline(module)
    test_cue_wait_network_failure_keeps_short_deadline(module)
    test_queued_cue_can_complete_after_network_failure_window(module)
    test_load_config_shutdown_audio_wait_seconds(module)
    print("power button service tests passed")


if __name__ == "__main__":
    main()
