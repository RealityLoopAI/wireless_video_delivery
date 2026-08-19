#!/usr/bin/env python3
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = (
    Path(__file__).resolve().parents[1]
    / "12_apps"
    / "recording_buttons"
    / "recording_led_service.py"
)
SPEC = importlib.util.spec_from_file_location("recording_led_service", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class FakeLed:
    def __init__(self):
        self.values = []

    def set(self, value):
        self.values.append(value)

    def close(self):
        pass


class RecordingLedServiceTest(unittest.TestCase):
    def config(self):
        return MODULE.LedConfig(
            sender_id="lubancat-52d2ef0c",
            receiver_base_url="http://receiver:8080",
            chip="gpiochip4",
            line_offset=19,
            active_high=True,
            blink_interval_seconds=0.5,
            status_poll_interval_seconds=0.25,
            request_timeout_seconds=1.0,
            status_stale_seconds=1.0,
        )

    def test_loads_gpio4_c3_defaults(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.json"
            path.write_text(
                json.dumps(
                    {
                        "sender_id": "lubancat-52d2ef0c",
                        "receiver_base_url": "http://192.168.66.196:8080",
                        "recording_led": {},
                    }
                )
            )
            config = MODULE.load_config(path)
        self.assertEqual(config.chip, "gpiochip4")
        self.assertEqual(config.line_offset, 19)
        self.assertTrue(config.active_high)

    def test_blinks_only_while_recording(self):
        now = [0.0]
        led = FakeLed()
        service = MODULE.RecordingLedService(
            self.config(),
            led,
            clock=lambda: now[0],
        )
        service.poll_once()
        self.assertEqual(led.values, [])

        service._reported_recording = True
        service._last_status_success = 0.25
        now[0] = 0.25
        service.poll_once()
        self.assertEqual(led.values, [True])

        now[0] = 0.75
        service.poll_once()
        self.assertEqual(led.values, [True, False])

        service._reported_recording = False
        service._last_status_success = 1.0
        now[0] = 1.0
        service.poll_once()
        self.assertEqual(led.values, [True, False])

    def test_stale_status_forces_led_off(self):
        now = [0.0]
        led = FakeLed()
        service = MODULE.RecordingLedService(
            self.config(), led, clock=lambda: now[0]
        )
        service._reported_recording = True
        service._last_status_success = 0.0
        service.poll_once()
        now[0] = 1.01
        service.poll_once()
        self.assertEqual(led.values, [True, False])


if __name__ == "__main__":
    unittest.main()
