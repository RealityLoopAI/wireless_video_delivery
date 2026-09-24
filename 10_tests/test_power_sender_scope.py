import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "power_sender_scope", ROOT / "12_apps/recording_buttons/power_button_service.py"
)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class Http:
    def __init__(self, cameras, stale=False):
        self.cameras = cameras
        self.stale = stale
        self.calls = []

    def request(self, method, url, payload=None, timeout_seconds=None):
        self.calls.append((method, url))
        if method == "GET":
            return {
                "recording_state": "recording",
                "recording_all": True,
                "receiver_admin_stale": self.stale,
                "cameras": self.cameras,
            }
        return {"ok": True}


class SenderScopeTests(unittest.TestCase):
    def controller(self, cameras, stale=False):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "config.json"
        path.write_text(json.dumps({
            "sender_id": "lubancat-0282f88a",
            "event_device": "/dev/input/test-only",
            "receiver_base_url": "http://192.168.1.196:8080",
            "speech_base_url": "http://127.0.0.1:18082",
            "retry_count": 1,
        }), encoding="utf-8")
        http = Http(cameras, stale)
        return MODULE.PowerController(MODULE.load_config(path), http), http

    def test_stops_only_configured_sender(self):
        controller, http = self.controller([
            {"sender_id": "lubancat-0282f88a", "recording": True},
            {"sender_id": "other-device", "recording": True},
        ])
        self.assertEqual(controller.stop_recording_if_needed(), "stopped")
        self.assertEqual([url for method, url in http.calls if method == "POST"], [
            "http://192.168.1.196:8080/api/record/stop-sender?sender_id=lubancat-0282f88a"
        ])

    def test_other_sender_recording_does_not_trigger_stop(self):
        controller, http = self.controller([
            {"sender_id": "lubancat-0282f88a", "recording": False},
            {"sender_id": "other-device", "recording": True},
        ])
        self.assertEqual(controller.stop_recording_if_needed(), "idle")
        self.assertFalse(any(method == "POST" for method, _ in http.calls))

    def test_missing_sender_never_falls_back_to_global_stop(self):
        controller, http = self.controller([
            {"sender_id": "other-device", "recording": True},
        ])
        self.assertEqual(controller.stop_recording_if_needed(), "failed")
        self.assertFalse(any(method == "POST" for method, _ in http.calls))

    def test_stale_receiver_status_does_not_trigger_stop(self):
        controller, http = self.controller([
            {"sender_id": "lubancat-0282f88a", "recording": True},
        ], stale=True)
        self.assertEqual(controller.stop_recording_if_needed(), "failed")
        self.assertFalse(any(method == "POST" for method, _ in http.calls))


if __name__ == "__main__":
    unittest.main()
