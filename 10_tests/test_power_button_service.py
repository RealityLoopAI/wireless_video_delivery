#!/usr/bin/env python3
import importlib.util
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


def main():
    module = load_module()
    assert module.INPUT_EVENT.size == 24
    test_power_key_state(module)
    test_shutdown_stops_recording_waits_for_cue_and_powers_off(module)
    test_idle_shutdown_skips_receiver_stop(module)
    test_boot_cue_runs_once(module)
    test_audio_unavailable_still_powers_off(module)
    print("power button service tests passed")


if __name__ == "__main__":
    main()
