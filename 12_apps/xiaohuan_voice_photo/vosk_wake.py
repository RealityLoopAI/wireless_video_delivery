#!/usr/bin/env python3
import argparse
import collections
from datetime import datetime
import json
import signal
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from vosk import KaldiRecognizer, Model, SetLogLevel

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

AUDIO_FILTERS = {
    "none": "",
    "voice-lite": "highpass=f=80,lowpass=f=7600",
    "voice": "highpass=f=80,lowpass=f=7600,afftdn=nf=-30",
    "voice-agc": "highpass=f=80,lowpass=f=7600,afftdn=nf=-30,dynaudnorm=f=75:g=8:p=0.9",
}


def normalize_text(text: str):
    return "".join(ch for ch in text if not ch.isspace())


def is_wake_text(text: str, aliases):
    text = normalize_text(text)
    if not text:
        return False
    return any(alias in text for alias in aliases)


def is_photo_text(text: str, aliases):
    text = normalize_text(text)
    if not text:
        return False
    return any(alias in text for alias in aliases)


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
        "照相",
    ]
    return unique_items(phrases + ["[unk]"])


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

        has_greeting = "你好" in compact or "您好" in compact
        has_name = "小环" in compact

        if has_greeting and has_name:
            self.reset()
            return True, "same-segment"

        if has_greeting:
            self.last_greeting_time = now
        if has_name:
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


