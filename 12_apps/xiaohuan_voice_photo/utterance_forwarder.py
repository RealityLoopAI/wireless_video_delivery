#!/usr/bin/env python3
import io
import queue
import threading
import time
import urllib.error
import urllib.request
import wave


def pcm_to_wav_bytes(pcm: bytes, sample_rate: int) -> bytes:
    if sample_rate <= 0:
        raise ValueError("sample_rate must be positive")
    if len(pcm) % 2 != 0:
        raise ValueError("16-bit PCM payload must contain complete samples")

    output = io.BytesIO()
    with wave.open(output, "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(sample_rate)
        audio.writeframes(pcm)
    return output.getvalue()


class UtteranceForwarder:
    def __init__(
        self,
        endpoint: str,
        *,
        queue_capacity: int = 8,
        timeout_seconds: float = 10.0,
        max_retries: int = 3,
        retry_delay_seconds: float = 0.5,
        opener=None,
    ):
        if queue_capacity <= 0:
            raise ValueError("queue_capacity must be positive")
        if timeout_seconds <= 0:
            raise ValueError("timeout_seconds must be positive")
        if max_retries < 0:
            raise ValueError("max_retries must be non-negative")
        if retry_delay_seconds < 0:
            raise ValueError("retry_delay_seconds must be non-negative")

        self.endpoint = endpoint
        self.queue_capacity = queue_capacity
        self.timeout_seconds = timeout_seconds
        self.max_retries = max_retries
        self.retry_delay_seconds = retry_delay_seconds
        self._opener = opener or urllib.request.urlopen
        self._queue = queue.Queue(maxsize=queue_capacity)
        self._stop = threading.Event()
        self._thread = None
        self._state_lock = threading.Lock()
        self._active = False
        self._accepted = 0
        self._completed = 0
        self._failed = 0
        self._dropped = 0

    def start(self):
        if self._thread is not None:
            return
        self._thread = threading.Thread(
            target=self._run,
            name="xiaohuan-utterance-forward",
            daemon=True,
        )
        self._thread.start()

    def stop(self):
        self._stop.set()
        try:
            self._queue.put_nowait(None)
        except queue.Full:
            pass
        if self._thread is not None:
            self._thread.join(timeout=1.0)

    def enqueue(self, wav_data: bytes) -> bool:
        if not wav_data.startswith(b"RIFF") or wav_data[8:12] != b"WAVE":
            raise ValueError("payload is not a WAV file")
        if self._stop.is_set():
            return False

        while True:
            try:
                self._queue.put_nowait(bytes(wav_data))
                with self._state_lock:
                    self._accepted += 1
                    depth = self._queue.qsize()
                print(
                    f"utterance queued bytes={len(wav_data)} queue_depth={depth}",
                    flush=True,
                )
                return True
            except queue.Full:
                try:
                    dropped = self._queue.get_nowait()
                except queue.Empty:
                    continue
                if dropped is not None:
                    with self._state_lock:
                        self._dropped += 1
                    print(
                        "utterance queue full; dropped oldest audio "
                        f"bytes={len(dropped)}",
                        flush=True,
                    )

    def wait_idle(self, timeout_seconds: float) -> bool:
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            with self._state_lock:
                active = self._active
            if self._queue.empty() and not active:
                return True
            time.sleep(0.01)
        return False

    def metrics(self):
        with self._state_lock:
            return {
                "accepted": self._accepted,
                "completed": self._completed,
                "failed": self._failed,
                "dropped": self._dropped,
                "queue_depth": self._queue.qsize(),
                "active": self._active,
            }

    def _run(self):
        while not self._stop.is_set():
            try:
                wav_data = self._queue.get(timeout=0.2)
            except queue.Empty:
                continue
            if wav_data is None:
                break
            with self._state_lock:
                self._active = True
            try:
                if self._send_with_retries(wav_data):
                    with self._state_lock:
                        self._completed += 1
                else:
                    with self._state_lock:
                        self._failed += 1
            finally:
                with self._state_lock:
                    self._active = False

    def _send_with_retries(self, wav_data: bytes) -> bool:
        started = time.monotonic()
        attempts = self.max_retries + 1
        last_error = ""
        for attempt in range(1, attempts + 1):
            if self._stop.is_set():
                return False
            request = urllib.request.Request(
                self.endpoint,
                data=wav_data,
                headers={
                    "Content-Type": "audio/wav",
                    "Content-Length": str(len(wav_data)),
                },
                method="POST",
            )
            try:
                with self._opener(
                    request,
                    timeout=self.timeout_seconds,
                ) as response:
                    status = getattr(response, "status", None)
                    if status is None:
                        status = response.getcode()
                    status = int(status)
                    response.read(4096)
                if 200 <= status < 300:
                    print(
                        f"utterance delivered endpoint={self.endpoint} "
                        f"bytes={len(wav_data)} attempts={attempt} "
                        f"elapsed_ms={round((time.monotonic() - started) * 1000)}",
                        flush=True,
                    )
                    return True
                last_error = f"HTTP {status}"
            except urllib.error.HTTPError as exc:
                last_error = f"HTTP {exc.code}"
            except (OSError, TimeoutError, urllib.error.URLError) as exc:
                last_error = str(exc)

            if attempt >= attempts:
                break
            delay = self.retry_delay_seconds * (2 ** min(attempt - 1, 3))
            print(
                f"utterance delivery retry attempt={attempt} "
                f"remaining={attempts - attempt} delay={delay:.2f}s "
                f"error={last_error}",
                flush=True,
            )
            if self._stop.wait(delay):
                return False

        print(
            f"utterance delivery failed endpoint={self.endpoint} "
            f"bytes={len(wav_data)} attempts={attempts} error={last_error}",
            flush=True,
        )
        return False
