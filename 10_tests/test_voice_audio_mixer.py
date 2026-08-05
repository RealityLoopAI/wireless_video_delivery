#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys


SOURCE_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE_ROOT / "12_apps" / "xiaohuan_voice_photo"))

from audio_mixer import alsa_card_from_device, restore_capture_mixer


def test_card_parsing():
    assert alsa_card_from_device("plughw:CARD=Device_1,DEV=0") == "Device_1"
    assert alsa_card_from_device("hw:4,0") == "4"
    assert alsa_card_from_device("default") is None


def test_mixer_restore_commands():
    calls = []

    def fake_runner(command, **kwargs):
        calls.append((command, kwargs))
        return subprocess.CompletedProcess(command, 0, "", "")

    ok, status = restore_capture_mixer(
        "plughw:CARD=Device_1,DEV=0",
        "62%",
        "off",
        runner=fake_runner,
    )
    assert ok, status
    assert [call[0] for call in calls] == [
        ["amixer", "-q", "-c", "Device_1", "sset", "Mic", "62%", "unmute"],
        ["amixer", "-q", "-c", "Device_1", "sset", "Auto Gain Control", "off"],
    ]


def main():
    test_card_parsing()
    test_mixer_restore_commands()
    print("voice audio mixer tests passed")


if __name__ == "__main__":
    main()
