#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import threading
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

    def fake_read(result_path):
        return True, {"status": "captured", "image_path": f"/nas/{result_path.stem}.jpg"}

    module.queue_photo_request = tracked_queue
    original_read = module.read_photo_result
    module.read_photo_result = fake_read
    ok, result = module.capture_photo_burst(args)
    module.queue_photo_request = original_queue
    module.read_photo_result = original_read
    assert ok is True
    assert result["captured_count"] == 3
    assert len(result["image_paths"]) == 3
    assert [item["burst_index"] for item in request_payloads] == [1, 2, 3]
    assert all(item["burst_count"] == 3 for item in request_payloads)
    assert len({item["burst_id"] for item in request_payloads}) == 1
    request_group_ids = [item["request_id"].rsplit("_", 1)[0] for item in request_payloads]
    assert len(set(request_group_ids)) == 1
    assert request_times[-1] - request_times[0] < 0.04
    capture_schedule = [item["capture_not_before_unix_us"] for item in request_payloads]
    assert capture_schedule[1] - capture_schedule[0] == 50_000
    assert capture_schedule[2] - capture_schedule[1] == 50_000


def test_partial_failure(module, root: Path):
    args = make_args(root, count=3, interval=0.0)
    attempts = 0

    def fake_read(_result_path):
        nonlocal attempts
        attempts += 1
        if attempts == 1:
            return True, {"status": "captured", "image_path": "/nas/first.jpg"}
        return False, {"status": "error", "error": "injected failure"}

    original_read = module.read_photo_result
    module.read_photo_result = fake_read
    ok, result = module.capture_photo_burst(args)
    module.read_photo_result = original_read
    assert ok is False
    assert attempts == 3
    assert result["captured_count"] == 1
    assert result["requested_count"] == 3
    assert result["error"].count("injected failure") == 2


def test_photo_text_matching(module, defaults):
    aliases = [module.normalize_text(item) for item in defaults.photo_alias]
    accepted = [
        "拍照",
        "拍 照",
        "拍一下",
        "帮我拍照",
        "拍 [unk]",
        "[unk] 照",
        "牌照",
        "拍 早",
        "排 造",
        "派 澡",
        "拍我[unk]",
        "帮我排早",
        "请派造",
    ]
    for text in accepted:
        assert module.is_photo_text(text, aliases), text
    for first in module.PHOTO_PAI_SYLLABLES:
        for second in module.PHOTO_ZAO_SYLLABLES:
            assert module.is_photo_text(f"{first} {second}", aliases)
    for text in ["", "[unk]", "播放音乐", "你好小环", "早上好", "排队"]:
        assert not module.is_photo_text(text, aliases), text

    grammar = module.make_photo_grammar(defaults)
    for phrase in [
        "拍 一 张 照片",
        "帮 我 拍照",
        "请 拍照",
        "牌照",
        "拍 早",
        "排 造",
        "派 澡",
        "[unk]",
    ]:
        assert phrase in grammar
    for first in module.PHOTO_PAI_SYLLABLES:
        for second in module.PHOTO_ZAO_SYLLABLES:
            assert f"{first} {second}" in grammar


def test_async_capture_does_not_block(module):
    started = threading.Event()
    release = threading.Event()
    state_lock = threading.Lock()
    calls = 0
    active = 0
    max_active = 0
    original_capture = module.capture_photo_burst

    def blocking_capture(_args):
        nonlocal calls, active, max_active
        with state_lock:
            calls += 1
            active += 1
            max_active = max(max_active, active)
        started.set()
        assert release.wait(timeout=1.0)
        with state_lock:
            active -= 1
        return True, {"image_paths": ["/nas/test.jpg"]}

    module.capture_photo_burst = blocking_capture
    begin = time.monotonic()
    first_worker = module.start_photo_capture_async(
        types.SimpleNamespace(),
        "拍照",
        "unit-test",
    )
    second_worker = module.start_photo_capture_async(
        types.SimpleNamespace(),
        "拍照",
        "unit-test",
    )
    elapsed = time.monotonic() - begin
    try:
        assert elapsed < 0.1
        assert started.wait(timeout=0.5)
        time.sleep(0.05)
        assert calls == 1
        assert first_worker.is_alive()
        assert second_worker.is_alive()
    finally:
        release.set()
        first_worker.join(timeout=1.0)
        second_worker.join(timeout=1.0)
        module.capture_photo_burst = original_capture
    assert not first_worker.is_alive()
    assert not second_worker.is_alive()
    assert calls == 2
    assert max_active == 1


def main():
    source_root = Path(__file__).resolve().parents[1]
    module = load_voice_module(source_root)
    defaults = module.build_parser().parse_args(["listen"])
    assert defaults.photo_burst_count == 3
    assert defaults.photo_burst_interval_seconds == 0.2
    assert defaults.photo_result_timeout_seconds == 30.0
    assert defaults.photo_decode_min_seconds == 0.45
    assert defaults.photo_decode_min_rms == 0.016
    assert defaults.photo_end_silence_seconds == 0.25
    assert defaults.barge_in is False
    test_photo_text_matching(module, defaults)
    test_async_capture_does_not_block(module)
    with tempfile.TemporaryDirectory(prefix="gwv3_voice_burst_") as temporary:
        root = Path(temporary)
        test_success(module, root / "success")
        test_partial_failure(module, root / "failure")
    print("voice photo burst test passed")


if __name__ == "__main__":
    main()