def play_response(path: Path, device: str):
    if not path.exists():
        print(f"response wav not found: {path}", file=sys.stderr)
        return None
    return subprocess.Popen(
        ["aplay", "-q", "-D", device, str(path)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def queue_photo_request(args):
    now = datetime.now()
    request_id = f"xiaohuan_photo_{now.strftime('%Y%m%d_%H%M%S_%f')}"
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


def wait_photo_result(result_path: Path, timeout_seconds: float):
    deadline = time.time() + timeout_seconds
    last_error = None
    while time.time() < deadline:
        if result_path.exists():
            try:
                payload = json.loads(result_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as exc:
                last_error = str(exc)
                time.sleep(0.05)
                continue
            status = payload.get("status")
            ok = bool(payload.get("ok")) or status == "ok"
            if ok:
                return True, payload
            return False, payload
        time.sleep(0.05)
    return False, {"status": "timeout", "error": last_error or "snapshot result timeout"}


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


def check_result(result_json: str, aliases, label: str):
    try:
        data = json.loads(result_json)
    except json.JSONDecodeError:
        return False, ""
    text = data.get(label, "") or data.get("text", "")
    if text:
        print(f"{label}: {text}")
    return is_wake_text(text, aliases), text


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


def start_capture(args):
    arecord = subprocess.Popen(
        make_arecord_cmd(args),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    procs = [("arecord", arecord)]
    filter_graph = args.audio_filter_graph or AUDIO_FILTERS[args.audio_filter]
    if not filter_graph:
        return arecord.stdout, procs, "none"

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
        stdin=arecord.stdout,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    arecord.stdout.close()
    procs.append(("ffmpeg", ffmpeg))
    return ffmpeg.stdout, procs, filter_graph


def terminate_capture(procs):
    for _name, proc in reversed(procs):
        if proc.poll() is None:
            proc.terminate()
    for _name, proc in reversed(procs):
        try:
            proc.wait(timeout=1)
        except subprocess.TimeoutExpired:
            proc.kill()


def capture_errors(procs):
    errors = []
    for name, proc in procs:
        if proc.stderr is None or proc.poll() is None:
            continue
        text = proc.stderr.read().decode("utf-8", errors="replace").strip()
        if text:
            errors.append(f"{name} stderr: {text}")
    return "\n".join(errors)


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


def decode_segment(model, args, segment: bytes, aliases, grammar):
    rec = make_recognizer(args, model, grammar)
    rec.AcceptWaveform(segment)
    ok, text = check_result(rec.FinalResult(), aliases, "text")
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
    capture_out, capture_procs, audio_filter = start_capture(args)
    stop = False

    def handle_signal(_signum, _frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    print("listening for: 你好小环")
    print(f"aliases={aliases}")
    print(f"record_device={args.record_device}, playback_device={args.playback_device}")
    print(f"mode=gated-vad-asr, vad={vad_mode}")
    print(f"audio_filter={audio_filter}")
    print(f"wake_grammar={wake_grammar}")
    print(f"photo_aliases={photo_aliases}")
    print(f"photo_grammar={photo_grammar}")
    print(f"photo_request_dir={args.photo_request_dir}")
    print(f"photo_result_dir={args.photo_result_dir}")
    print(f"photo_result_timeout_seconds={args.photo_result_timeout_seconds}")
    print(f"photo_result_retention_seconds={args.photo_result_retention_seconds}")
    print("photo_storage_target=receiver_nas")
    print(f"zero_audio_rms={args.zero_audio_rms}")
    print(f"zero_audio_restart_seconds={args.zero_audio_restart_seconds}")
    print(f"barge_in={args.barge_in}")
    print(
        f"barge_in_min_rms={args.barge_in_min_rms}, "
        f"barge_in_voice_ratio={args.barge_in_voice_ratio}, "
        f"barge_in_min_chunks={args.barge_in_min_chunks}"
    )
    cleanup_old_photo_results(args)

    calibration_chunks = max(1, int(args.vad_calibration_seconds / args.chunk_seconds))
    noise_levels = []
    for _ in range(calibration_chunks):
        data = capture_out.read(chunk_bytes)
        if len(data) < chunk_bytes:
            terminate_capture(capture_procs)
            err = capture_errors(capture_procs)
            print(f"audio stream ended during calibration: got={len(data)} expected={chunk_bytes}", flush=True)
            if err.strip():
                print(err.strip(), flush=True)
            return
        noise_levels.append(pcm_rms(data))
    noise_rms = float(np.median(noise_levels)) if noise_levels else 0.0
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
    playback_speech_chunks = []
    playback_speech_voice_ratios = []
    playbacks = []
    in_speech = False
    playback_in_speech = False
    silence_chunks = 0
    playback_silence_chunks = 0
    skipped_segments = 0
    zero_audio_since = None
    last_skip_log = time.time()
    end_silence_chunks = max(1, int(args.end_silence_seconds / args.chunk_seconds))
    try:
        while not stop:
            data = capture_out.read(chunk_bytes)
            if len(data) < chunk_bytes:
                terminate_capture(capture_procs)
                err = capture_errors(capture_procs)
                print(f"audio stream ended: got={len(data)} expected={chunk_bytes}", flush=True)
                if err.strip():
                    print(err.strip(), flush=True)
                break
            now = time.time()

            level = pcm_rms(data)
            if args.zero_audio_restart_seconds > 0:
                if level <= args.zero_audio_rms:
                    if zero_audio_since is None:
                        zero_audio_since = now
                    elif now - zero_audio_since >= args.zero_audio_restart_seconds:
                        terminate_capture(capture_procs)
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

            active_playbacks = []
            for playback, opens_command_window, started_at in playbacks:
                if playback.poll() is None:
                    active_playbacks.append((playback, opens_command_window, started_at))
                else:
                    ignore_until = max(ignore_until, now + args.echo_tail_seconds)
                    if opens_command_window:
                        command_listen_until = max(command_listen_until, now + args.echo_tail_seconds + args.post_wake_command_seconds)
                        print(
                            f"photo command window opens after playback: "
                            f"tail={args.echo_tail_seconds:.2f}s window={args.post_wake_command_seconds:.2f}s",
                            flush=True,
                        )
            playbacks = active_playbacks

            if playbacks:
                opens_command_window = any(opens for _playback, opens, _started_at in playbacks)
                playback_started_at = min(started_at for _playback, _opens, started_at in playbacks)
                barge_threshold = max(
                    args.barge_in_min_rms,
                    noise_rms + args.barge_in_noise_margin,
                    noise_rms * args.barge_in_noise_scale,
                )
                barge_active = (
                    args.barge_in
                    and opens_command_window
                    and now - playback_started_at >= args.barge_in_ignore_seconds
                    and level >= barge_threshold
                    and (vad is None or voice_ratio >= args.barge_in_voice_ratio)
                )

                if not playback_in_speech:
                    if barge_active:
                        playback_in_speech = True
                        playback_speech_chunks = [data]
                        playback_speech_voice_ratios = [voice_ratio]
                        playback_silence_chunks = 0
                    else:
                        playback_speech_chunks = []
                        playback_speech_voice_ratios = []
                        playback_silence_chunks = 0
                    pre_roll.clear()
                    speech_chunks = []
                    speech_voice_ratios = []
                    in_speech = False
                    silence_chunks = 0
                    continue

                playback_speech_chunks.append(data)
                playback_speech_voice_ratios.append(voice_ratio)
                if barge_active:
                    playback_silence_chunks = 0
                else:
                    playback_silence_chunks += 1

                playback_speech_seconds = len(playback_speech_chunks) * args.chunk_seconds
                playback_ended = (
                    playback_silence_chunks >= end_silence_chunks
                    and playback_speech_seconds >= args.min_speech_seconds
                )
                playback_too_long = playback_speech_seconds >= args.max_speech_seconds
                if not playback_ended and not playback_too_long:
                    pre_roll.clear()
                    speech_chunks = []
                    speech_voice_ratios = []
                    in_speech = False
                    silence_chunks = 0
                    continue

                segment = b"".join(playback_speech_chunks)
                segment_rms = pcm_rms(segment)
                segment_voice_ratio = max(playback_speech_voice_ratios) if playback_speech_voice_ratios else 0.0
                too_short = len(playback_speech_chunks) < args.barge_in_min_chunks or playback_speech_seconds < args.decode_min_seconds
                ok = False
                text = ""
                if not too_short and opens_command_window:
                    ok, text = decode_segment(model, args, segment, photo_aliases, photo_grammar)
                print(
                    f"playback command candidate seconds={playback_speech_seconds:.2f} "
                    f"rms={segment_rms:.5f} threshold={barge_threshold:.5f} "
                    f"voice_ratio={segment_voice_ratio:.2f} text={text or '<empty>'} matched={ok}",
                    flush=True,
                )
                playback_speech_chunks = []
                playback_speech_voice_ratios = []
                playback_in_speech = False
                playback_silence_chunks = 0

                if ok:
                    for playback, _opens, _started_at in playbacks:
                        if playback.poll() is None:
                            playback.terminate()
                    playbacks = []
                    command_listen_until = 0.0
                    matcher.reset()
                    _request_path, result_path = queue_photo_request(args)
                    saved, result = wait_photo_result(result_path, args.photo_result_timeout_seconds)
                    if saved:
                        print(
                            f"photo command matched during playback: {text} -> "
                            f"saved path={result.get('image_path', '<unknown>')}",
                            flush=True,
                        )
                        playback = play_response(args.photo_response_wav, args.playback_device)
                        if playback is not None:
                            playbacks.append((playback, False, now))
                    else:
                        print(
                            f"photo command during playback matched but snapshot not confirmed: "
                            f"status={result.get('status')} error={result.get('error', '')}",
                            flush=True,
                        )

                pre_roll.clear()
                speech_chunks = []
                speech_voice_ratios = []
                in_speech = False
                silence_chunks = 0
                continue

            playback_speech_chunks = []
            playback_speech_voice_ratios = []
            playback_in_speech = False
            playback_silence_chunks = 0

            if now < ignore_until:
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
            ended = silence_chunks >= end_silence_chunks and speech_seconds >= args.min_speech_seconds
            too_long = speech_seconds >= args.max_speech_seconds
            if not ended and not too_long:
                continue

            segment = b"".join(speech_chunks)
            segment_rms = pcm_rms(segment)
            segment_voice_ratio = max(speech_voice_ratios) if speech_voice_ratios else 0.0
            decode_rms_threshold = max(args.decode_min_rms, noise_rms + args.decode_noise_margin)
            too_weak = segment_rms < decode_rms_threshold
            too_short = speech_seconds < args.decode_min_seconds
            if too_weak or too_short:
                skipped_segments += 1
                if now - last_skip_log >= args.skip_log_seconds:
                    print(
                        f"skipped low_quality segments={skipped_segments} "
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
            command_mode = now <= command_listen_until
            decode_aliases = photo_aliases if command_mode else aliases
            decode_grammar = photo_grammar if command_mode else wake_grammar
            ok, text = decode_segment(model, args, segment, decode_aliases, decode_grammar)
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
                    _request_path, result_path = queue_photo_request(args)
                    saved, result = wait_photo_result(result_path, args.photo_result_timeout_seconds)
                    if saved:
                        print(
                            f"photo command matched: {text} -> saved path={result.get('image_path', '<unknown>')}",
                            flush=True,
                        )
                        playback = play_response(args.photo_response_wav, args.playback_device)
                        if playback is not None:
                            playbacks.append((playback, False, now))
                    else:
                        print(
                            f"photo command matched but snapshot not confirmed: "
                            f"status={result.get('status')} error={result.get('error', '')}",
                            flush=True,
                        )
                    command_listen_until = 0.0
                    matcher.reset()
            elif ok and now - last_trigger >= args.cooldown_seconds:
                print(f"wake matched: {text} ({match_reason or 'full'}) -> 我在", flush=True)
                playback = play_response(args.response_wav, args.playback_device)
                if playback is not None:
                    playbacks.append((playback, True, now))
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
        for playback, _opens_command_window, _started_at in playbacks:
            if playback.poll() is None:
                playback.terminate()
        terminate_capture(capture_procs)


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
    p_listen.add_argument("--response-wav", type=Path, default=DEFAULT_RESPONSE)
    p_listen.add_argument("--photo-response-wav", type=Path, default=DEFAULT_PHOTO_RESPONSE)
    p_listen.add_argument(
        "--photo-alias",
        action="append",
        default=["拍照", "拍 照", "拍张照", "拍 张 照", "照相"],
    )
    p_listen.add_argument("--post-wake-command-seconds", type=float, default=8.0)
    p_listen.add_argument("--photo-request-dir", type=Path, default=DEFAULT_PHOTO_REQUEST_DIR)
    p_listen.add_argument("--photo-result-dir", type=Path, default=DEFAULT_PHOTO_RESULT_DIR)
    p_listen.add_argument("--photo-result-timeout-seconds", type=float, default=5.0)
    p_listen.add_argument("--photo-result-retention-seconds", type=float, default=86400.0)
    p_listen.add_argument("--photo-output-root", type=Path, default=DEFAULT_PHOTO_OUTPUT_ROOT)
    p_listen.add_argument("--photo-sender-id", default="*")
    p_listen.add_argument("--photo-camera-id", default="cam01")
    p_listen.add_argument("--audio-filter", choices=sorted(AUDIO_FILTERS), default="voice")
    p_listen.add_argument("--audio-filter-graph", default="")
    p_listen.add_argument("--chunk-seconds", type=float, default=0.10)
    p_listen.add_argument("--cooldown-seconds", type=float, default=2.5)
    p_listen.add_argument("--playback-ignore-seconds", type=float, default=0.3)
    p_listen.add_argument("--echo-tail-seconds", type=float, default=0.3)
    p_listen.add_argument("--vad-calibration-seconds", type=float, default=1.5)
    p_listen.add_argument("--vad-noise-scale", type=float, default=1.25)
    p_listen.add_argument("--vad-noise-margin", type=float, default=0.003)
    p_listen.add_argument("--vad-min-rms", type=float, default=0.008)
    p_listen.add_argument("--noise-update-alpha", type=float, default=0.03)
    p_listen.add_argument("--noise-log-seconds", type=float, default=20.0)
    p_listen.add_argument("--zero-audio-rms", type=float, default=0.0005)
    p_listen.add_argument("--zero-audio-restart-seconds", type=float, default=12.0)
    p_listen.add_argument("--barge-in", action=argparse.BooleanOptionalAction, default=True)
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
    p_listen.add_argument("--decode-noise-margin", type=float, default=0.008)
    p_listen.add_argument("--skip-log-seconds", type=float, default=20.0)
    p_listen.add_argument("--allow-split-wake", action=argparse.BooleanOptionalAction, default=True)
    p_listen.add_argument("--split-wake-window-seconds", type=float, default=2.4)
    p_listen.add_argument("--pre-roll-seconds", type=float, default=0.30)
    p_listen.add_argument("--min-speech-seconds", type=float, default=0.45)
    p_listen.add_argument("--end-silence-seconds", type=float, default=0.55)
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
