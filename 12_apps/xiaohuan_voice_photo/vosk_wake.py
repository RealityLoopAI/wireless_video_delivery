#!/usr/bin/env python3
import argparse
import collections
from datetime import datetime
import json
import os
import select
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import numpy as np
from vosk import KaldiRecognizer, Model, SetLogLevel

from speech_service import (
    EdgeTtsEngine,
    EspeakTtsEngine,
    OfflineTtsEngine,
    UnifiedSpeechService,
)

try:
    import webrtcvad
except ImportError:
    webrtcvad = None


BASE_DIR = Path(__file__).resolve().parent
MODEL_DIR = BASE_DIR / "models" / "vosk-model-small-cn-0.22"
DEFAULT_RESPONSE = BASE_DIR / "response_wozai_tts_default.wav"
DEFAULT_PHOTO_RESPONSE = BASE_DIR / "response_photo_done.wav"
DEFAULT_PHOTO_REQUEST_DIR = Path("/tmp/gemini_rgb_snapshot_requests")
DEFAULT_PHOTO_RESULT_DIR = Path("/tmp/gemini_rgb_snapshot_results")
DEFAULT_PHOTO_OUTPUT_ROOT = Path("/home/orangepi/Desktop/Photos")
DEFAULT_TTS_MODEL_DIR = BASE_DIR / "models" / "vits-melo-tts-zh_en"
DEFAULT_EDGE_TTS_CACHE_DIR = Path.home() / ".cache" / "xiaohuan" / "edge_tts"
PHOTO_CAPTURE_LOCK = threading.Lock()

AUDIO_FILTERS = {
    "none": "",
    "voice-lite": "highpass=f=80,lowpass=f=7600",
    "voice": "highpass=f=80,lowpass=f=7600,afftdn=nf=-30",
    "voice-agc": "highpass=f=80,lowpass=f=7600,afftdn=nf=-30,dynaudnorm=f=75:g=8:p=0.9",
}

# Some speakers pronounce "拍照" as "pai zao" rather than "pai zhao".
# Keep these alternatives scoped to the post-wake photo grammar.
PHOTO_PAI_SYLLABLES = ("拍", "排", "牌", "派")
PHOTO_ZAO_SYLLABLES = ("照", "早", "造", "澡", "灶", "遭")
PHOTO_PHONETIC_FORMS = frozenset(
    first + second
    for first in PHOTO_PAI_SYLLABLES
    for second in PHOTO_ZAO_SYLLABLES
)
PHOTO_PARTIAL_FORMS = frozenset(PHOTO_PAI_SYLLABLES + PHOTO_ZAO_SYLLABLES)


def normalize_text(text: str):
    return "".join(ch for ch in text if not ch.isspace())


def is_wake_text(text: str, aliases):
    text = normalize_text(text)
    if not text:
        return False
    return text in aliases


def is_photo_text(text: str, aliases):
    compact = normalize_text(text)
    if not compact:
        return False
    if any(alias in compact for alias in aliases):
        return True
    # The constrained Vosk grammar may retain only one syllable or emit a
    # homophone. This matcher is only called in the post-wake command mode.
    has_unknown = "[unk]" in compact
    compact_without_unknown = compact.replace("[unk]", "")
    if compact_without_unknown in PHOTO_PARTIAL_FORMS:
        return True
    if compact_without_unknown in PHOTO_PHONETIC_FORMS:
        return True

    # Preserve intent when a longer request such as "帮我拍照" contains filler
    # words or one undecodable syllable.
    for first in PHOTO_PAI_SYLLABLES:
        first_pos = compact_without_unknown.find(first)
        if first_pos < 0:
            continue
        for second in PHOTO_ZAO_SYLLABLES:
            second_pos = compact_without_unknown.find(second, first_pos + 1)
            if 0 < second_pos - first_pos <= 4:
                return True
        if has_unknown and compact_without_unknown in {
            first,
            first + "我",
            "帮我" + first,
            "给我" + first,
            "请" + first,
        }:
            return True
    return False


def unique_items(items):
    out = []
    for item in items:
        if item not in out:
            out.append(item)
    return out


def load_model():
    if not MODEL_DIR.exists():
        raise FileNotFoundError(f"missing model dir: {MODEL_DIR}")
    SetLogLevel(-1)
    return Model(str(MODEL_DIR))


def make_recognizer(args, model=None, grammar=None):
    if model is None:
        model = load_model()
    if grammar:
        rec = KaldiRecognizer(model, args.sample_rate, json.dumps(grammar, ensure_ascii=False))
    else:
        rec = KaldiRecognizer(model, args.sample_rate)
    rec.SetWords(False)
    return rec


def make_wake_grammar(args):
    phrases = [
        "你好 小环",
        "您好 小环",
        "你好",
        "您好",
        "小环",
    ]
    phrases.append("[unk]")
    return phrases


def make_photo_grammar(args):
    phrases = [
        "拍照",
        "拍 照",
        "拍 张 照",
        "拍 一 张 照",
        "拍 一 张 照片",
        "拍 照片",
        "拍 一下",
        "照相",
        "帮 我 拍照",
        "帮 我 拍 张 照",
        "给 我 拍照",
        "请 拍照",
        "开始 拍照",
        "牌照",
    ]
    phonetic_phrases = [
        f"{first} {second}"
        for first in PHOTO_PAI_SYLLABLES
        for second in PHOTO_ZAO_SYLLABLES
    ]
    return unique_items(phrases + phonetic_phrases + ["[unk]"])


class SplitWakeMatcher:
    def __init__(self, window_seconds: float):
        self.window_seconds = window_seconds
        self.last_greeting_time = None
        self.last_name_time = None

    def reset(self):
        self.last_greeting_time = None
        self.last_name_time = None

    def update(self, text: str, now: float, aliases):
        compact = normalize_text(text)
        if not compact:
            return False, ""
        if is_wake_text(compact, aliases):
            self.reset()
            return True, "full"

        greeting_positions = [
            position
            for greeting in ("你好", "您好")
            if (position := compact.find(greeting)) >= 0
        ]
        has_greeting = bool(greeting_positions)
        has_name = "小环" in compact

        if has_greeting and has_name:
            name_position = compact.find("小环")
            if any(position < name_position for position in greeting_positions):
                self.reset()
                return True, "same-segment"
            self.reset()
            return False, ""

        if has_greeting and not has_name:
            self.last_greeting_time = now
        if has_name and not has_greeting:
            self.last_name_time = now

        if (
            self.last_greeting_time is not None
            and self.last_name_time is not None
            and 0 <= self.last_name_time - self.last_greeting_time <= self.window_seconds
        ):
            self.reset()
            return True, "split"

        expire_before = now - self.window_seconds
        if self.last_greeting_time is not None and self.last_greeting_time < expire_before:
            self.last_greeting_time = None
        if self.last_name_time is not None and self.last_name_time < expire_before:
            self.last_name_time = None
        return False, ""


