#!/usr/bin/env python3
import importlib.util
import io
import json
import os
from pathlib import Path
import socket
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
    sys.path.insert(0, str(module_path.parent))
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


def test_wake_text_matching(module, defaults):
    aliases = list(
        dict.fromkeys(module.normalize_text(item) for item in defaults.alias)
    )
    for text in ["你好小环", "你好 小环", "您好小环", "您好 小环"]:
        assert module.is_wake_text(text, aliases), text
    for text in [
        "",
        "[unk]",
        "你好",
        "小环",
        "小环你好",
        "[unk]你好小环",
        "你好小环[unk]",
        "你好小环你好小环",
        "大家你好小环",
    ]:
        assert not module.is_wake_text(text, aliases), text

    matcher = module.SplitWakeMatcher(2.4)
    assert matcher.update("小环 你好", 1.0, aliases) == (False, "")
    assert matcher.update("你好", 2.0, aliases) == (False, "")
    assert matcher.update("小环", 3.0, aliases) == (True, "split")

    strict_args = types.SimpleNamespace(
        wake_require_end_silence=True,
        wake_decode_max_seconds=3.2,
    )
    assert module.wake_segment_rejection_reason(strict_args, False, True, 2.0) == ""
    assert (
        module.wake_segment_rejection_reason(strict_args, False, False, 2.0)
        == "wake_missing_end_silence"
    )
    assert (
        module.wake_segment_rejection_reason(strict_args, False, True, 3.3)
        == "wake_too_long"
    )
    assert module.wake_segment_rejection_reason(strict_args, True, False, 4.0) == ""


def test_audio_stream_command(module):
    args = types.SimpleNamespace(
        record_device="plughw:3,0",
        sample_rate=16000,
        audio_stream_host="192.168.66.32",
        audio_stream_port=50020,
        audio_stream_sample_rate=48000,
        audio_stream_bitrate=64000,
    )
    command = module.make_streaming_capture_cmd(args)
    command_text = " ".join(command)
    assert command[0] == "gst-launch-1.0"
    assert "device=plughw:3,0" in command
    assert "rate=48000" in command_text
    assert "rate=16000" in command_text
    assert "leaky=downstream" in command
    assert "bitrate=64000" in command
    assert "complexity=5" in command
    assert "host=192.168.66.32" in command
    assert "port=50020" in command
    gated_command = module.make_streaming_capture_cmd(
        args,
        ("127.0.0.1", 43123),
    )
    assert "host=127.0.0.1" in gated_command
    assert "port=43123" in gated_command
    assert module.udp_port("50020") == 50020
    assert module.opus_bitrate("64000") == 64000


def test_audio_stream_gate(module):
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind(("127.0.0.1", 0))
    receiver.settimeout(0.2)
    gate = module.UdpPacketGate("127.0.0.1", receiver.getsockname()[1])
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    gate.start()
    try:
        sender.sendto(b"forwarded", ("127.0.0.1", gate.local_port))
        assert receiver.recvfrom(1024)[0] == b"forwarded"

        gate.pause()
        sender.sendto(b"dropped", ("127.0.0.1", gate.local_port))
        try:
            receiver.recvfrom(1024)
        except socket.timeout:
            pass
        else:
            raise AssertionError("paused audio stream gate forwarded a packet")

        gate.resume()
        sender.sendto(b"resumed", ("127.0.0.1", gate.local_port))
        assert receiver.recvfrom(1024)[0] == b"resumed"
    finally:
        gate.stop()
        sender.close()
        receiver.close()


def test_playback_capture_drain(module):
    read_fd, write_fd = os.pipe()
    read_stream = os.fdopen(read_fd, "rb", buffering=0)
    drain = module.CapturePlaybackDrain(read_stream)
    drain.start()
    try:
        os.write(write_fd, b"x" * 4096)
        deadline = time.monotonic() + 0.5
        while drain.bytes_drained < 4096 and time.monotonic() < deadline:
            time.sleep(0.01)
        assert drain.bytes_drained == 4096
        assert drain.ended is False
    finally:
        drain.stop()
        read_stream.close()
        os.close(write_fd)


def test_audio_capture_read_timeout(module):
    read_fd, write_fd = os.pipe()
    read_stream = os.fdopen(read_fd, "rb", buffering=0)
    try:
        started = time.monotonic()
        assert module.read_capture_chunk(read_stream, 16, 0.03) == b""
        assert time.monotonic() - started < 0.2

        os.write(write_fd, b"0123456789abcdef")
        assert module.read_capture_chunk(read_stream, 16, 0.1) == b"0123456789abcdef"
    finally:
        read_stream.close()
        os.close(write_fd)


def test_command_audio_trimming(module):
    chunks = [bytes([index, index]) * 1600 for index in range(10)]
    trimmed = module.trim_command_pcm(
        chunks,
        6,
        chunk_seconds=0.1,
        tail_seconds=0.3,
    )
    assert trimmed == b"".join(chunks[:7])
    assert module.chunks_for_seconds(0.6, 0.1) == 6
    assert module.chunks_for_seconds(0.2, 0.1) == 2


