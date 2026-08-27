#!/usr/bin/env python3
import importlib.util
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = (
    ROOT
    / "12_apps"
    / "xiaohuan_voice_photo"
    / "audio_capture_recovery.py"
)


def load_module():
    spec = importlib.util.spec_from_file_location(
        "audio_capture_recovery",
        MODULE_PATH,
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main():
    module = load_module()
    current = [10.0]
    guard = module.CaptureRebuildGuard(2.0, clock=lambda: current[0])

    guard.mark_local_packet()
    current[0] = 10.5
    healthy = guard.request_rebuild("request-1")
    assert not healthy.accepted
    assert healthy.reason == "local_capture_healthy"

    current[0] = 13.0
    stale = guard.request_rebuild("request-2")
    assert stale.accepted
    assert stale.reason == "local_capture_stale"
    assert guard.begin_rebuild() == "request-2"
    assert guard.begin_rebuild() is None
    guard.complete_rebuild()

    duplicate = guard.request_rebuild("request-2")
    assert not duplicate.accepted
    assert duplicate.reason == "duplicate_request"

    empty_guard = module.CaptureRebuildGuard(2.0, clock=lambda: current[0])
    no_packets = empty_guard.request_rebuild("request-3")
    assert no_packets.accepted
    assert no_packets.local_packet_age_seconds is None

    pending = empty_guard.request_rebuild("request-4")
    assert not pending.accepted
    assert pending.reason == "rebuild_already_pending"
    print("audio capture rebuild guard test passed")


if __name__ == "__main__":
    main()
