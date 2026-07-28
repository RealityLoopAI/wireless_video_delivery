#!/usr/bin/env python3
import argparse
import signal
import subprocess
import sys
import time
import wave
from pathlib import Path

import numpy as np
import sherpa_onnx


BASE_DIR = Path(__file__).resolve().parent
MODEL_DIR = BASE_DIR / "models" / "sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01-mobile"
DEFAULT_KEYWORDS = BASE_DIR / "keywords_nihao_xiaohuan_variants.txt"
DEFAULT_RESPONSE = BASE_DIR / "response_wozai_tts_default.wav"


def read_wav(path: Path):
    with wave.open(str(path), "rb") as wf:
        sample_rate = wf.getframerate()
        channels = wf.getnchannels()
        sample_width = wf.getsampwidth()
        raw = wf.readframes(wf.getnframes())
    if sample_width != 2:
        raise ValueError(f"unsupported sample width: {sample_width}")
    samples = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    if channels > 1:
        samples = samples.reshape(-1, channels).mean(axis=1)
    return samples, sample_rate


def pcm16_to_float32(raw: bytes):
    if not raw:
        return np.zeros(0, dtype=np.float32)
    return np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0


def make_spotter(args):
    encoder = MODEL_DIR / "encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx"
    decoder = MODEL_DIR / "decoder-epoch-12-avg-2-chunk-16-left-64.onnx"
    joiner = MODEL_DIR / "joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx"
    tokens = MODEL_DIR / "tokens.txt"
    missing = [p for p in [encoder, decoder, joiner, tokens, args.keywords_file] if not p.exists()]
    if missing:
        raise FileNotFoundError("missing files: " + ", ".join(str(p) for p in missing))

    return sherpa_onnx.KeywordSpotter(
        tokens=str(tokens),
        encoder=str(encoder),
        decoder=str(decoder),
        joiner=str(joiner),
        keywords_file=str(args.keywords_file),
        num_threads=args.num_threads,
        sample_rate=16000,
        feature_dim=80,
        max_active_paths=args.max_active_paths,
        keywords_score=args.keywords_score,
        keywords_threshold=args.keywords_threshold,
        num_trailing_blanks=args.num_trailing_blanks,
        provider="cpu",
    )


def decode_stream(spotter, stream):
    detected = []
    while spotter.is_ready(stream):
        spotter.decode_stream(stream)
        result = spotter.get_result(stream)
        if result:
            detected.append(result)
            spotter.reset_stream(stream)
    return detected


def run_file(args):
    spotter = make_spotter(args)
    samples, sample_rate = read_wav(args.wav)
    stream = spotter.create_stream()
    stream.accept_waveform(sample_rate, samples)
    stream.accept_waveform(sample_rate, np.zeros(int(0.8 * sample_rate), dtype=np.float32))
    stream.input_finished()
    detected = decode_stream(spotter, stream)
    if detected:
        for item in detected:
            print(f"detected: {item}")
        return 0
    print("detected: none")
    return 1


def play_response(path: Path, device: str):
    if not path.exists():
        print(f"response wav not found: {path}", file=sys.stderr)
        return None
    return subprocess.Popen(
        ["aplay", "-q", "-D", device, str(path)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def listen(args):
    spotter = make_spotter(args)
    stream = spotter.create_stream()
    chunk_samples = int(args.sample_rate * args.chunk_seconds)
    chunk_bytes = chunk_samples * 2
    arecord_cmd = [
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
    proc = subprocess.Popen(arecord_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stop = False

    def handle_signal(_signum, _frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    print("listening for: 你好小环")
    print(f"record_device={args.record_device}, playback_device={args.playback_device}")
    last_trigger = 0.0
    playbacks = []
    try:
        while not stop:
            playbacks = [p for p in playbacks if p.poll() is None]
            raw = proc.stdout.read(chunk_bytes)
            if len(raw) < chunk_bytes:
                break
            samples = pcm16_to_float32(raw)
            stream.accept_waveform(args.sample_rate, samples)
            detected = decode_stream(spotter, stream)
            now = time.time()
            for item in detected:
                print(f"detected: {item}")
                if now - last_trigger >= args.cooldown_seconds:
                    playback = play_response(args.response_wav, args.playback_device)
                    if playback is not None:
                        playbacks.append(playback)
                    last_trigger = now
    finally:
        for playback in playbacks:
            if playback.poll() is None:
                playback.terminate()
        proc.terminate()
        try:
            proc.wait(timeout=1.0)
        except subprocess.TimeoutExpired:
            proc.kill()


def build_parser():
    parser = argparse.ArgumentParser(description="sherpa-onnx KWS prototype for 你好小环")
    parser.add_argument("--keywords-file", type=Path, default=DEFAULT_KEYWORDS)
    parser.add_argument("--num-threads", type=int, default=2)
    parser.add_argument("--max-active-paths", type=int, default=4)
    parser.add_argument("--keywords-score", type=float, default=1.5)
    parser.add_argument("--keywords-threshold", type=float, default=0.20)
    parser.add_argument("--num-trailing-blanks", type=int, default=1)

    sub = parser.add_subparsers(dest="command", required=True)

    p_file = sub.add_parser("file")
    p_file.add_argument("wav", type=Path)
    p_file.set_defaults(func=run_file)

    p_listen = sub.add_parser("listen")
    p_listen.add_argument("--record-device", default="plughw:4,0")
    p_listen.add_argument("--playback-device", default="plughw:3,0")
    p_listen.add_argument("--response-wav", type=Path, default=DEFAULT_RESPONSE)
    p_listen.add_argument("--sample-rate", type=int, default=16000)
    p_listen.add_argument("--chunk-seconds", type=float, default=0.10)
    p_listen.add_argument("--cooldown-seconds", type=float, default=2.5)
    p_listen.set_defaults(func=listen)
    return parser


def main():
    args = build_parser().parse_args()
    result = args.func(args)
    if isinstance(result, int):
        raise SystemExit(result)


if __name__ == "__main__":
    main()