def queue_photo_request(
    args,
    burst_index=1,
    burst_count=1,
    burst_id="",
    capture_not_before_unix_us=0,
):
    now = datetime.now()
    request_id = burst_id or f"xiaohuan_photo_{now.strftime('%Y%m%d_%H%M%S_%f')}"
    if burst_count > 1:
        request_id += f"_{burst_index:02d}of{burst_count:02d}"
    args.photo_request_dir.mkdir(parents=True, exist_ok=True)
    args.photo_result_dir.mkdir(parents=True, exist_ok=True)
    request_path = args.photo_request_dir / f"{request_id}.json"
    result_path = args.photo_result_dir / f"{request_id}.json"
    tmp_path = args.photo_request_dir / f".{request_id}.tmp"
    payload = {
        "message_type": "rgb_snapshot_request",
        "request_id": request_id,
        "sender_id": args.photo_sender_id,
        "camera_id": args.photo_camera_id,
        "result_path": str(result_path),
        "trigger": "xiaohuan_voice_photo",
        "storage_target": "receiver_nas",
        "burst_id": burst_id,
        "burst_index": burst_index,
        "burst_count": burst_count,
        "capture_not_before_unix_us": capture_not_before_unix_us,
        "requested_at_unix_us": int(time.time() * 1_000_000),
    }
    tmp_path.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
    tmp_path.replace(request_path)
    print(
        f"photo request queued: request_id={request_id} camera_id={args.photo_camera_id} "
        "storage_target=receiver_nas",
        flush=True,
    )
    return request_path, result_path


def read_photo_result(result_path: Path):
    if not result_path.exists():
        return None, {}
    try:
        payload = json.loads(result_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return None, {"error": str(exc)}
    status = payload.get("status")
    ok = bool(payload.get("ok")) or status == "ok"
    return ok, payload


def wait_photo_result(result_path: Path, timeout_seconds: float):
    deadline = time.monotonic() + timeout_seconds
    last_error = None
    while True:
        saved, payload = read_photo_result(result_path)
        if saved is not None:
            return saved, payload
        last_error = payload.get("error") or last_error
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(0.05, remaining))
    return False, {"status": "timeout", "error": last_error or "snapshot result timeout"}


def capture_photo_burst(args):
    pending_results = []
    capture_schedule_base_us = time.time_ns() // 1000
    interval_us = round(args.photo_burst_interval_seconds * 1_000_000)
    burst_id = f"xiaohuan_photo_{datetime.now().strftime('%Y%m%d_%H%M%S_%f')}"
    for burst_index in range(1, args.photo_burst_count + 1):
        capture_not_before_unix_us = capture_schedule_base_us + ((burst_index - 1) * interval_us)
        _request_path, result_path = queue_photo_request(
            args,
            burst_index=burst_index,
            burst_count=args.photo_burst_count,
            burst_id=burst_id,
            capture_not_before_unix_us=capture_not_before_unix_us,
        )
        pending_results.append((burst_index, result_path))

    pending = dict(pending_results)
    results_by_index = {}
    failures_by_index = {}
    pending_errors = {}
    deadline = time.monotonic() + args.photo_result_timeout_seconds
    while pending:
        for burst_index, result_path in list(pending.items()):
            saved, result = read_photo_result(result_path)
            if saved is None:
                if result.get("error"):
                    pending_errors[burst_index] = result["error"]
                continue
            del pending[burst_index]
            if not saved:
                failures_by_index[burst_index] = result.get(
                    "error",
                    result.get("status", "snapshot not confirmed"),
                )
                continue
            results_by_index[burst_index] = result
            print(
                f"photo burst captured index={burst_index}/{args.photo_burst_count} "
                f"path={result.get('image_path', '<unknown>')}",
                flush=True,
            )
        if not pending:
            break
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(0.05, remaining))

    for burst_index in pending:
        failures_by_index[burst_index] = pending_errors.get(
            burst_index,
            "snapshot result timeout",
        )
    results = [results_by_index[index] for index in sorted(results_by_index)]
    failures = [
        f"{index}/{args.photo_burst_count}: {failures_by_index[index]}"
        for index in sorted(failures_by_index)
    ]
    if failures:
        return False, {
            "status": "partial" if results else "error",
            "error": "; ".join(failures),
            "captured_count": len(results),
            "requested_count": args.photo_burst_count,
            "image_paths": [item.get("image_path", "") for item in results],
        }
    return True, {
        "status": "captured",
        "captured_count": len(results),
        "requested_count": args.photo_burst_count,
        "image_paths": [item.get("image_path", "") for item in results],
        "results": results,
    }


def start_photo_capture_async(args, matched_text: str, source: str):
    def worker():
        started = time.monotonic()
        with PHOTO_CAPTURE_LOCK:
            queue_wait_ms = round((time.monotonic() - started) * 1000)
            saved, result = capture_photo_burst(args)
        elapsed_ms = round((time.monotonic() - started) * 1000)
        if saved:
            print(
                f"photo capture completed source={source} text={matched_text} "
                f"queue_wait_ms={queue_wait_ms} elapsed_ms={elapsed_ms} "
                f"paths={result.get('image_paths', [])}",
                flush=True,
            )
        else:
            print(
                f"photo capture failed after immediate acknowledgement source={source} "
                f"text={matched_text} queue_wait_ms={queue_wait_ms} elapsed_ms={elapsed_ms} "
                f"status={result.get('status')} error={result.get('error', '')} "
                f"captured={result.get('captured_count', 0)}/{result.get('requested_count', 0)}",
                flush=True,
            )

    thread = threading.Thread(
        target=worker,
        name="xiaohuan-photo-capture",
        daemon=True,
    )
    thread.start()
    return thread


def cleanup_old_photo_results(args):
    cutoff = time.time() - args.photo_result_retention_seconds
    try:
        entries = list(args.photo_result_dir.glob("*.json"))
    except OSError as exc:
        print(f"photo result cleanup skipped: {exc}", flush=True)
        return
    removed = 0
    for path in entries:
        try:
            if path.stat().st_mtime < cutoff:
                path.unlink()
                removed += 1
        except OSError:
            continue
    if removed:
        print(f"photo result cleanup removed={removed}", flush=True)


