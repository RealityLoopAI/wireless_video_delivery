#!/usr/bin/env python3
import math
from pathlib import Path
import struct
import wave


SAMPLE_RATE = 24000
ROOT = Path(__file__).resolve().parent


def render(path: Path, duration: float, components):
    frame_count = int(SAMPLE_RATE * duration)
    frames = bytearray()
    for index in range(frame_count):
        t = index / SAMPLE_RATE
        attack = min(1.0, t / 0.006)
        release = min(1.0, max(0.0, duration - t) / 0.035)
        value = 0.0
        for frequency, amplitude, decay, phase in components:
            value += (
                amplitude
                * math.sin((2.0 * math.pi * frequency * t) + phase)
                * math.exp(-decay * t)
            )
        sample = max(-1.0, min(1.0, value * attack * release))
        frames.extend(struct.pack("<h", round(sample * 26000)))

    with wave.open(str(path), "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(SAMPLE_RATE)
        audio.writeframes(frames)


def main():
    render(
        ROOT / "cue_photo_ding.wav",
        0.26,
        [
            (1046.50, 0.60, 7.0, 0.0),
            (1567.98, 0.25, 10.0, 0.2),
            (2093.00, 0.10, 14.0, 0.4),
        ],
    )
    render(
        ROOT / "cue_forward_deng.wav",
        0.30,
        [
            (523.25, 0.62, 8.0, 0.0),
            (783.99, 0.20, 11.0, 0.3),
            (1046.50, 0.08, 15.0, 0.5),
        ],
    )


if __name__ == "__main__":
    main()
