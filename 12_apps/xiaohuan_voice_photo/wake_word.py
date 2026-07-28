#!/usr/bin/env python3
import argparse
import collections
import json
import math
import os
import signal
import subprocess
import sys
import time
import wave
from pathlib import Path

import numpy as np


BASE_DIR = Path(__file__).resolve().parent
DEFAULT_CONFIG = BASE_DIR / "wake_word_config.json"
DEFAULT_RESPONSE = BASE_DIR / "response_wozai.wav"


def read_wav_mono(path: Path):
    with wave.open(str(path), "rb") as wf:
        channels = wf.getnchannels()
        sample_rate = wf.getframerate()
        sample_width = wf.getsampwidth()
        frames = wf.readframes(wf.getnframes())
    if sample_width != 2:
        raise ValueError(f"unsupported sample width: {sample_width}")
    audio = np.frombuffer(frames, dtype="<i2").astype(np.float32) / 32768.0
    if channels > 1:
        audio = audio.reshape(-1, channels).mean(axis=1)
    return audio, sample_rate


def bytes_to_float32(raw: bytes):
    if not raw:
        return np.zeros(0, dtype=np.float32)
    return np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0


def rms(audio: np.ndarray):
    if audio.size == 0:
        return 0.0
    return float(np.sqrt(np.mean(np.square(audio), dtype=np.float64)))