def check_result(result_json: str, aliases, label: str, text_matcher=is_wake_text):
    try:
        data = json.loads(result_json)
    except json.JSONDecodeError:
        return False, ""
    text = data.get(label, "") or data.get("text", "")
    if text:
        print(f"{label}: {text}")
    return text_matcher(text, aliases), text


def pcm_rms(data: bytes):
    if not data:
        return 0.0
    samples = np.frombuffer(data, dtype="<i2").astype(np.float32) / 32768.0
    if samples.size == 0:
        return 0.0
    return float(np.sqrt(np.mean(samples * samples)))


def webrtc_voice_ratio(data: bytes, sample_rate: int, frame_ms: int, vad):
    if vad is None:
        return 0.0
    frame_bytes = int(sample_rate * frame_ms / 1000) * 2
    if frame_bytes <= 0:
        return 0.0
    frame_count = len(data) // frame_bytes
    if frame_count <= 0:
        return 0.0
    voiced = 0
    for index in range(frame_count):
        frame = data[index * frame_bytes : (index + 1) * frame_bytes]
        if vad.is_speech(frame, sample_rate):
            voiced += 1
    return voiced / frame_count


def update_noise_floor(noise_rms: float, level: float, alpha: float):
    if level <= 0:
        return noise_rms
    if noise_rms <= 0:
        return level
    return (noise_rms * (1.0 - alpha)) + (level * alpha)


def compute_vad_threshold(args, noise_rms: float):
    return max(args.vad_min_rms, noise_rms * args.vad_noise_scale, noise_rms + args.vad_noise_margin)


def wake_segment_rejection_reason(args, command_mode: bool, ended: bool, speech_seconds: float):
    if command_mode:
        return ""
    if args.wake_require_end_silence and not ended:
        return "wake_missing_end_silence"
    if args.wake_decode_max_seconds > 0 and speech_seconds > args.wake_decode_max_seconds:
        return "wake_too_long"
    return ""


def make_arecord_cmd(args):
    return [
        "arecord",
        "-q",
        "-D",
        args.record_device,
        "-f",
        "S16_LE",
        "-r",
        str(args.sample_rate),
        "-c",
        "1",
        "-t",
        "raw",
    ]


class UdpPacketGate:
    def __init__(self, remote_host: str, remote_port: int):
        self._remote_address = (socket.gethostbyname(remote_host), remote_port)
        self._input = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._input.bind(("127.0.0.1", 0))
        self._input.settimeout(0.1)
        self._output = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._enabled = threading.Event()
        self._enabled.set()
        self._stop = threading.Event()
        self._thread = None

    @property
    def local_port(self):
        return self._input.getsockname()[1]

    def start(self):
        self._thread = threading.Thread(
            target=self._run,
            name="xiaohuan-audio-stream-gate",
            daemon=True,
        )
        self._thread.start()

    def pause(self):
        self._enabled.clear()

    def resume(self):
        self._enabled.set()

    def stop(self):
        self._stop.set()
        self._input.close()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
        self._output.close()

    def _run(self):
        while not self._stop.is_set():
            try:
                packet, _source = self._input.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            if not self._enabled.is_set():
                continue
            try:
                self._output.sendto(packet, self._remote_address)
            except OSError:
                continue


class CapturePlaybackDrain:
    def __init__(self, capture_out):
        self.capture_out = capture_out
        self.bytes_drained = 0
        self.ended = False
        self.error = ""
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        self._thread = threading.Thread(
            target=self._run,
            name="xiaohuan-playback-capture-drain",
            daemon=True,
        )
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)

    def _run(self):
        try:
            descriptor = self.capture_out.fileno()
            while not self._stop.is_set():
                readable, _writable, _exceptional = select.select(
                    [descriptor],
                    [],
                    [],
                    0.05,
                )
                if not readable:
                    continue
                data = os.read(descriptor, 65536)
                if not data:
                    self.ended = True
                    break
                self.bytes_drained += len(data)
        except (OSError, ValueError) as exc:
            if not self._stop.is_set():
                self.ended = True
                self.error = str(exc)


def make_streaming_capture_cmd(args, stream_target=None):
    stream_host, stream_port = stream_target or (
        args.audio_stream_host,
        args.audio_stream_port,
    )
    return [
        "gst-launch-1.0",
        "-q",
        "alsasrc",
        f"device={args.record_device}",
        "do-timestamp=true",
        "!",
        (
            "audio/x-raw,"
            f"format=S16LE,rate={args.audio_stream_sample_rate},"
            "channels=1,layout=interleaved"
        ),
        "!",
        "tee",
        "name=audio",
        "audio.",
        "!",
        "queue",
        "max-size-time=500000000",
        "max-size-bytes=0",
        "max-size-buffers=0",
        "!",
        "audioconvert",
        "!",
        "audioresample",
        "!",
        (
            "audio/x-raw,"
            f"format=S16LE,rate={args.sample_rate},"
            "channels=1,layout=interleaved"
        ),
        "!",
        "fdsink",
        "fd=1",
        "sync=false",
        "audio.",
        "!",
        "queue",
        "leaky=downstream",
        "max-size-time=200000000",
        "max-size-bytes=0",
        "max-size-buffers=0",
        "!",
        "opusenc",
        f"bitrate={args.audio_stream_bitrate}",
        "bitrate-type=cbr",
        "audio-type=voice",
        "frame-size=20",
        "complexity=5",
        "inband-fec=true",
        "packet-loss-percentage=5",
        "perfect-timestamp=true",
        "!",
        "rtpopuspay",
        "pt=96",
        "mtu=1200",
        "!",
        "udpsink",
        f"host={stream_host}",
        f"port={stream_port}",
        "sync=false",
        "async=false",
    ]


