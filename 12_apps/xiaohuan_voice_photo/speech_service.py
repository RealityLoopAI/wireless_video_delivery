#!/usr/bin/env python3
import html
import io
import json
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import uuid
import wave
from collections import deque
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional


STREAM_END = object()


class SpeechQueueFull(Exception):
    pass


class SpeechValidationError(ValueError):
    pass


@dataclass(frozen=True)
class PcmSegment:
    data: bytes
    sample_rate: int


@dataclass(frozen=True)
class WavSegment:
    path: Path


@dataclass
class SpeechTask:
    task_id: str
    request_id: str
    source: str
    text: str = ""
    wav_path: Optional[Path] = None
    opens_command_window: bool = False
    segments: queue.Queue = field(default_factory=lambda: queue.Queue(maxsize=2))
    cancelled: threading.Event = field(default_factory=threading.Event)
    synthesis_error: str = ""


@dataclass(frozen=True)
class EnqueueResult:
    task_id: str
    request_id: str
    queue_position: int
    duplicate: bool


def split_tts_text(text: str, max_segment_chars: int = 120):
    text = text.strip()
    if not text:
        return []

    sentences = re.split(r"(?<=[。！？!?；;\n])", text)
    segments = []
    for sentence in sentences:
        sentence = sentence.strip()
        while len(sentence) > max_segment_chars:
            search_start = max_segment_chars // 2
            split_at = -1
            for delimiter in ("，", ",", "、", "：", ":", " "):
                candidate = sentence.rfind(delimiter, search_start, max_segment_chars + 1)
                split_at = max(split_at, candidate)
            if split_at < search_start:
                split_at = max_segment_chars
            else:
                split_at += 1
            segments.append(sentence[:split_at].strip())
            sentence = sentence[split_at:].strip()
        if sentence:
            segments.append(sentence)
    return segments


def make_espeak_ssml(text: str):
    english_pattern = re.compile(
        r"[A-Za-z]+[A-Za-z0-9_'./+#-]*(?:\s+[A-Za-z]+[A-Za-z0-9_'./+#-]*)*"
    )
    parts = []
    position = 0
    for match in english_pattern.finditer(text):
        parts.append(html.escape(text[position : match.start()]))
        parts.append(
            '<voice name="en-us">'
            + html.escape(match.group(0))
            + "</voice>"
        )
        position = match.end()
    parts.append(html.escape(text[position:]))
    return "<speak>" + "".join(parts) + "</speak>"


