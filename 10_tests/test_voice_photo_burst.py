#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import time
import types


def load_voice_module(source_root: Path):
    vosk_stub = types.ModuleType("vosk")
    vosk_stub.KaldiRecognizer = object
    vosk_stub.Model = object
    vosk_stub.SetLogLevel = lambda _level: None
    sys.modules["vosk"] = vosk_stub
    module_path = source_root / "12_apps" / "xiaohuan_voice_photo" / "vosk_wake.py"
    spec = importlib.util.spec_from_file_location("gwv3_vosk_wake_test", module_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_args(root: Path, count: int, interval: float):
    return types.SimpleNamespace(
        photo_request_dir=root / "requests",
        photo_result_dir=root / "results",
        photo_sender_id="sender-test",
        photo_camera_id="cam01",
        photo_result_timeout_seconds=1.0,
        photo_burst_count=count,
        photo_burst_interval_seconds=interval,
    )


def test_success(module, root: Path):
    args = make_args(root, count=3, interval=0.05)
    request_times = []
    request_payloads = []
    original_queue = module.queue_photo_request

    def tracked_queue(*queue_args, **queue_kwargs):
        request_times.append(time.monotonic())
        request_path, result_path = original_queue(*queue_args, **queue_kwargs)
        request_payloads.append(
            json.loads(request_path.read_text(encoding="utf-8"))
        )
        return request_path, result_path

    def fake_wait(result_path, _timeout):
        return True, {"status": "captured", "image_path": f"/nas/{result_path.stem}.jpg"}

    module.queue_photo_request = tracked_queue
    module.wait_photo_result = fake_wait
    ok, result = module.capture_photo_burst(args)
    module.queue_photo_request = original_queue
    assert ok is True
    assert result["captured_count"] == 3
    assert len(result["image_paths"]) == 3
    assert [item["burst_index"] for item in request_payloads] == [1, 2, 3]
    assert all(item["burst_count"] == 3 for item in request_payloads)
    assert request_times[-1] - request_times[0] < 0.04
    capture_schedule = [item["capture_not_before_unix_us"] for item in request_payloads]
    assert capture_schedule[1] - capture_schedule[0] == 50_000
    assert capture_schedule[2] - capture_schedule[1] == 50_000


def test_partial_failure(module, root: Path):
    args = make_args(root, count=3, interval=0.0)
    attempts = 0

    def fake_wait(_result_path, _timeout):
        nonlocal attempts
        attempts += 1
        if attempts == 1:
            return True, {"status": "captured", "image_path": "/nas/first.jpg"}
        return False, {"status": "error", "error": "injected failure"}

    module.wait_photo_result = fake_wait
    ok, result = module.capture_photo_burst(args)
    assert ok is False
    assert attempts == 3
    assert result["captured_count"] == 1
    assert result["requested_count"] == 3
    assert result["error"].count("injected failure") == 2


def main():
    source_root = Path(__file__).resolve().parents[1]
    module = load_voice_module(source_root)
    defaults = module.build_parser().parse_args(["listen"])
    assert defaults.photo_burst_count == 3
    assert defaults.photo_burst_interval_seconds == 0.2
    with tempfile.TemporaryDirectory(prefix="gwv3_voice_burst_") as temporary:
        root = Path(temporary)
        test_success(module, root / "success")
        test_partial_failure(module, root / "failure")
    print("voice photo burst test passed")


if __name__ == "__main__":
    main()