def start_capture(args, stream_target=None):
    if args.audio_stream:
        source = subprocess.Popen(
            make_streaming_capture_cmd(args, stream_target),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        procs = [("gstreamer-audio-tee", source)]
    else:
        source = subprocess.Popen(
            make_arecord_cmd(args),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        procs = [("arecord", source)]

    filter_graph = args.audio_filter_graph or AUDIO_FILTERS[args.audio_filter]
    if not filter_graph:
        return source.stdout, procs, "none"

    ffmpeg_cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-f",
        "s16le",
        "-ar",
        str(args.sample_rate),
        "-ac",
        "1",
        "-probesize",
        "32",
        "-analyzeduration",
        "0",
        "-i",
        "pipe:0",
        "-af",
        filter_graph,
        "-f",
        "s16le",
        "-ar",
        str(args.sample_rate),
        "-ac",
        "1",
        "pipe:1",
    ]
    ffmpeg = subprocess.Popen(
        ffmpeg_cmd,
        stdin=source.stdout,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    source.stdout.close()
    procs.append(("ffmpeg", ffmpeg))
    return ffmpeg.stdout, procs, filter_graph


def read_capture_chunk(capture_out, expected_bytes: int, timeout_seconds: float):
    data = bytearray()
    deadline = time.monotonic() + timeout_seconds
    while len(data) < expected_bytes:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        readable, _writable, _exceptional = select.select(
            [capture_out.fileno()],
            [],
            [],
            remaining,
        )
        if not readable:
            break
        chunk = os.read(capture_out.fileno(), expected_bytes - len(data))
        if not chunk:
            break
        data.extend(chunk)
    return bytes(data)


def terminate_capture(procs):
    for _name, proc in reversed(procs):
        if proc.poll() is None:
            proc.terminate()
    for _name, proc in reversed(procs):
        try:
            proc.wait(timeout=1)
        except subprocess.TimeoutExpired:
            proc.kill()
            try:
                proc.wait(timeout=1)
            except subprocess.TimeoutExpired:
                pass


def capture_errors(procs):
    errors = []
    for name, proc in procs:
        if proc.stderr is None or proc.poll() is None:
            continue
        text = proc.stderr.read().decode("utf-8", errors="replace").strip()
        if text:
            errors.append(f"{name} stderr: {text}")
    return "\n".join(errors)


def close_capture_streams(capture_out, procs):
    streams = [capture_out]
    for _name, proc in procs:
        streams.extend((proc.stdin, proc.stdout, proc.stderr))
    closed = set()
    for stream in streams:
        if stream is None or id(stream) in closed:
            continue
        closed.add(id(stream))
        try:
            stream.close()
        except (OSError, ValueError):
            pass


def shutdown_capture(capture_out, procs, collect_errors=False):
    terminate_capture(procs)
    errors = capture_errors(procs) if collect_errors else ""
    close_capture_streams(capture_out, procs)
    return errors


def run_file(args):
    rec = make_recognizer(args)
    ffmpeg_cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        str(args.wav),
        "-ac",
        "1",
        "-ar",
        str(args.sample_rate),
        "-f",
        "s16le",
        "-",
    ]
    proc = subprocess.Popen(ffmpeg_cmd, stdout=subprocess.PIPE)
    aliases = [normalize_text(x) for x in args.alias]
    detected = False
    while True:
        data = proc.stdout.read(4000)
        if not data:
            break
        if rec.AcceptWaveform(data):
            ok, _ = check_result(rec.Result(), aliases, "text")
            detected = detected or ok
        else:
            ok, _ = check_result(rec.PartialResult(), aliases, "partial")
            detected = detected or ok
    ok, _ = check_result(rec.FinalResult(), aliases, "text")
    detected = detected or ok
    print("wake_detected:", "yes" if detected else "no")
    return 0 if detected else 1


def decode_segment(model, args, segment: bytes, aliases, grammar, text_matcher=is_wake_text):
    rec = make_recognizer(args, model, grammar)
    rec.AcceptWaveform(segment)
    ok, text = check_result(rec.FinalResult(), aliases, "text", text_matcher)
    return ok, text


def listen(args):
    model = load_model()
    aliases = unique_items([normalize_text(x) for x in args.alias])
    photo_aliases = unique_items([normalize_text(x) for x in args.photo_alias])
    wake_grammar = make_wake_grammar(args)
    photo_grammar = make_photo_grammar(args)
    chunk_bytes = int(args.sample_rate * args.chunk_seconds) * 2
    vad = None
    vad_mode = "rms"
    if args.webrtc_vad:
        if webrtcvad is None:
            print("webrtcvad not installed, falling back to rms vad", flush=True)
        else:
            vad = webrtcvad.Vad(args.webrtc_vad_mode)
            vad_mode = f"webrtcvad(mode={args.webrtc_vad_mode})+rms"
    if args.tts_backend == "espeak":
        tts_engine = EspeakTtsEngine()
    elif args.tts_backend == "sherpa":
        tts_engine = OfflineTtsEngine(
            model_dir=args.tts_model_dir,
            num_threads=args.tts_num_threads,
        )
    else:
        tts_engine = EdgeTtsEngine(
            voice=args.tts_edge_voice,
            timeout_seconds=args.tts_edge_timeout_seconds,
            cache_entries=args.tts_edge_cache_entries,
            disk_cache_dir=args.tts_edge_cache_dir,
            disk_cache_max_bytes=args.tts_edge_cache_max_mb * 1024 * 1024,
        )
    speech_service = UnifiedSpeechService(
        tts_engine=tts_engine,
        http_bind=args.tts_http_bind,
        http_port=args.tts_http_port,
        max_queue=args.tts_max_queue,
        max_text_chars=args.tts_max_text_chars,
        speaker_retry_seconds=args.tts_speaker_retry_seconds,
    )
    speech_service.start(enable_http=args.tts_http)
    stop = False
    stream_gate = None
    stream_target = None
    if args.audio_stream:
        try:
            stream_gate = UdpPacketGate(
                args.audio_stream_host,
                args.audio_stream_port,
            )
            stream_gate.start()
            stream_target = ("127.0.0.1", stream_gate.local_port)
        except OSError as exc:
            if stream_gate is not None:
                stream_gate.stop()
            stream_gate = None
            print(
                f"audio stream gate unavailable; using direct output: {exc}",
                flush=True,
            )

    def handle_signal(_signum, _frame):
        nonlocal stop
        stop = True
        speech_service.request_stop()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    print("listening for: 你好小环")
    print(f"aliases={aliases}")
    print(f"record_device={args.record_device}, playback_device={args.playback_device}")
    print(f"mode=gated-vad-asr, vad={vad_mode}")
    print(
        f"tts_http={args.tts_http} bind={args.tts_http_bind}:{args.tts_http_port} "
        f"backend={args.tts_backend} model={args.tts_model_dir} "
        f"queue_capacity={args.tts_max_queue} "
        f"max_text_chars={args.tts_max_text_chars}"
    )
    print(f"audio_stream={args.audio_stream}")
    if args.audio_stream:
        print(
            f"audio_stream_target={args.audio_stream_host}:{args.audio_stream_port} "
            f"codec=opus rate={args.audio_stream_sample_rate} channels=1 "
            f"bitrate={args.audio_stream_bitrate} transport=rtp/udp"
        )
        if stream_gate is not None:
            print(
                f"audio_stream_gate=127.0.0.1:{stream_gate.local_port} "
                "playback_policy=drop",
                flush=True,
            )
    print(f"wake_grammar={wake_grammar}")
    print(f"photo_aliases={photo_aliases}")
    print(f"photo_grammar={photo_grammar}")
    print(f"photo_request_dir={args.photo_request_dir}")
    print(f"photo_result_dir={args.photo_result_dir}")
    print(f"photo_result_timeout_seconds={args.photo_result_timeout_seconds}")
    print(f"photo_result_retention_seconds={args.photo_result_retention_seconds}")
    print(f"photo_burst_count={args.photo_burst_count}")
    print(f"photo_burst_interval_seconds={args.photo_burst_interval_seconds}")
    print("photo_storage_target=receiver_nas")
    print(f"zero_audio_rms={args.zero_audio_rms}")
    print(f"zero_audio_restart_seconds={args.zero_audio_restart_seconds}")
    print(f"audio_read_timeout_seconds={args.audio_read_timeout_seconds}")
    print(f"audio_recovery_seconds={args.audio_recovery_seconds}")
    print(f"audio_recovery_interval_seconds={args.audio_recovery_interval_seconds}")
    print(f"capture_playback_mode={args.capture_playback_mode}")
    print(f"barge_in={args.barge_in}")
    print(
        f"barge_in_min_rms={args.barge_in_min_rms}, "
        f"barge_in_voice_ratio={args.barge_in_voice_ratio}, "
        f"barge_in_min_chunks={args.barge_in_min_chunks}"
    )
    cleanup_old_photo_results(args)

    calibration_chunks = max(1, int(args.vad_calibration_seconds / args.chunk_seconds))

    def open_capture(
        phase: str,
        *,
        calibrate: bool,
        previous_noise_rms: float = 0.0,
    ):
        recovery_started = time.monotonic()
        attempt = 0
        while not stop:
            attempt += 1
            capture_out = None
            capture_procs = []
            try:
                capture_out, capture_procs, audio_filter = start_capture(
                    args,
                    stream_target,
                )
                noise_levels = []
                sample_count = calibration_chunks if calibrate else 1
                for _ in range(sample_count):
                    data = read_capture_chunk(
                        capture_out,
                        chunk_bytes,
                        args.audio_read_timeout_seconds,
                    )
                    if len(data) < chunk_bytes:
                        raise RuntimeError(
                            f"short audio read got={len(data)} expected={chunk_bytes}"
                        )
                    if calibrate:
                        noise_levels.append(pcm_rms(data))
            except (OSError, RuntimeError) as exc:
                err = shutdown_capture(
                    capture_out,
                    capture_procs,
                    collect_errors=True,
                )
                elapsed = time.monotonic() - recovery_started
                print(
                    f"audio capture unavailable phase={phase} attempt={attempt} "
                    f"elapsed={elapsed:.2f}s error={exc}",
                    flush=True,
                )
                if err.strip():
                    print(err.strip(), flush=True)
                if (
                    args.audio_recovery_seconds > 0
                    and elapsed >= args.audio_recovery_seconds
                ):
                    print(
                        f"audio recovery exhausted phase={phase} "
                        f"after={elapsed:.2f}s",
                        flush=True,
                    )
                    return None
                retry_until = time.monotonic() + args.audio_recovery_interval_seconds
                while not stop and time.monotonic() < retry_until:
                    time.sleep(max(0.0, min(0.05, retry_until - time.monotonic())))
                continue

            noise_rms = (
                float(np.median(noise_levels))
                if calibrate
                else previous_noise_rms
            )
            if phase != "startup" or attempt > 1:
                elapsed = time.monotonic() - recovery_started
                print(
                    f"audio capture recovered phase={phase} attempt={attempt} "
                    f"elapsed={elapsed:.2f}s calibrated={str(calibrate).lower()}",
                    flush=True,
                )
            print(f"audio_filter={audio_filter}")
            return capture_out, capture_procs, audio_filter, noise_rms
        return None

    capture_state = open_capture("startup", calibrate=True)
    if capture_state is None:
        if stream_gate is not None:
            stream_gate.stop()
        speech_service.stop()
        return 12
    capture_out, capture_procs, audio_filter, noise_rms = capture_state
    vad_threshold = compute_vad_threshold(args, noise_rms)
    print(f"noise_rms={noise_rms:.5f}, vad_threshold={vad_threshold:.5f}")

    last_trigger = 0.0
    ignore_until = 0.0
    command_listen_until = 0.0
    last_noise_log = time.time()
    matcher = SplitWakeMatcher(args.split_wake_window_seconds)
    pre_roll = collections.deque(maxlen=max(1, int(args.pre_roll_seconds / args.chunk_seconds)))
    speech_chunks = []
    speech_voice_ratios = []
    photo_capture_threads = []
    in_speech = False
    silence_chunks = 0
    skipped_segments = 0
    zero_audio_since = None
    last_skip_log = time.time()
    end_silence_chunks = max(1, int(args.end_silence_seconds / args.chunk_seconds))
    photo_end_silence_chunks = max(1, int(args.photo_end_silence_seconds / args.chunk_seconds))

    def acknowledge_and_capture_photo(text: str, source: str, detected_at: float):
        playback = speech_service.enqueue_wav(
            args.photo_response_wav,
            source="photo-acknowledgement",
        )
        print(
            f"photo command acknowledged immediately source={source} text={text} "
            f"feedback_started={playback is not None}",
            flush=True,
        )
        photo_capture_threads.append(start_photo_capture_async(args, text, source))

    try:
        while not stop:
            photo_capture_threads[:] = [thread for thread in photo_capture_threads if thread.is_alive()]

            playback_task = speech_service.next_ready_task()
            if playback_task is not None:
                playback_started = time.monotonic()
                if stream_gate is not None:
                    stream_gate.pause()
                capture_drain = None
                if args.capture_playback_mode == "keep":
                    capture_drain = CapturePlaybackDrain(capture_out)
                    capture_drain.start()
                else:
                    shutdown_capture(capture_out, capture_procs)
                    capture_out = None
                    capture_procs = []
                    print(
                        "audio capture paused for half-duplex playback",
                        flush=True,
                    )
                opens_command_window = False
                while playback_task is not None and not stop:
                    playback_ok = speech_service.play_task(
                        playback_task,
                        args.playback_device,
                    )
                    opens_command_window = (
                        opens_command_window or playback_task.opens_command_window
                    )
                    speech_service.complete_task(playback_task, playback_ok)
                    queue_wait_seconds = (
                        0.0
                        if opens_command_window
                        else args.tts_resume_delay_seconds
                    )
                    playback_task = speech_service.next_ready_task(
                        timeout=queue_wait_seconds,
                    )
                echo_deadline = time.monotonic() + args.echo_tail_seconds
                while not stop and time.monotonic() < echo_deadline:
                    time.sleep(min(0.01, echo_deadline - time.monotonic()))
                if stop:
                    if capture_drain is not None:
                        capture_drain.stop()
                    break

                if args.capture_playback_mode == "restart":
                    capture_state = open_capture(
                        "post-playback-resume",
                        calibrate=False,
                        previous_noise_rms=noise_rms,
                    )
                    if capture_state is None:
                        return 12
                    capture_out, capture_procs, audio_filter, noise_rms = capture_state
                    print(
                        "audio capture resumed after half-duplex playback: "
                        f"elapsed={time.monotonic() - playback_started:.3f}s",
                        flush=True,
                    )
                else:
                    capture_drain.stop()
                    capture_failed = capture_drain.ended or any(
                        proc.poll() is not None
                        for _name, proc in capture_procs
                    )
                    if capture_failed:
                        err = shutdown_capture(
                            capture_out,
                            capture_procs,
                            collect_errors=True,
                        )
                        print(
                            "audio capture lost during playback: "
                            f"error={capture_drain.error or 'capture process exited'}",
                            flush=True,
                        )
                        if err.strip():
                            print(err.strip(), flush=True)
                        capture_state = open_capture(
                            "post-playback-failure",
                            calibrate=True,
                        )
                        if capture_state is None:
                            return 12
                        capture_out, capture_procs, audio_filter, noise_rms = capture_state
                        vad_threshold = compute_vad_threshold(args, noise_rms)
                    else:
                        print(
                            "audio capture kept alive across playback: "
                            f"elapsed={time.monotonic() - playback_started:.3f}s "
                            f"drained_bytes={capture_drain.bytes_drained} "
                            f"echo_tail={args.echo_tail_seconds:.3f}s",
                            flush=True,
                        )
                if stream_gate is not None:
                    stream_gate.resume()
                now = time.time()
                ignore_until = now
                if opens_command_window:
                    command_listen_until = now + args.post_wake_command_seconds
                    print(
                        f"photo command window opens after unified playback: "
                        f"window={args.post_wake_command_seconds:.2f}s",
                        flush=True,
                    )
                matcher.reset()
                pre_roll.clear()
                speech_chunks = []
                speech_voice_ratios = []
                in_speech = False
                silence_chunks = 0
                zero_audio_since = None
                continue

            data = read_capture_chunk(
                capture_out,
                chunk_bytes,
                args.audio_read_timeout_seconds,
            )
            if len(data) < chunk_bytes:
                err = shutdown_capture(
                    capture_out,
                    capture_procs,
                    collect_errors=True,
                )
                print(f"audio stream ended: got={len(data)} expected={chunk_bytes}", flush=True)
                if err.strip():
                    print(err.strip(), flush=True)
                capture_state = open_capture("runtime", calibrate=True)
                if capture_state is None:
                    return 12
                capture_out, capture_procs, audio_filter, noise_rms = capture_state
                vad_threshold = compute_vad_threshold(args, noise_rms)
                matcher.reset()
                pre_roll.clear()
                speech_chunks = []
                speech_voice_ratios = []
                in_speech = False
                silence_chunks = 0
                zero_audio_since = None
                continue
            now = time.time()

            level = pcm_rms(data)
            if args.zero_audio_restart_seconds > 0:
                if level <= args.zero_audio_rms:
                    if zero_audio_since is None:
                        zero_audio_since = now
                    elif now - zero_audio_since >= args.zero_audio_restart_seconds:
                        shutdown_capture(capture_out, capture_procs)
                        print(
                            f"zero audio watchdog triggered: level={level:.6f} "
                            f"threshold={args.zero_audio_rms:.6f} "
                            f"seconds={now - zero_audio_since:.1f}",
                            flush=True,
                        )
                        return 12
                else:
                    zero_audio_since = None

            voice_ratio = webrtc_voice_ratio(data, args.sample_rate, args.webrtc_frame_ms, vad)
            voice_energy_ok = level >= noise_rms + args.webrtc_voice_energy_margin
            voice_active = vad is not None and voice_ratio >= args.webrtc_voice_ratio and voice_energy_ok
            energy_active = level >= vad_threshold
            if vad is not None:
                energy_active = energy_active and level >= noise_rms * args.energy_fallback_scale
            active = voice_active or energy_active

            if now < ignore_until:
                if command_listen_until > now:
                    pre_roll.append(data)
                else:
                    pre_roll.clear()
                speech_chunks = []
                speech_voice_ratios = []
                in_speech = False
                silence_chunks = 0
                continue

            if not in_speech:
                if active:
                    in_speech = True
                    speech_chunks = list(pre_roll)
                    speech_chunks.append(data)
                    speech_voice_ratios = [voice_ratio]
                    silence_chunks = 0
                else:
                    noise_rms = update_noise_floor(noise_rms, level, args.noise_update_alpha)
                    vad_threshold = compute_vad_threshold(args, noise_rms)
                    if now - last_noise_log >= args.noise_log_seconds:
                        print(
                            f"noise_rms={noise_rms:.5f}, vad_threshold={vad_threshold:.5f}, "
                            f"last_rms={level:.5f}, voice_ratio={voice_ratio:.2f}",
                            flush=True,
                        )
                        last_noise_log = now
                    pre_roll.append(data)
                continue

            speech_chunks.append(data)
            speech_voice_ratios.append(voice_ratio)
            if active:
                silence_chunks = 0
            else:
                silence_chunks += 1

            speech_seconds = len(speech_chunks) * args.chunk_seconds
            command_mode = now <= command_listen_until
            required_end_silence_chunks = photo_end_silence_chunks if command_mode else end_silence_chunks
            ended = silence_chunks >= required_end_silence_chunks and speech_seconds >= args.min_speech_seconds
            too_long = speech_seconds >= args.max_speech_seconds
            if not ended and not too_long:
                continue

            segment = b"".join(speech_chunks)
            segment_rms = pcm_rms(segment)
            segment_voice_ratio = max(speech_voice_ratios) if speech_voice_ratios else 0.0
            decode_min_rms = args.photo_decode_min_rms if command_mode else args.decode_min_rms
            decode_rms_threshold = max(decode_min_rms, noise_rms + args.decode_noise_margin)
            too_weak = segment_rms < decode_rms_threshold
            decode_min_seconds = args.photo_decode_min_seconds if command_mode else args.decode_min_seconds
            too_short = speech_seconds < decode_min_seconds
            wake_rejection = wake_segment_rejection_reason(
                args,
                command_mode,
                ended,
                speech_seconds,
            )
            if too_weak or too_short or wake_rejection:
                skipped_segments += 1
                if now - last_skip_log >= args.skip_log_seconds:
                    if wake_rejection:
                        skip_reason = wake_rejection
                    elif too_weak:
                        skip_reason = "low_rms"
                    else:
                        skip_reason = "too_short"
                    print(
                        f"skipped low_quality segments={skipped_segments} "
                        f"last_reason={skip_reason} "
                        f"last_seconds={speech_seconds:.2f} last_rms={segment_rms:.5f} "
                        f"decode_rms_threshold={decode_rms_threshold:.5f} "
                        f"last_voice_ratio={segment_voice_ratio:.2f}",
                        flush=True,
                    )
                    skipped_segments = 0
                    last_skip_log = now
                pre_roll.clear()
                speech_chunks = []
                speech_voice_ratios = []
                in_speech = False
                silence_chunks = 0
                continue
            decode_aliases = photo_aliases if command_mode else aliases
            decode_grammar = photo_grammar if command_mode else wake_grammar
            ok, text = decode_segment(
                model,
                args,
                segment,
                decode_aliases,
                decode_grammar,
                is_photo_text if command_mode else is_wake_text,
            )
            if command_mode:
                match_reason = "photo-command" if ok else ""
            elif args.allow_split_wake:
                split_ok, match_reason = matcher.update(text, now, aliases)
                ok = ok or split_ok
            else:
                match_reason = "full" if ok else ""
            print(
                f"segment seconds={speech_seconds:.2f} rms={segment_rms:.5f} "
                f"voice_ratio={segment_voice_ratio:.2f} mode={'photo' if command_mode else 'wake'} "
                f"text={text or '<empty>'}",
                flush=True,
            )
            if command_mode:
                if ok:
                    acknowledge_and_capture_photo(text, "command-window", now)
                    command_listen_until = 0.0
                    matcher.reset()
            elif ok and now - last_trigger >= args.cooldown_seconds:
                print(f"wake matched: {text} ({match_reason or 'full'}) -> 我在", flush=True)
                playback = speech_service.enqueue_wav(
                    args.response_wav,
                    source="wake-response",
                    opens_command_window=True,
                )
                if playback is not None:
                    command_listen_until = 0.0
                else:
                    command_listen_until = now + args.post_wake_command_seconds
                last_trigger = now
                ignore_until = now + args.playback_ignore_seconds
                matcher.reset()
            pre_roll.clear()
            speech_chunks = []
            speech_voice_ratios = []
            in_speech = False
            silence_chunks = 0
    finally:
        for thread in photo_capture_threads:
            thread.join(timeout=0.2)
        shutdown_capture(capture_out, capture_procs)
        if stream_gate is not None:
            stream_gate.stop()
        speech_service.stop()


def nonnegative_float(value):
    parsed = float(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("value must be non-negative")
    return parsed


def positive_float(value):
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return parsed


def udp_port(value):
    parsed = int(value)
    if not 1 <= parsed <= 65535:
        raise argparse.ArgumentTypeError("port must be between 1 and 65535")
    return parsed


def opus_bitrate(value):
    parsed = int(value)
    if not 6000 <= parsed <= 510000:
        raise argparse.ArgumentTypeError("Opus bitrate must be between 6000 and 510000")
    return parsed


def build_parser():
    parser = argparse.ArgumentParser(description="Vosk ASR wake word prototype for 你好小环")
    parser.add_argument("--sample-rate", type=int, default=16000)
    parser.add_argument(
        "--alias",
        action="append",
        default=["你好小环", "你好 小环", "您好小环", "您好 小环"],
    )

    sub = parser.add_subparsers(dest="command", required=True)

    p_file = sub.add_parser("file")
    p_file.add_argument("wav", type=Path)
    p_file.set_defaults(func=run_file)

    p_listen = sub.add_parser("listen")
    p_listen.add_argument("--record-device", default="plughw:4,0")
    p_listen.add_argument("--playback-device", default="plughw:3,0")
    p_listen.add_argument(
        "--tts-http",
        action=argparse.BooleanOptionalAction,
        default=True,
    )
    p_listen.add_argument("--tts-http-bind", default="0.0.0.0")
    p_listen.add_argument("--tts-http-port", type=udp_port, default=18082)
    p_listen.add_argument(
        "--tts-backend",
        choices=["espeak", "sherpa", "edge"],
        default="edge",
    )
    p_listen.add_argument("--tts-model-dir", type=Path, default=DEFAULT_TTS_MODEL_DIR)
    p_listen.add_argument("--tts-num-threads", type=int, choices=range(1, 9), default=4)
    p_listen.add_argument(
        "--tts-edge-voice",
        default="zh-CN-XiaoyiNeural",
    )
    p_listen.add_argument(
        "--tts-edge-timeout-seconds",
        type=positive_float,
        default=4.0,
    )
    p_listen.add_argument(
        "--tts-edge-cache-entries",
        type=int,
        choices=range(0, 257),
        default=64,
    )
    p_listen.add_argument(
        "--tts-edge-cache-dir",
        type=Path,
        default=DEFAULT_EDGE_TTS_CACHE_DIR,
    )
    p_listen.add_argument(
        "--tts-edge-cache-max-mb",
        type=int,
        choices=range(1, 4097),
        default=256,
    )
    p_listen.add_argument("--tts-max-queue", type=int, choices=range(1, 101), default=100)
    p_listen.add_argument("--tts-max-text-chars", type=int, choices=range(1, 501), default=500)
    p_listen.add_argument("--tts-speaker-retry-seconds", type=nonnegative_float, default=15.0)
    p_listen.add_argument("--tts-resume-delay-seconds", type=nonnegative_float, default=0.2)
    p_listen.add_argument("--response-wav", type=Path, default=DEFAULT_RESPONSE)
    p_listen.add_argument("--photo-response-wav", type=Path, default=DEFAULT_PHOTO_RESPONSE)
    p_listen.add_argument(
        "--photo-alias",
        action="append",
        default=[
            "拍照",
            "拍 照",
            "拍张照",
            "拍 张 照",
            "拍一张照",
            "拍一张照片",
            "拍照片",
            "拍一下",
            "帮我拍照",
            "给我拍照",
            "请拍照",
            "照相",
            "牌照",
        ],
    )
    p_listen.add_argument("--post-wake-command-seconds", type=float, default=8.0)
    p_listen.add_argument("--photo-request-dir", type=Path, default=DEFAULT_PHOTO_REQUEST_DIR)
    p_listen.add_argument("--photo-result-dir", type=Path, default=DEFAULT_PHOTO_RESULT_DIR)
    p_listen.add_argument("--photo-result-timeout-seconds", type=float, default=30.0)
    p_listen.add_argument("--photo-result-retention-seconds", type=float, default=86400.0)
    p_listen.add_argument("--photo-burst-count", type=int, choices=range(1, 11), default=3)
    p_listen.add_argument("--photo-burst-interval-seconds", type=nonnegative_float, default=0.2)
    p_listen.add_argument("--photo-output-root", type=Path, default=DEFAULT_PHOTO_OUTPUT_ROOT)
    p_listen.add_argument("--photo-sender-id", default="*")
    p_listen.add_argument("--photo-camera-id", default="cam01")
    p_listen.add_argument("--audio-filter", choices=sorted(AUDIO_FILTERS), default="voice")
    p_listen.add_argument("--audio-filter-graph", default="")
    p_listen.add_argument("--audio-stream", action=argparse.BooleanOptionalAction, default=False)
    p_listen.add_argument("--audio-stream-host", default="127.0.0.1")
    p_listen.add_argument("--audio-stream-port", type=udp_port, default=50020)
    p_listen.add_argument("--audio-stream-sample-rate", type=int, choices=[48000], default=48000)
    p_listen.add_argument(
        "--audio-stream-bitrate",
        type=opus_bitrate,
        default=64000,
    )
    p_listen.add_argument("--chunk-seconds", type=float, default=0.10)
    p_listen.add_argument("--cooldown-seconds", type=float, default=2.5)
    p_listen.add_argument("--playback-ignore-seconds", type=float, default=0.3)
    p_listen.add_argument("--echo-tail-seconds", type=nonnegative_float, default=0.03)
    p_listen.add_argument("--vad-calibration-seconds", type=float, default=1.5)
    p_listen.add_argument("--vad-noise-scale", type=float, default=1.25)
    p_listen.add_argument("--vad-noise-margin", type=float, default=0.003)
    p_listen.add_argument("--vad-min-rms", type=float, default=0.008)
    p_listen.add_argument("--noise-update-alpha", type=float, default=0.03)
    p_listen.add_argument("--noise-log-seconds", type=float, default=20.0)
    p_listen.add_argument("--zero-audio-rms", type=float, default=0.0005)
    p_listen.add_argument("--zero-audio-restart-seconds", type=float, default=12.0)
    p_listen.add_argument("--audio-read-timeout-seconds", type=positive_float, default=2.0)
    p_listen.add_argument("--audio-recovery-seconds", type=nonnegative_float, default=20.0)
    p_listen.add_argument(
        "--audio-recovery-interval-seconds",
        type=positive_float,
        default=0.5,
    )
    p_listen.add_argument(
        "--capture-playback-mode",
        choices=["keep", "restart"],
        default="keep",
    )
    p_listen.add_argument("--barge-in", action=argparse.BooleanOptionalAction, default=False)
    p_listen.add_argument("--barge-in-ignore-seconds", type=float, default=0.45)
    p_listen.add_argument("--barge-in-min-rms", type=float, default=0.035)
    p_listen.add_argument("--barge-in-noise-margin", type=float, default=0.020)
    p_listen.add_argument("--barge-in-noise-scale", type=float, default=4.0)
    p_listen.add_argument("--barge-in-voice-ratio", type=float, default=0.70)
    p_listen.add_argument("--barge-in-min-chunks", type=int, default=3)
    p_listen.add_argument("--webrtc-vad", action=argparse.BooleanOptionalAction, default=True)
    p_listen.add_argument("--webrtc-vad-mode", type=int, choices=[0, 1, 2, 3], default=2)
    p_listen.add_argument("--webrtc-frame-ms", type=int, choices=[10, 20, 30], default=20)
    p_listen.add_argument("--webrtc-voice-ratio", type=float, default=0.55)
    p_listen.add_argument("--webrtc-voice-energy-margin", type=float, default=0.003)
    p_listen.add_argument("--energy-fallback-scale", type=float, default=1.35)
    p_listen.add_argument("--decode-min-seconds", type=float, default=0.70)
    p_listen.add_argument("--decode-min-rms", type=float, default=0.024)
    p_listen.add_argument("--photo-decode-min-seconds", type=float, default=0.45)
    p_listen.add_argument("--photo-decode-min-rms", type=float, default=0.016)
    p_listen.add_argument("--decode-noise-margin", type=float, default=0.008)
    p_listen.add_argument("--skip-log-seconds", type=float, default=20.0)
    p_listen.add_argument("--allow-split-wake", action=argparse.BooleanOptionalAction, default=False)
    p_listen.add_argument("--split-wake-window-seconds", type=float, default=2.4)
    p_listen.add_argument("--wake-require-end-silence", action=argparse.BooleanOptionalAction, default=True)
    p_listen.add_argument("--wake-decode-max-seconds", type=nonnegative_float, default=3.2)
    p_listen.add_argument("--pre-roll-seconds", type=float, default=0.30)
    p_listen.add_argument("--min-speech-seconds", type=float, default=0.45)
    p_listen.add_argument("--end-silence-seconds", type=float, default=0.55)
    p_listen.add_argument("--photo-end-silence-seconds", type=float, default=0.25)
    p_listen.add_argument("--max-speech-seconds", type=float, default=4.0)
    p_listen.set_defaults(func=listen)
    return parser


def main():
    args = build_parser().parse_args()
    result = args.func(args)
    if isinstance(result, int):
        raise SystemExit(result)


if __name__ == "__main__":
    main()