class EspeakTtsEngine:
    def __init__(
        self,
        executable: str = "espeak-ng",
        voice: str = "cmn",
        speed: int = 175,
    ):
        self.executable = executable
        self.voice = voice
        self.speed = speed
        self.ready = False
        self.load_error = ""
        self.backend_name = "espeak-ng"

    def load(self):
        try:
            executable = shutil.which(self.executable)
            if executable is None:
                raise FileNotFoundError(f"{self.executable} is not installed")
            started = time.monotonic()
            probe = subprocess.run(
                [executable, "--voices", self.voice],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5.0,
                check=False,
            )
            if probe.returncode != 0 or self.voice.encode() not in probe.stdout:
                error = probe.stderr.decode("utf-8", errors="replace").strip()
                raise RuntimeError(error or f"voice {self.voice} is unavailable")
            self.executable = executable
            self.ready = True
            self.load_error = ""
            print(
                f"offline TTS loaded backend=espeak-ng voice={self.voice} "
                f"speed={self.speed} seconds={time.monotonic() - started:.3f}",
                flush=True,
            )
        except Exception as exc:
            self.ready = False
            self.load_error = str(exc)
            print(f"offline TTS unavailable: {exc}", file=sys.stderr, flush=True)

    def synthesize(self, text: str):
        if not self.ready:
            raise RuntimeError(self.load_error or "offline TTS is not ready")
        started = time.monotonic()
        result = subprocess.run(
            [
                self.executable,
                "-m",
                "-v",
                self.voice,
                "-s",
                str(self.speed),
                "--stdout",
                make_espeak_ssml(text),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10.0,
            check=False,
        )
        if result.returncode != 0:
            error = result.stderr.decode("utf-8", errors="replace").strip()
            raise RuntimeError(error or f"espeak-ng exited with {result.returncode}")

        try:
            with wave.open(io.BytesIO(result.stdout), "rb") as audio:
                if audio.getnchannels() != 1 or audio.getsampwidth() != 2:
                    raise RuntimeError(
                        f"unsupported eSpeak PCM format: "
                        f"channels={audio.getnchannels()} width={audio.getsampwidth()}"
                    )
                sample_rate = audio.getframerate()
                pcm = audio.readframes(audio.getnframes())
        except (EOFError, wave.Error) as exc:
            raise RuntimeError(f"invalid eSpeak WAV output: {exc}") from exc
        if not pcm:
            raise RuntimeError("espeak-ng returned empty audio")

        elapsed = time.monotonic() - started
        duration = len(pcm) / (sample_rate * 2)
        print(
            f"offline TTS generated backend=espeak-ng chars={len(text)} "
            f"seconds={elapsed:.3f} audio_seconds={duration:.3f} "
            f"rtf={elapsed / duration:.3f}",
            flush=True,
        )
        return PcmSegment(data=pcm, sample_rate=sample_rate)


class OfflineTtsEngine:
    def __init__(
        self,
        model_dir: Path,
        num_threads: int = 4,
        speed: float = 1.0,
    ):
        self.model_dir = Path(model_dir)
        self.num_threads = num_threads
        self.speed = speed
        self.ready = False
        self.load_error = ""
        self.backend_name = "sherpa-onnx-vits"
        self._tts = None
        self._sherpa_onnx = None

    def load(self):
        try:
            import sherpa_onnx

            model = self.model_dir / "model.onnx"
            lexicon = self.model_dir / "lexicon.txt"
            tokens = self.model_dir / "tokens.txt"
            required = (model, lexicon, tokens)
            missing = [str(path) for path in required if not path.is_file()]
            if missing:
                raise FileNotFoundError(f"missing TTS model files: {', '.join(missing)}")

            rule_fsts = [
                str(path)
                for path in (
                    self.model_dir / "date.fst",
                    self.model_dir / "number.fst",
                )
                if path.is_file()
            ]
            config = sherpa_onnx.OfflineTtsConfig(
                model=sherpa_onnx.OfflineTtsModelConfig(
                    vits=sherpa_onnx.OfflineTtsVitsModelConfig(
                        model=str(model),
                        lexicon=str(lexicon),
                        tokens=str(tokens),
                    ),
                    provider="cpu",
                    debug=False,
                    num_threads=self.num_threads,
                ),
                rule_fsts=",".join(rule_fsts),
                max_num_sentences=1,
            )
            if not config.validate():
                raise ValueError(f"invalid sherpa-onnx TTS config: {self.model_dir}")

            started = time.monotonic()
            self._tts = sherpa_onnx.OfflineTts(config)
            self._sherpa_onnx = sherpa_onnx
            self.ready = True
            self.load_error = ""
            print(
                f"offline TTS loaded model={self.model_dir} "
                f"threads={self.num_threads} seconds={time.monotonic() - started:.3f}",
                flush=True,
            )
        except Exception as exc:
            self.ready = False
            self.load_error = str(exc)
            print(f"offline TTS unavailable: {exc}", file=sys.stderr, flush=True)

    def synthesize(self, text: str):
        if not self.ready or self._tts is None or self._sherpa_onnx is None:
            raise RuntimeError(self.load_error or "offline TTS is not ready")

        import numpy as np

        generation = self._sherpa_onnx.GenerationConfig()
        generation.sid = 0
        generation.speed = self.speed
        generation.silence_scale = 0.2
        started = time.monotonic()
        audio = self._tts.generate(text, generation)
        samples = np.asarray(audio.samples, dtype=np.float32)
        if samples.size == 0:
            raise RuntimeError("offline TTS returned empty audio")
        pcm = (np.clip(samples, -1.0, 1.0) * 32767.0).astype("<i2").tobytes()
        duration = samples.size / audio.sample_rate
        elapsed = time.monotonic() - started
        print(
            f"offline TTS generated chars={len(text)} seconds={elapsed:.3f} "
            f"audio_seconds={duration:.3f} rtf={elapsed / duration:.3f}",
            flush=True,
        )
        return PcmSegment(data=pcm, sample_rate=audio.sample_rate)


class ReusableThreadingHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True


class UnifiedSpeechService:
    def __init__(
        self,
        tts_engine,
        http_bind: str = "0.0.0.0",
        http_port: int = 18082,
        max_queue: int = 100,
        max_text_chars: int = 500,
        max_request_id_chars: int = 128,
        max_body_bytes: int = 8192,
        speaker_retry_seconds: float = 5.0,
    ):
        self.tts_engine = tts_engine
        self.http_bind = http_bind
        self.http_port = http_port
        self.max_queue = max_queue
        self.max_text_chars = max_text_chars
        self.max_request_id_chars = max_request_id_chars
        self.max_body_bytes = max_body_bytes
        self.speaker_retry_seconds = speaker_retry_seconds

        self._input_queue = queue.Queue()
        self._playback_queue = queue.Queue()
        self._state_lock = threading.Lock()
        self._request_records = {}
        self._task_order = deque()
        self._outstanding = 0
        self._stop = threading.Event()
        self._synthesis_thread = None
        self._http_server = None
        self._http_thread = None
        self._player_lock = threading.Lock()
        self._current_player = None

    @property
    def tts_ready(self):
        return bool(getattr(self.tts_engine, "ready", False))

    @property
    def outstanding(self):
        with self._state_lock:
            return self._outstanding

    def start(self, enable_http: bool = True):
        self.tts_engine.load()
        self._synthesis_thread = threading.Thread(
            target=self._synthesis_worker,
            name="xiaohuan-tts-synthesis",
            daemon=True,
        )
        self._synthesis_thread.start()
        if enable_http:
            self._start_http()

    def request_stop(self):
        self._stop.set()
        with self._player_lock:
            player = self._current_player
        if player is not None and player.poll() is None:
            player.terminate()

    def stop(self):
        self.request_stop()
        if self._http_server is not None:
            self._http_server.shutdown()
            self._http_server.server_close()
        if self._http_thread is not None:
            self._http_thread.join(timeout=2.0)
        self._input_queue.put(None)
        if self._synthesis_thread is not None:
            self._synthesis_thread.join(timeout=5.0)

    def enqueue_text(self, request_id: str, text: str):
        request_id, text = self._validate_text_request(request_id, text)
        with self._state_lock:
            existing = self._request_records.get(request_id)
            if existing is not None:
                return EnqueueResult(
                    task_id=existing["task_id"],
                    request_id=request_id,
                    queue_position=self._position_locked(existing["task_id"]),
                    duplicate=True,
                )
            if self._outstanding >= self.max_queue:
                raise SpeechQueueFull("speech queue is full")
            task = SpeechTask(
                task_id=uuid.uuid4().hex,
                request_id=request_id,
                source="http",
                text=text,
            )
            self._request_records[request_id] = {
                "task_id": task.task_id,
                "state": "queued",
            }
            position = self._append_task_locked(task)
        self._input_queue.put(task)
        return EnqueueResult(
            task_id=task.task_id,
            request_id=request_id,
            queue_position=position,
            duplicate=False,
        )

    def enqueue_wav(
        self,
        wav_path: Path,
        source: str,
        opens_command_window: bool = False,
    ):
        wav_path = Path(wav_path)
        if not wav_path.is_file():
            print(f"speech wav not found: {wav_path}", file=sys.stderr, flush=True)
            return None
        with self._state_lock:
            if self._outstanding >= self.max_queue:
                print(
                    f"speech queue full; local prompt dropped source={source}",
                    file=sys.stderr,
                    flush=True,
                )
                return None
            task = SpeechTask(
                task_id=uuid.uuid4().hex,
                request_id="",
                source=source,
                wav_path=wav_path,
                opens_command_window=opens_command_window,
            )
            position = self._append_task_locked(task)
        self._input_queue.put(task)
        print(
            f"speech prompt queued source={source} task_id={task.task_id} "
            f"queue_position={position}",
            flush=True,
        )
        return task

    def next_ready_task(self, timeout: Optional[float] = None):
        try:
            if timeout is None:
                return self._playback_queue.get_nowait()
            return self._playback_queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def play_task(self, task: SpeechTask, playback_device: str):
        success = True
        while not self._stop.is_set() and not task.cancelled.is_set():
            try:
                segment = task.segments.get(timeout=0.1)
            except queue.Empty:
                continue
            if segment is STREAM_END:
                if task.synthesis_error:
                    print(
                        f"speech synthesis failed task_id={task.task_id} "
                        f"error={task.synthesis_error}",
                        file=sys.stderr,
                        flush=True,
                    )
                    success = False
                break
            if not self._play_segment_with_retry(segment, playback_device):
                task.cancelled.set()
                success = False
                break
        if self._stop.is_set():
            task.cancelled.set()
            success = False
        return success

    def complete_task(self, task: SpeechTask, success: bool):
        with self._state_lock:
            try:
                self._task_order.remove(task.task_id)
            except ValueError:
                pass
            self._outstanding = max(0, self._outstanding - 1)
            if task.request_id:
                record = self._request_records.get(task.request_id)
                if record is not None:
                    record["state"] = "completed" if success else "failed"
        print(
            f"speech task finished task_id={task.task_id} source={task.source} "
            f"success={str(success).lower()} outstanding={self.outstanding}",
            flush=True,
        )

    def health_payload(self):
        return {
            "ok": True,
            "service": "xiaohuan_speech",
            "tts_ready": self.tts_ready,
            "tts_backend": getattr(self.tts_engine, "backend_name", "unknown"),
            "queue_depth": self.outstanding,
            "queue_capacity": self.max_queue,
        }

    def _validate_text_request(self, request_id, text):
        if not isinstance(request_id, str) or not request_id.strip():
            raise SpeechValidationError("request_id must be a non-empty string")
        request_id = request_id.strip()
        if len(request_id) > self.max_request_id_chars:
            raise SpeechValidationError(
                f"request_id exceeds {self.max_request_id_chars} characters"
            )
        if not isinstance(text, str) or not text.strip():
            raise SpeechValidationError("text must be a non-empty string")
        text = text.strip()
        if len(text) > self.max_text_chars:
            raise SpeechValidationError(
                f"text exceeds {self.max_text_chars} characters"
            )
        return request_id, text

    def _append_task_locked(self, task: SpeechTask):
        self._task_order.append(task.task_id)
        self._outstanding += 1
        return self._outstanding

    def _position_locked(self, task_id: str):
        try:
            return list(self._task_order).index(task_id) + 1
        except ValueError:
            return 0

    def _put_task_segment(self, task: SpeechTask, segment):
        while not self._stop.is_set() and not task.cancelled.is_set():
            try:
                task.segments.put(segment, timeout=0.1)
                return True
            except queue.Full:
                continue
        return False

    def _synthesis_worker(self):
        while not self._stop.is_set():
            try:
                task = self._input_queue.get(timeout=0.2)
            except queue.Empty:
                continue
            if task is None:
                break

            announced = False
            try:
                if task.wav_path is not None:
                    segments = [WavSegment(task.wav_path)]
                else:
                    segments = split_tts_text(task.text)
                    if not segments:
                        raise RuntimeError("text produced no TTS segments")

                for text_or_segment in segments:
                    if task.cancelled.is_set() or self._stop.is_set():
                        break
                    if isinstance(text_or_segment, str):
                        segment = self.tts_engine.synthesize(text_or_segment)
                    else:
                        segment = text_or_segment
                    if not self._put_task_segment(task, segment):
                        break
                    if not announced:
                        self._playback_queue.put(task)
                        announced = True
            except Exception as exc:
                task.synthesis_error = str(exc)
                print(
                    f"speech preparation failed task_id={task.task_id} "
                    f"source={task.source} error={exc}",
                    file=sys.stderr,
                    flush=True,
                )
            finally:
                if announced and not task.cancelled.is_set():
                    self._put_task_segment(task, STREAM_END)
                elif not announced:
                    self.complete_task(task, False)

    def _play_segment_with_retry(self, segment, playback_device: str):
        failure_deadline = None
        last_error = ""
        while not self._stop.is_set():
            return_code, error = self._play_segment_once(segment, playback_device)
            if return_code == 0:
                return True
            last_error = error or f"aplay exited with {return_code}"
            if failure_deadline is None:
                failure_deadline = time.monotonic() + self.speaker_retry_seconds
            remaining = failure_deadline - time.monotonic()
            if remaining <= 0:
                print(
                    f"speaker unavailable after {self.speaker_retry_seconds:.1f}s: "
                    f"{last_error}",
                    file=sys.stderr,
                    flush=True,
                )
                return False
            time.sleep(min(0.2, remaining))
        return False

    def _play_segment_once(self, segment, playback_device: str):
        if isinstance(segment, WavSegment):
            command = ["aplay", "-q", "-D", playback_device, str(segment.path)]
            payload = None
        else:
            command = [
                "aplay",
                "-q",
                "-D",
                playback_device,
                "-t",
                "raw",
                "-f",
                "S16_LE",
                "-r",
                str(segment.sample_rate),
                "-c",
                "1",
            ]
            payload = segment.data

        try:
            player = subprocess.Popen(
                command,
                stdin=subprocess.PIPE if payload is not None else subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
        except OSError as exc:
            return 127, str(exc)
        with self._player_lock:
            self._current_player = player
        try:
            _stdout, stderr = player.communicate(input=payload)
            error = stderr.decode("utf-8", errors="replace").strip() if stderr else ""
            return player.returncode, error
        finally:
            with self._player_lock:
                if self._current_player is player:
                    self._current_player = None

    def _start_http(self):
        service = self

        class SpeechRequestHandler(BaseHTTPRequestHandler):
            server_version = "XiaohuanSpeech/1.0"

            def do_POST(self):
                self.connection.settimeout(2.0)
                if self.path != "/api/tts/speak":
                    self._send_json(404, {"accepted": False, "error": "not_found"})
                    return
                if not service.tts_ready:
                    self._send_json(
                        503,
                        {
                            "accepted": False,
                            "error": "tts_unavailable",
                            "detail": getattr(service.tts_engine, "load_error", ""),
                        },
                    )
                    return
                content_length = self.headers.get("Content-Length")
                try:
                    body_size = int(content_length)
                except (TypeError, ValueError):
                    self._send_json(
                        411,
                        {"accepted": False, "error": "content_length_required"},
                    )
                    return
                if body_size < 0 or body_size > service.max_body_bytes:
                    self._send_json(
                        413,
                        {"accepted": False, "error": "request_too_large"},
                    )
                    return
                try:
                    body = self.rfile.read(body_size)
                    if len(body) != body_size:
                        raise SpeechValidationError("incomplete request body")
                    payload = json.loads(body.decode("utf-8"))
                    if not isinstance(payload, dict):
                        raise SpeechValidationError("JSON body must be an object")
                    result = service.enqueue_text(
                        payload.get("request_id"),
                        payload.get("text"),
                    )
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    self._send_json(
                        400,
                        {"accepted": False, "error": "invalid_json", "detail": str(exc)},
                    )
                    return
                except SpeechValidationError as exc:
                    self._send_json(
                        400,
                        {
                            "accepted": False,
                            "error": "invalid_request",
                            "detail": str(exc),
                        },
                    )
                    return
                except TimeoutError:
                    self._send_json(
                        408,
                        {"accepted": False, "error": "request_timeout"},
                    )
                    return
                except SpeechQueueFull:
                    self._send_json(
                        429,
                        {"accepted": False, "error": "queue_full"},
                    )
                    return

                self._send_json(
                    202,
                    {
                        "accepted": True,
                        "request_id": result.request_id,
                        "task_id": result.task_id,
                        "queue_position": result.queue_position,
                        "duplicate": result.duplicate,
                    },
                )

            def do_GET(self):
                if self.path == "/healthz":
                    self._send_json(200, service.health_payload())
                else:
                    self._send_json(404, {"error": "not_found"})

            def log_message(self, fmt, *args):
                print(f"speech HTTP {self.address_string()} {fmt % args}", flush=True)

            def _send_json(self, status, payload):
                body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
                try:
                    self.send_response(status)
                    self.send_header("Content-Type", "application/json; charset=utf-8")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        self._http_server = ReusableThreadingHTTPServer(
            (self.http_bind, self.http_port),
            SpeechRequestHandler,
        )
        self.http_port = self._http_server.server_address[1]
        self._http_thread = threading.Thread(
            target=self._http_server.serve_forever,
            name="xiaohuan-tts-http",
            daemon=True,
        )
        self._http_thread.start()
        print(
            f"speech HTTP listening http://{self.http_bind}:{self.http_port} "
            f"endpoint=/api/tts/speak queue_capacity={self.max_queue}",
            flush=True,
        )
