#!/usr/bin/env python3
from array import array
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import time
import wave
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


def test_fixed_xiaoyi_prompts_are_trimmed(source_root: Path):
    prompts = {
        "response_wozai_tts_default.wav": 1.0,
        "response_wozai_tts_fast.wav": 1.0,
        "response_photo_done.wav": 1.4,
        "cue_photo_ding.wav": 0.5,
        "cue_forward_deng.wav": 0.5,
        "cue_power_startup.wav": 0.8,
        "cue_power_shutdown.wav": 0.8,
    }
    prompt_dir = source_root / "12_apps" / "xiaohuan_voice_photo"
    threshold = int(32767 * (10 ** (-45 / 20)))
    for filename, max_duration in prompts.items():
        with wave.open(str(prompt_dir / filename), "rb") as audio:
            assert audio.getsampwidth() == 2
            channels = audio.getnchannels()
            sample_rate = audio.getframerate()
            frame_count = audio.getnframes()
            samples = array("h")
            samples.frombytes(audio.readframes(frame_count))
        if sys.byteorder != "little":
            samples.byteswap()
        active_frames = [
            frame
            for frame in range(frame_count)
            if max(
                abs(samples[frame * channels + channel])
                for channel in range(channels)
            )
            > threshold
        ]
        assert active_frames
        duration = frame_count / sample_rate
        trailing_silence = duration - ((active_frames[-1] + 1) / sample_rate)
        assert duration <= max_duration
        assert trailing_silence <= 0.10


