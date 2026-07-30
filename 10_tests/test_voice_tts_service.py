#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen


def load_speech_module(source_root: Path):
    module_path = source_root / "12_apps" / "xiaohuan_voice_photo" / "speech_service.py"
    sys.path.insert(0, str(module_path.parent))
    spec = importlib.util.spec_from_file_location("gwv3_speech_service_test", module_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class FakeTtsEngine:
    def __init__(self, module):
        self.module = module
        self.ready = False
        self.load_error = ""
        self.generated = []

    def load(self):
        self.ready = True

    def synthesize(self, text):
        self.generated.append(text)
        return self.module.PcmSegment(data=b"\x00\x00" * 80, sample_rate=16000)


class FakeEdgeModule:
    def __init__(self, fail=False):
        self.fail = fail
        self.calls = []

    def Communicate(self, **kwargs):
        self.calls.append(kwargs)
        fail = self.fail

        class FakeCommunicate:
            async def stream(self):
                if fail:
                    raise RuntimeError("injected Edge failure")
                yield {"type": "audio", "data": b"fake-mp3"}

        return FakeCommunicate()


def request_json(port: int, path: str, payload=None):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = Request(
        f"http://127.0.0.1:{port}{path}",
        data=data,
        method="GET" if payload is None else "POST",
        headers={"Content-Type": "application/json"},
    )
    try:
        with urlopen(request, timeout=2.0) as response:
            return response.status, json.loads(response.read().decode("utf-8"))
    except HTTPError as exc:
        return exc.code, json.loads(exc.read().decode("utf-8"))


def wait_ready_task(service, timeout=1.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        task = service.next_ready_task(timeout=0.05)
        if task is not None:
            return task
    raise AssertionError("speech task did not become ready")


def test_sentence_splitting(module):
    text = (
        "第一句话用于测试。第二句话 includes camera 01 and 30 FPS!"
        + "很长的一段文字，" * 30
    )
    segments = module.split_tts_text(text, max_segment_chars=40)
    assert "".join(segments).replace(" ", "") == text.replace(" ", "")
    assert all(0 < len(segment) <= 40 for segment in segments)
    assert len(segments) > 3


def test_espeak_mixed_language_markup(module):
    ssml = module.make_espeak_ssml(
        "设备 camera 01 已连接，当前帧率 30 FPS，值小于 < 5。"
    )
    assert '<voice name="en-us">camera</voice>' in ssml
    assert '<voice name="en-us">FPS</voice>' in ssml
    assert "&lt; 5" in ssml
    assert ssml.startswith("<speak>")
    assert ssml.endswith("</speak>")


def test_edge_tts_cache_and_fallback(module):
    edge_module = FakeEdgeModule()
    fallback = FakeTtsEngine(module)
    engine = module.EdgeTtsEngine(
        fallback_engine=fallback,
        voice="zh-CN-XiaoyiNeural",
        edge_module=edge_module,
        decoder_executable="/bin/true",
    )
    engine.load()
    engine._decode_mp3 = lambda data: module.PcmSegment(
        data=data + b"\x00\x00",
        sample_rate=24000,
    )

    first = engine.synthesize("缓存测试")
    second = engine.synthesize("缓存测试")
    assert first == second
    assert len(edge_module.calls) == 1
    assert edge_module.calls[0]["voice"] == "zh-CN-XiaoyiNeural"
    assert fallback.generated == []

    failing_module = FakeEdgeModule(fail=True)
    fallback_after_failure = FakeTtsEngine(module)
    failing_engine = module.EdgeTtsEngine(
        fallback_engine=fallback_after_failure,
        edge_module=failing_module,
        decoder_executable="/bin/true",
    )
    failing_engine.load()
    result = failing_engine.synthesize("故障回退")
    second_result = failing_engine.synthesize("退避期直接回退")
    assert result.sample_rate == 16000
    assert second_result.sample_rate == 16000
    assert len(failing_module.calls) == 1
    assert fallback_after_failure.generated == ["故障回退", "退避期直接回退"]


def test_http_queue_and_idempotency(module):
    engine = FakeTtsEngine(module)
    service = module.UnifiedSpeechService(
        tts_engine=engine,
        http_bind="127.0.0.1",
        http_port=0,
        max_queue=2,
        max_text_chars=500,
    )
    service.start(enable_http=True)
    service._play_segment_once = lambda _segment, _device: (0, "")
    try:
        status, health = request_json(service.http_port, "/healthz")
        assert status == 200
        assert health["tts_ready"] is True
        assert health["queue_capacity"] == 2

        status, invalid = request_json(
            service.http_port,
            "/api/tts/speak",
            {"request_id": "", "text": "测试"},
        )
        assert status == 400
        assert invalid["accepted"] is False

        status, first = request_json(
            service.http_port,
            "/api/tts/speak",
            {"request_id": "request-1", "text": "第一条。Second sentence."},
        )
        assert status == 202
        assert first["accepted"] is True
        assert first["duplicate"] is False
        assert first["queue_position"] == 1

        status, duplicate = request_json(
            service.http_port,
            "/api/tts/speak",
            {"request_id": "request-1", "text": "内容不同也不能重复入队"},
        )
        assert status == 202
        assert duplicate["accepted"] is True
        assert duplicate["duplicate"] is True
        assert duplicate["task_id"] == first["task_id"]

        status, second = request_json(
            service.http_port,
            "/api/tts/speak",
            {"request_id": "request-2", "text": "设备 camera 01 已连接，帧率 30 FPS。"},
        )
        assert status == 202
        assert second["queue_position"] == 2

        status, full = request_json(
            service.http_port,
            "/api/tts/speak",
            {"request_id": "request-3", "text": "队列已满"},
        )
        assert status == 429
        assert full == {"accepted": False, "error": "queue_full"}

        first_task = wait_ready_task(service)
        assert first_task.task_id == first["task_id"]
        assert service.play_task(first_task, "test-device") is True
        service.complete_task(first_task, True)

        second_task = wait_ready_task(service)
        assert second_task.task_id == second["task_id"]
        assert service.play_task(second_task, "test-device") is True
        service.complete_task(second_task, True)

        status, completed_duplicate = request_json(
            service.http_port,
            "/api/tts/speak",
            {"request_id": "request-1", "text": "仍然不重复播放"},
        )
        assert status == 202
        assert completed_duplicate["duplicate"] is True
        assert completed_duplicate["queue_position"] == 0
        assert service.outstanding == 0
    finally:
        service.stop()


def test_local_wav_uses_same_queue(module):
    engine = FakeTtsEngine(module)
    service = module.UnifiedSpeechService(
        tts_engine=engine,
        http_bind="127.0.0.1",
        http_port=0,
        max_queue=100,
    )
    service.start(enable_http=False)
    service._play_segment_once = lambda _segment, _device: (0, "")
    try:
        with tempfile.TemporaryDirectory(prefix="gwv3_tts_test_") as temporary:
            wav = Path(temporary) / "prompt.wav"
            wav.write_bytes(b"RIFF")
            local = service.enqueue_wav(
                wav,
                source="wake-response",
                opens_command_window=True,
            )
            remote = service.enqueue_text("remote-1", "远程提示")
            assert local is not None
            assert remote.queue_position == 2

            local_ready = wait_ready_task(service)
            assert local_ready.task_id == local.task_id
            assert local_ready.opens_command_window is True
            assert service.play_task(local_ready, "test-device") is True
            service.complete_task(local_ready, True)

            remote_ready = wait_ready_task(service)
            assert remote_ready.task_id == remote.task_id
            assert service.play_task(remote_ready, "test-device") is True
            service.complete_task(remote_ready, True)
            assert service.outstanding == 0
    finally:
        service.stop()


def test_speaker_retry_deadline(module):
    engine = FakeTtsEngine(module)
    service = module.UnifiedSpeechService(
        tts_engine=engine,
        http_bind="127.0.0.1",
        http_port=0,
        speaker_retry_seconds=0.05,
    )
    attempts = 0

    def fail_playback(_segment, _device):
        nonlocal attempts
        attempts += 1
        return 1, "injected speaker failure"

    service._play_segment_once = fail_playback
    started = time.monotonic()
    ok = service._play_segment_with_retry(
        module.PcmSegment(data=b"\x00\x00", sample_rate=16000),
        "missing-device",
    )
    elapsed = time.monotonic() - started
    assert ok is False
    assert attempts >= 2
    assert elapsed >= 0.045
    assert elapsed < 0.5


def test_player_start_failure_is_reported(module):
    engine = FakeTtsEngine(module)
    service = module.UnifiedSpeechService(tts_engine=engine)
    original_popen = module.subprocess.Popen

    def missing_player(*_args, **_kwargs):
        raise FileNotFoundError("injected missing aplay")

    module.subprocess.Popen = missing_player
    try:
        return_code, error = service._play_segment_once(
            module.PcmSegment(data=b"\x00\x00", sample_rate=16000),
            "test-device",
        )
    finally:
        module.subprocess.Popen = original_popen
    assert return_code == 127
    assert "injected missing aplay" in error


def main():
    source_root = Path(__file__).resolve().parents[1]
    module = load_speech_module(source_root)
    test_sentence_splitting(module)
    test_espeak_mixed_language_markup(module)
    test_edge_tts_cache_and_fallback(module)
    test_http_queue_and_idempotency(module)
    test_local_wav_uses_same_queue(module)
    test_speaker_retry_deadline(module)
    test_player_start_failure_is_reported(module)
    print("voice TTS service test passed")


if __name__ == "__main__":
    main()