def test_capture_termination_reaps_killed_process(module):
    class StubbornProcess:
        def __init__(self):
            self.running = True
            self.terminate_calls = 0
            self.kill_calls = 0
            self.wait_calls = 0

        def poll(self):
            return None if self.running else -9

        def terminate(self):
            self.terminate_calls += 1

        def kill(self):
            self.kill_calls += 1
            self.running = False

        def wait(self, timeout):
            del timeout
            self.wait_calls += 1
            if self.running:
                raise module.subprocess.TimeoutExpired("capture", 1)
            return -9

    process = StubbornProcess()
    module.terminate_capture([("stubborn", process)])
    assert process.terminate_calls == 1
    assert process.kill_calls == 1
    assert process.wait_calls == 2


def test_capture_streams_are_closed(module):
    capture_out = io.BytesIO()
    process = type(
        "CaptureProcess",
        (),
        {
            "stdin": io.BytesIO(),
            "stdout": capture_out,
            "stderr": io.BytesIO(),
        },
    )()
    module.close_capture_streams(capture_out, [("capture", process)])
    assert capture_out.closed
    assert process.stdin.closed
    assert process.stderr.closed


def test_usb_audio_exclusive_install(source_root: Path):
    app_root = source_root / "12_apps" / "xiaohuan_voice_photo"
    rule_path = app_root / "systemd" / "90-xiaohuan-usb-audio-exclusive.rules"
    rule_text = rule_path.read_text(encoding="utf-8")
    assert 'ATTRS{idVendor}=="8087"' in rule_text
    assert 'ATTRS{idProduct}=="1024"' in rule_text
    assert 'ATTRS{idVendor}=="08bb"' in rule_text
    assert 'ATTRS{idProduct}=="2902"' in rule_text
    assert rule_text.count('ENV{PULSE_IGNORE}="1"') == 2

    installer = (app_root / "install_wake_service.sh").read_text(encoding="utf-8")
    assert "90-xiaohuan-usb-audio-exclusive.rules" in installer
    assert "udevadm control --reload-rules" in installer
    assert "XIAOHUAN_CAPTURE_PLAYBACK_MODE" in (
        app_root / "run_wake_service.sh"
    ).read_text(encoding="utf-8")
    profile = (
        app_root / "systemd" / "xiaohuan-wake-utterance-forward.conf"
    ).read_text(encoding="utf-8")
    assert "XIAOHUAN_CAPTURE_PLAYBACK_MODE=restart" in profile
    assert "XIAOHUAN_AUDIO_STREAM_ENABLED=0" in profile
    assert "XIAOHUAN_UTTERANCE_FORWARD_ENABLED=1" in profile
    assert "http://192.168.66.113:50020/api/audio" in profile
    assert "XIAOHUAN_COMMAND_MAX_SPEECH_SECONDS=60" in profile


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
    assert defaults.audio_stream is False
    assert defaults.tts_http is True
    assert defaults.tts_http_port == 18082
    assert defaults.tts_backend == "edge"
    assert defaults.tts_max_queue == 100
    assert defaults.tts_max_text_chars == 500
    assert defaults.tts_speaker_retry_seconds == 15.0
    assert defaults.tts_resume_delay_seconds == 0.2
    assert defaults.audio_read_timeout_seconds == 2.0
    assert defaults.audio_recovery_seconds == 20.0
    assert defaults.audio_recovery_interval_seconds == 0.5
    assert defaults.echo_tail_seconds == 0.03
    assert defaults.capture_playback_mode == "keep"
    assert defaults.utterance_forward is False
    assert defaults.utterance_forward_queue == 8
    assert defaults.utterance_forward_retries == 3
    assert defaults.command_pre_roll_seconds == 0.2
    assert defaults.command_tail_seconds == 0.3
    assert defaults.command_end_silence_seconds == 0.6
    assert defaults.command_max_speech_seconds == 60.0
    assert defaults.allow_split_wake is False
    assert defaults.wake_require_end_silence is True
    assert defaults.wake_decode_max_seconds == 3.2
    test_wake_text_matching(module, defaults)
    test_photo_text_matching(module, defaults)
    test_audio_capture_read_timeout(module)
    test_command_audio_trimming(module)
    test_capture_termination_reaps_killed_process(module)
    test_capture_streams_are_closed(module)
    test_audio_stream_command(module)
    test_audio_stream_gate(module)
    test_playback_capture_drain(module)
    test_usb_audio_exclusive_install(source_root)
    test_async_capture_does_not_block(module)
    with tempfile.TemporaryDirectory(prefix="gwv3_voice_burst_") as temporary:
        root = Path(temporary)
        test_success(module, root / "success")
        test_partial_failure(module, root / "failure")
    print("voice photo burst test passed")


if __name__ == "__main__":
    main()