def resample_to_16k(audio: np.ndarray, sample_rate: int):
    if sample_rate == 16000:
        return audio
    if sample_rate == 48000:
        usable = (audio.size // 3) * 3
        if usable == 0:
            return np.zeros(0, dtype=np.float32)
        return audio[:usable].reshape(-1, 3).mean(axis=1).astype(np.float32)
    duration = audio.size / float(sample_rate)
    out_len = max(1, int(round(duration * 16000)))
    x_old = np.linspace(0.0, duration, num=audio.size, endpoint=False)
    x_new = np.linspace(0.0, duration, num=out_len, endpoint=False)
    return np.interp(x_new, x_old, audio).astype(np.float32)


def trim_silence(audio: np.ndarray, sample_rate: int):
    if audio.size == 0:
        return audio
    frame = max(1, int(sample_rate * 0.02))
    hop = max(1, int(sample_rate * 0.01))
    values = []
    starts = []
    for start in range(0, max(1, audio.size - frame + 1), hop):
        chunk = audio[start:start + frame]
        values.append(rms(chunk))
        starts.append(start)
    if not values:
        return audio
    values = np.asarray(values, dtype=np.float32)
    peak = float(np.max(values))
    floor = float(np.percentile(values, 30))
    threshold = max(floor * 2.5, peak * 0.12, 0.002)
    active = np.where(values >= threshold)[0]
    if active.size == 0:
        return audio
    left = max(0, starts[int(active[0])] - int(sample_rate * 0.15))
    right = min(audio.size, starts[int(active[-1])] + frame + int(sample_rate * 0.20))
    return audio[left:right]


def mel_filterbank(sample_rate=16000, n_fft=512, n_mels=32):
    def hz_to_mel(hz):
        return 2595.0 * math.log10(1.0 + hz / 700.0)

    def mel_to_hz(mel):
        return 700.0 * (10.0 ** (mel / 2595.0) - 1.0)

    low_mel = hz_to_mel(80.0)
    high_mel = hz_to_mel(sample_rate / 2.0)
    mel_points = np.linspace(low_mel, high_mel, n_mels + 2)
    hz_points = np.asarray([mel_to_hz(m) for m in mel_points])
    bins = np.floor((n_fft + 1) * hz_points / sample_rate).astype(int)
    fb = np.zeros((n_mels, n_fft // 2 + 1), dtype=np.float32)
    for i in range(1, n_mels + 1):
        left, center, right = bins[i - 1], bins[i], bins[i + 1]
        if center <= left:
            center = left + 1
        if right <= center:
            right = center + 1
        for j in range(left, min(center, fb.shape[1])):
            fb[i - 1, j] = (j - left) / float(center - left)
        for j in range(center, min(right, fb.shape[1])):
            fb[i - 1, j] = (right - j) / float(right - center)
    return fb


_MEL_FB = mel_filterbank()


def features(audio: np.ndarray, sample_rate: int):
    audio = resample_to_16k(audio, sample_rate)
    audio = audio - float(np.mean(audio)) if audio.size else audio
    audio = trim_silence(audio, 16000)
    if audio.size < 1600:
        return None
    current_rms = rms(audio)
    if current_rms > 1e-5:
        audio = audio * min(10.0, 0.08 / current_rms)
    frame_len = 400
    hop = 160
    n_fft = 512
    if audio.size < frame_len:
        return None
    frames = []
    window = np.hamming(frame_len).astype(np.float32)
    for start in range(0, audio.size - frame_len + 1, hop):
        frame = audio[start:start + frame_len] * window
        spectrum = np.abs(np.fft.rfft(frame, n=n_fft)) ** 2
        mel = _MEL_FB @ spectrum
        frames.append(np.log(mel + 1e-8))
    feat = np.asarray(frames, dtype=np.float32)
    if feat.shape[0] < 4:
        return None
    feat = (feat - feat.mean(axis=0, keepdims=True)) / (feat.std(axis=0, keepdims=True) + 1e-5)
    delta = np.vstack([np.zeros((1, feat.shape[1]), dtype=np.float32), np.diff(feat, axis=0)])
    return np.concatenate([feat, delta * 0.6], axis=1)


def dtw_distance(a: np.ndarray, b: np.ndarray):
    n, m = a.shape[0], b.shape[0]
    if n == 0 or m == 0:
        return float("inf")
    prev = np.full(m + 1, np.inf, dtype=np.float32)
    curr = np.full(m + 1, np.inf, dtype=np.float32)
    prev[0] = 0.0
    for i in range(1, n + 1):
        curr[0] = np.inf
        ai = a[i - 1]
        for j in range(1, m + 1):
            d = float(np.linalg.norm(ai - b[j - 1]))
            curr[j] = d + min(prev[j], curr[j - 1], prev[j - 1])
        prev, curr = curr, prev
    return float(prev[m] / (n + m))


def score_against_templates(audio: np.ndarray, sample_rate: int, template_features):
    feat = features(audio, sample_rate)
    if feat is None:
        return float("inf"), None
    scores = [dtw_distance(feat, tmpl) for tmpl in template_features]
    idx = int(np.argmin(scores))
    return float(scores[idx]), idx


def run_arecord_to_file(device, sample_rate, seconds, path: Path):
    cmd = [
        "arecord", "-q",
        "-D", device,
        "-f", "S16_LE",
        "-r", str(sample_rate),
        "-c", "1",
        "-d", str(seconds),
        str(path),
    ]
    subprocess.run(cmd, check=True)


def load_config(path: Path):
    if not path.exists():
        raise FileNotFoundError(f"config not found: {path}")
    data = json.loads(path.read_text(encoding="utf-8"))
    template_features = []
    for item in data["templates"]:
        wav_path = Path(item)
        audio, sr = read_wav_mono(wav_path)
        feat = features(audio, sr)
        if feat is None:
            raise RuntimeError(f"template has no usable speech: {wav_path}")
        template_features.append(feat)
    return data, template_features


def save_config(path: Path, templates, threshold, sample_rate, record_device, playback_device):
    data = {
        "keyword": "你好小环",
        "response_text": "我在",
        "sample_rate": sample_rate,
        "record_device": record_device,
        "playback_device": playback_device,
        "threshold": threshold,
        "templates": [str(Path(t).resolve()) for t in templates],
        "response_wav": str(DEFAULT_RESPONSE.resolve()),
    }
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def enroll(args):
    template_dir = BASE_DIR / "keyword_templates"
    template_dir.mkdir(parents=True, exist_ok=True)
    paths = []
    for i in range(args.count):
        path = template_dir / f"nihao_xiaohuan_{i + 1:02d}.wav"
        print(f"\n[{i + 1}/{args.count}] 1 秒后开始录关键词：你好小环")
        time.sleep(1.0)
        run_arecord_to_file(args.record_device, args.sample_rate, args.seconds, path)
        paths.append(path)
        audio, sr = read_wav_mono(path)
        print(f"saved: {path.name}, rms={rms(audio):.5f}")

    feats = []
    for path in paths:
        audio, sr = read_wav_mono(path)
        feat = features(audio, sr)
        if feat is None:
            raise RuntimeError(f"recording too quiet or too short: {path}")
        feats.append(feat)

    pair_scores = []
    for i in range(len(feats)):
        for j in range(i + 1, len(feats)):
            pair_scores.append(dtw_distance(feats[i], feats[j]))
    if pair_scores:
        base = max(pair_scores)
        threshold = base * args.threshold_scale
    else:
        threshold = args.threshold
    threshold = float(max(0.8, min(args.threshold, threshold)))
    save_config(args.config, paths, threshold, args.sample_rate, args.record_device, args.playback_device)
    print("\n模板录制完成")
    print(f"pair_scores={['%.3f' % s for s in pair_scores]}")
    print(f"threshold={threshold:.3f}")
    print(f"config={args.config}")


def record_response(args):
    print("1 秒后开始录固定回复：我在")
    time.sleep(1.0)
    run_arecord_to_file(args.record_device, args.sample_rate, args.seconds, args.output)
    print(f"saved: {args.output}")


def play_response(response_wav: Path, playback_device: str):
    if response_wav.exists():
        subprocess.Popen(["aplay", "-q", "-D", playback_device, str(response_wav)])
    else:
        print(f"response wav not found: {response_wav}")


def listen(args):
    data, template_features = load_config(args.config)
    threshold = float(args.threshold if args.threshold is not None else data.get("threshold", 2.2))
    response_wav = Path(args.response_wav or data.get("response_wav", DEFAULT_RESPONSE))
    record_device = args.record_device or data.get("record_device", "hw:3,0")
    playback_device = args.playback_device or data.get("playback_device", "plughw:3,0")
    sample_rate = int(args.sample_rate or data.get("sample_rate", 48000))

    chunk_samples = int(sample_rate * args.chunk_seconds)
    chunk_bytes = chunk_samples * 2
    cmd = [
        "arecord", "-q",
        "-D", record_device,
        "-f", "S16_LE",
        "-r", str(sample_rate),
        "-c", "1",
        "-t", "raw",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stop = False

    def handle_signal(_signum, _frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    print("calibrating noise floor, please stay quiet for 2 seconds...")
    noise_values = []
    for _ in range(max(1, int(args.calibration_seconds / args.chunk_seconds))):
        raw = proc.stdout.read(chunk_bytes)
        if len(raw) < chunk_bytes:
            break
        noise_values.append(rms(bytes_to_float32(raw)))
    noise = float(np.median(noise_values)) if noise_values else 0.003
    vad_threshold = max(noise * args.noise_scale, args.min_rms)
    print(f"listening: threshold={threshold:.3f}, noise={noise:.5f}, vad={vad_threshold:.5f}")
    print("say: 你好小环")

    pre_roll = collections.deque(maxlen=max(1, int(0.35 / args.chunk_seconds)))
    speech_chunks = []
    in_speech = False
    silence_chunks = 0
    last_trigger = 0.0

    try:
        while not stop:
            raw = proc.stdout.read(chunk_bytes)
            if len(raw) < chunk_bytes:
                break
            audio = bytes_to_float32(raw)
            level = rms(audio)
            active = level >= vad_threshold
            pre_roll.append(raw)

            if active and not in_speech:
                in_speech = True
                speech_chunks = list(pre_roll)
                silence_chunks = 0
            elif in_speech:
                speech_chunks.append(raw)
                if active:
                    silence_chunks = 0
                else:
                    silence_chunks += 1

            if in_speech:
                speech_seconds = len(speech_chunks) * args.chunk_seconds
                ended = silence_chunks >= int(args.end_silence_seconds / args.chunk_seconds) and speech_seconds >= 0.45
                too_long = speech_seconds >= args.max_speech_seconds
                if ended or too_long:
                    segment = bytes_to_float32(b"".join(speech_chunks))
                    score, template_idx = score_against_templates(segment, sample_rate, template_features)
                    print(f"speech score={score:.3f}, template={template_idx}, seconds={speech_seconds:.2f}, rms={rms(segment):.5f}")
                    now = time.time()
                    if score <= threshold and now - last_trigger >= args.cooldown_seconds:
                        print("wake word matched: 你好小环 -> 我在")
                        play_response(response_wav, playback_device)
                        last_trigger = now
                    in_speech = False
                    speech_chunks = []
                    silence_chunks = 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=1.0)
        except subprocess.TimeoutExpired:
            proc.kill()


def build_parser():
    parser = argparse.ArgumentParser(description="Local wake-word prototype for 'ni hao xiao huan'.")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--record-device", default="hw:3,0")
    parser.add_argument("--playback-device", default="plughw:3,0")
    parser.add_argument("--sample-rate", type=int, default=48000)

    sub = parser.add_subparsers(dest="command", required=True)

    p_enroll = sub.add_parser("enroll")
    p_enroll.add_argument("--count", type=int, default=5)
    p_enroll.add_argument("--seconds", type=int, default=2)
    p_enroll.add_argument("--threshold", type=float, default=3.2)
    p_enroll.add_argument("--threshold-scale", type=float, default=1.35)
    p_enroll.set_defaults(func=enroll)

    p_resp = sub.add_parser("record-response")
    p_resp.add_argument("--seconds", type=int, default=2)
    p_resp.add_argument("--output", type=Path, default=DEFAULT_RESPONSE)
    p_resp.set_defaults(func=record_response)

    p_listen = sub.add_parser("listen")
    p_listen.add_argument("--response-wav", type=Path, default=None)
    p_listen.add_argument("--threshold", type=float, default=None)
    p_listen.add_argument("--chunk-seconds", type=float, default=0.10)
    p_listen.add_argument("--calibration-seconds", type=float, default=2.0)
    p_listen.add_argument("--noise-scale", type=float, default=3.2)
    p_listen.add_argument("--min-rms", type=float, default=0.004)
    p_listen.add_argument("--end-silence-seconds", type=float, default=0.45)
    p_listen.add_argument("--max-speech-seconds", type=float, default=3.0)
    p_listen.add_argument("--cooldown-seconds", type=float, default=2.5)
    p_listen.set_defaults(func=listen)
    return parser


def main():
    args = build_parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
