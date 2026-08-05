#!/usr/bin/env python3
import subprocess
from typing import Callable, Optional, Sequence, Tuple


def alsa_card_from_device(device: str) -> Optional[str]:
    if not device or ":" not in device:
        return None
    spec = device.split(":", 1)[1]
    for field in spec.split(","):
        field = field.strip()
        if field.startswith("CARD="):
            return field[5:] or None
    first = spec.split(",", 1)[0].strip()
    return first if first and "=" not in first else None


def restore_capture_mixer(
    record_device: str,
    mic_level: str,
    agc: str,
    *,
    runner: Callable[..., subprocess.CompletedProcess] = subprocess.run,
) -> Tuple[bool, str]:
    card = alsa_card_from_device(record_device)
    if not card:
        return False, f"cannot resolve ALSA card from {record_device}"

    commands: Sequence[Sequence[str]] = tuple(
        command
        for command in (
            ("amixer", "-q", "-c", card, "sset", "Mic", mic_level, "unmute")
            if mic_level
            else (),
            ("amixer", "-q", "-c", card, "sset", "Auto Gain Control", agc)
            if agc
            else (),
        )
        if command
    )
    if not commands:
        return True, f"card={card} unchanged"

    for command in commands:
        try:
            result = runner(
                list(command),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=2.0,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            return False, f"card={card} command={' '.join(command)} error={exc}"
        if result.returncode != 0:
            error = (result.stderr or result.stdout or "unknown amixer error").strip()
            return False, f"card={card} command={' '.join(command)} error={error}"

    return True, f"card={card} mic_level={mic_level or 'unchanged'} agc={agc or 'unchanged'}"
