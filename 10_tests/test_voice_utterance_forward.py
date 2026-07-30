#!/usr/bin/env python3
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import threading
import urllib.error
import wave


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class FakeResponse:
    status = 202

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def read(self, _size=-1):
        return b'{"accepted":true}'

    def getcode(self):
        return self.status


def make_wav(forward_module, seconds=0.2):
    return forward_module.pcm_to_wav_bytes(
        b"\x00\x00" * int(16000 * seconds),
        16000,
    )


def test_wav_encoding(forward_module):
    payload = make_wav(forward_module, 0.25)
    with wave.open(io.BytesIO(payload), "rb") as audio:
        assert audio.getnchannels() == 1
        assert audio.getsampwidth() == 2
        assert audio.getframerate() == 16000
        assert audio.getnframes() == 4000


def test_http_delivery(forward_module, receiver_module):
    with tempfile.TemporaryDirectory(prefix="gwv3_audio_receiver_") as temporary:
        receiver = receiver_module.AudioReceiver(
            Path(temporary),
            max_body_bytes=4 * 1024 * 1024,
            expected_sample_rate=16000,
            max_duration_seconds=61.0,
        )
        server = receiver_module.ReusableThreadingHTTPServer(
            ("127.0.0.1", 0),
            receiver_module.make_handler(receiver),
        )
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        forwarder = forward_module.UtteranceForwarder(
            f"http://127.0.0.1:{server.server_address[1]}/api/audio",
            timeout_seconds=1.0,
            retry_delay_seconds=0.01,
        )
        forwarder.start()
        try:
            assert forwarder.enqueue(make_wav(forward_module))
            assert forwarder.wait_idle(2.0)
            metrics = forwarder.metrics()
            assert metrics["completed"] == 1
            assert metrics["failed"] == 0
            saved = list(Path(temporary).glob("*/*.wav"))
            assert len(saved) == 1
            with wave.open(str(saved[0]), "rb") as audio:
                assert audio.getframerate() == 16000
        finally:
            forwarder.stop()
            server.shutdown()
            server.server_close()
            thread.join(timeout=1.0)


def test_retry_and_failure(forward_module):
    attempts = 0

    def flaky_opener(_request, timeout):
        nonlocal attempts
        assert timeout == 0.5
        attempts += 1
        if attempts < 3:
            raise urllib.error.URLError("injected failure")
        return FakeResponse()

    forwarder = forward_module.UtteranceForwarder(
        "http://127.0.0.1:1/api/audio",
        timeout_seconds=0.5,
        max_retries=3,
        retry_delay_seconds=0.01,
        opener=flaky_opener,
    )
    forwarder.start()
    try:
        assert forwarder.enqueue(make_wav(forward_module))
        assert forwarder.wait_idle(2.0)
        metrics = forwarder.metrics()
        assert attempts == 3
        assert metrics["completed"] == 1
        assert metrics["failed"] == 0
    finally:
        forwarder.stop()


def test_full_queue_drops_oldest(forward_module):
    first_started = threading.Event()
    release_first = threading.Event()
    delivered = []

    def blocking_opener(request, timeout):
        del timeout
        delivered.append(request.data)
        if len(delivered) == 1:
            first_started.set()
            assert release_first.wait(timeout=2.0)
        return FakeResponse()

    forwarder = forward_module.UtteranceForwarder(
        "http://127.0.0.1:1/api/audio",
        queue_capacity=2,
        timeout_seconds=1.0,
        opener=blocking_opener,
    )
    payloads = [make_wav(forward_module, 0.10 + (index * 0.01)) for index in range(4)]
    forwarder.start()
    try:
        assert forwarder.enqueue(payloads[0])
        assert first_started.wait(timeout=1.0)
        assert forwarder.enqueue(payloads[1])
        assert forwarder.enqueue(payloads[2])
        assert forwarder.enqueue(payloads[3])
        release_first.set()
        assert forwarder.wait_idle(3.0)
        metrics = forwarder.metrics()
        assert metrics["accepted"] == 4
        assert metrics["dropped"] == 1
        assert metrics["completed"] == 3
        assert delivered == [payloads[0], payloads[2], payloads[3]]
    finally:
        release_first.set()
        forwarder.stop()


def main():
    source_root = Path(__file__).resolve().parents[1]
    forward_module = load_module(
        "gwv3_utterance_forwarder_test",
        source_root
        / "12_apps"
        / "xiaohuan_voice_photo"
        / "utterance_forwarder.py",
    )
    receiver_module = load_module(
        "gwv3_audio_receiver_test",
        source_root / "05_tools" / "xiaohuan_audio_receiver.py",
    )
    test_wav_encoding(forward_module)
    test_http_delivery(forward_module, receiver_module)
    test_retry_and_failure(forward_module)
    test_full_queue_drops_oldest(forward_module)
    print("voice utterance forward test passed")


if __name__ == "__main__":
    main()