def test_edge_tts_persistent_cache_and_failure(module):
    with tempfile.TemporaryDirectory(prefix="gwv3_edge_cache_") as temporary:
        cache_dir = Path(temporary) / "cache"
        edge_module = FakeEdgeModule()
        engine = module.EdgeTtsEngine(
            voice="zh-CN-XiaoyiNeural",
            edge_module=edge_module,
            decoder_executable="/bin/true",
            disk_cache_dir=cache_dir,
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
        assert edge_module.calls[0]["rate"] == "+0%"
        assert edge_module.calls[0]["volume"] == "+0%"
        assert edge_module.calls[0]["pitch"] == "+0Hz"
        assert len(list(cache_dir.glob("*.mp3"))) == 1

        reloaded_module = FakeEdgeModule()
        reloaded = module.EdgeTtsEngine(
            voice="zh-CN-XiaoyiNeural",
            edge_module=reloaded_module,
            decoder_executable="/bin/true",
            disk_cache_dir=cache_dir,
        )
        reloaded.load()
        reloaded._decode_mp3 = engine._decode_mp3
        cached_after_restart = reloaded.synthesize("缓存测试")
        assert cached_after_restart == first
        assert reloaded_module.calls == []

        failing_module = FakeEdgeModule(fail=True)
        failing_engine = module.EdgeTtsEngine(
            edge_module=failing_module,
            decoder_executable="/bin/true",
            disk_cache_dir=Path(temporary) / "failing-cache",
        )
        failing_engine.load()
        for text in ("合成失败", "退避期直接失败"):
            try:
                failing_engine.synthesize(text)
            except RuntimeError:
                pass
            else:
                raise AssertionError("Edge failure must not fall back to another voice")
        assert len(failing_module.calls) == 1

        limited_cache = Path(temporary) / "limited-cache"
        limited_engine = module.EdgeTtsEngine(
            edge_module=FakeEdgeModule(),
            decoder_executable="/bin/true",
            disk_cache_dir=limited_cache,
            disk_cache_max_bytes=10,
        )
        limited_engine.load()
        limited_engine._disk_cache_put("第一条", b"12345678")
        time.sleep(0.01)
        limited_engine._disk_cache_put("第二条", b"abcdefgh")
        cached_files = list(limited_cache.glob("*.mp3"))
        assert len(cached_files) == 1
        assert sum(path.stat().st_size for path in cached_files) <= 10


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


def test_http_local_cue_queue(module):
    with tempfile.TemporaryDirectory(prefix="gwv3_tts_cue_") as temporary:
        ding = Path(temporary) / "ding.wav"
        deng = Path(temporary) / "deng.wav"
        ding.write_bytes(b"RIFF")
        deng.write_bytes(b"RIFF")
        engine = FakeTtsEngine(module)
        service = module.UnifiedSpeechService(
            tts_engine=engine,
            http_bind="127.0.0.1",
            http_port=0,
            cue_paths={"ding": ding, "deng": deng},
        )
        service.start(enable_http=True)
        service._play_segment_once = lambda _segment, _device: (0, "")
        try:
            status, health = request_json(service.http_port, "/healthz")
            assert status == 200
            assert health["cue_names"] == ["deng", "ding"]

            status, cue = request_json(
                service.http_port,
                "/api/audio/cue",
                {"request_id": "cue-1", "cue": "ding"},
            )
            assert status == 202
            assert cue["accepted"] is True
            assert cue["duplicate"] is False

            status, queued = request_json(
                service.http_port,
                "/api/audio/status?request_id=cue-1",
            )
            assert status == 200
            assert queued["state"] == "queued"
            assert queued["queue_position"] == 1

            cue_task = wait_ready_task(service)
            assert cue_task.source == "http-cue:ding"
            assert cue_task.wav_path == ding
            assert service.play_task(cue_task, "test-device") is True
            service.complete_task(cue_task, True)

            status, completed = request_json(
                service.http_port,
                "/api/audio/status?request_id=cue-1",
            )
            assert status == 200
            assert completed["state"] == "completed"
            assert completed["queue_position"] == 0

            status, duplicate = request_json(
                service.http_port,
                "/api/audio/cue",
                {"request_id": "cue-1", "cue": "deng"},
            )
            assert status == 202
            assert duplicate["duplicate"] is True
            assert duplicate["task_id"] == cue["task_id"]

            status, unknown = request_json(
                service.http_port,
                "/api/audio/cue",
                {"request_id": "cue-2", "cue": "unknown"},
            )
            assert status == 400
            assert unknown["accepted"] is False

            status, missing = request_json(
                service.http_port,
                "/api/audio/status?request_id=missing",
            )
            assert status == 404
            assert missing["error"] == "not_found"
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


def test_local_prompt_priority_and_http_deferral(module):
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
        remote = service.enqueue_text("remote-before-local", "远程提示")
        deadline = time.monotonic() + 1.0
        while service._playback_queue.empty() and time.monotonic() < deadline:
            time.sleep(0.01)
        assert not service._playback_queue.empty()

        with tempfile.TemporaryDirectory(prefix="gwv3_tts_priority_") as temporary:
            wav = Path(temporary) / "cue.wav"
            wav.write_bytes(b"RIFF")
            local = service.enqueue_wav(wav, source="photo-cue")
            assert local is not None

            local_ready = service.next_ready_task(allow_http=False)
            assert local_ready is not None
            assert local_ready.task_id == local.task_id
            assert service.play_task(local_ready, "test-device") is True
            service.complete_task(local_ready, True)

            assert service.next_ready_task(allow_http=False) is None
            remote_ready = wait_ready_task(service)
            assert remote_ready.task_id == remote.task_id
            assert service.play_task(remote_ready, "test-device") is True
            service.complete_task(remote_ready, True)
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


def test_hung_player_is_killed(module):
    engine = FakeTtsEngine(module)
    service = module.UnifiedSpeechService(tts_engine=engine)
    original_popen = module.subprocess.Popen

    class HungPlayer:
        def __init__(self):
            self.returncode = None
            self.killed = False
            self.calls = 0

        def communicate(self, input=None, timeout=None):
            del input, timeout
            self.calls += 1
            if self.calls == 1:
                raise module.subprocess.TimeoutExpired("aplay", 3)
            self.returncode = -9
            return b"", b""

        def kill(self):
            self.killed = True

        def poll(self):
            return self.returncode

        def terminate(self):
            self.returncode = -15

    player = HungPlayer()
    module.subprocess.Popen = lambda *_args, **_kwargs: player
    try:
        return_code, error = service._play_segment_once(
            module.PcmSegment(data=b"\x00\x00", sample_rate=16000),
            "test-device",
        )
    finally:
        module.subprocess.Popen = original_popen
    assert return_code == 124
    assert "timed out" in error
    assert player.killed is True


def main():
    source_root = Path(__file__).resolve().parents[1]
    module = load_speech_module(source_root)
    test_sentence_splitting(module)
    test_espeak_mixed_language_markup(module)
    test_fixed_xiaoyi_prompts_are_trimmed(source_root)
    test_edge_tts_persistent_cache_and_failure(module)
    test_http_queue_and_idempotency(module)
    test_http_local_cue_queue(module)
    test_local_wav_uses_same_queue(module)
    test_local_prompt_priority_and_http_deferral(module)
    test_speaker_retry_deadline(module)
    test_player_start_failure_is_reported(module)
    test_hung_player_is_killed(module)
    print("voice TTS service test passed")


if __name__ == "__main__":
    main()
