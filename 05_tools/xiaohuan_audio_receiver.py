#!/usr/bin/env python3
import argparse
from datetime import datetime
import io
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import threading
import uuid
import wave


class AudioReceiver:
    def __init__(
        self,
        output_root: Path,
        *,
        max_body_bytes: int,
        expected_sample_rate: int,
        max_duration_seconds: float,
    ):
        self.output_root = output_root.expanduser().resolve()
        self.max_body_bytes = max_body_bytes
        self.expected_sample_rate = expected_sample_rate
        self.max_duration_seconds = max_duration_seconds
        self._write_lock = threading.Lock()
        self.received = 0
        self.failures = 0

    def validate(self, payload: bytes):
        try:
            with wave.open(io.BytesIO(payload), "rb") as audio:
                channels = audio.getnchannels()
                sample_width = audio.getsampwidth()
                sample_rate = audio.getframerate()
                frame_count = audio.getnframes()
        except (EOFError, wave.Error) as exc:
            raise ValueError(f"invalid WAV: {exc}") from exc

        if channels != 1:
            raise ValueError(f"expected mono WAV, got channels={channels}")
        if sample_width != 2:
            raise ValueError(
                f"expected PCM 16-bit WAV, got sample_width={sample_width}"
            )
        if sample_rate != self.expected_sample_rate:
            raise ValueError(
                f"expected {self.expected_sample_rate}Hz WAV, got {sample_rate}Hz"
            )
        duration = frame_count / max(1, sample_rate)
        if duration <= 0 or duration > self.max_duration_seconds:
            raise ValueError(
                f"WAV duration out of range: {duration:.3f}s "
                f"(max {self.max_duration_seconds:.3f}s)"
            )
        return duration

    def save(self, payload: bytes):
        duration = self.validate(payload)
        now = datetime.now()
        day_directory = self.output_root / now.strftime("%Y-%m-%d")
        filename = (
            now.strftime("%Y%m%d_%H%M%S_%f")
            + "_"
            + uuid.uuid4().hex[:8]
            + ".wav"
        )
        final_path = day_directory / filename
        temporary_path = day_directory / ("." + filename + ".tmp")

        with self._write_lock:
            day_directory.mkdir(parents=True, exist_ok=True)
            try:
                with temporary_path.open("xb") as output:
                    output.write(payload)
                    output.flush()
                    os.fsync(output.fileno())
                os.replace(temporary_path, final_path)
            finally:
                temporary_path.unlink(missing_ok=True)
            self.received += 1
        return final_path, duration


class ReusableThreadingHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True


def make_handler(receiver: AudioReceiver):
    class Handler(BaseHTTPRequestHandler):
        server_version = "XiaohuanAudioReceiver/1.0"
        protocol_version = "HTTP/1.1"

        def do_POST(self):
            self.connection.settimeout(10.0)
            if self.path != "/api/audio":
                self.send_json(404, {"accepted": False, "error": "not_found"})
                return
            content_type = self.headers.get("Content-Type", "")
            if content_type.split(";", 1)[0].strip().lower() != "audio/wav":
                self.send_json(
                    415,
                    {"accepted": False, "error": "content_type_must_be_audio_wav"},
                )
                return
            try:
                body_size = int(self.headers.get("Content-Length", ""))
            except ValueError:
                self.send_json(
                    411,
                    {"accepted": False, "error": "content_length_required"},
                )
                return
            if body_size <= 0 or body_size > receiver.max_body_bytes:
                self.send_json(
                    413,
                    {
                        "accepted": False,
                        "error": "request_too_large",
                        "max_body_bytes": receiver.max_body_bytes,
                    },
                )
                return

            try:
                payload = self.rfile.read(body_size)
                if len(payload) != body_size:
                    raise ValueError(
                        f"incomplete request body: got={len(payload)} expected={body_size}"
                    )
                path, duration = receiver.save(payload)
            except (OSError, ValueError) as exc:
                receiver.failures += 1
                self.send_json(
                    400,
                    {
                        "accepted": False,
                        "error": "invalid_audio",
                        "detail": str(exc),
                    },
                )
                return

            print(
                f"audio accepted path={path} bytes={body_size} "
                f"duration={duration:.3f}s",
                flush=True,
            )
            self.send_json(
                202,
                {
                    "accepted": True,
                    "filename": path.name,
                    "duration_seconds": round(duration, 3),
                },
            )

        def do_GET(self):
            if self.path != "/healthz":
                self.send_json(404, {"ok": False, "error": "not_found"})
                return
            self.send_json(
                200,
                {
                    "ok": True,
                    "service": "xiaohuan_audio_receiver",
                    "received": receiver.received,
                    "failures": receiver.failures,
                },
            )

        def send_json(self, status, payload):
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *args):
            print(f"HTTP {self.address_string()} {fmt % args}", flush=True)

    return Handler


def build_parser():
    parser = argparse.ArgumentParser(
        description="Receive complete Xiaohuan utterances as raw WAV HTTP bodies"
    )
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=50020)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path.home() / "Desktop" / "xiaohuan_received_audio",
    )
    parser.add_argument("--max-body-mb", type=int, default=4)
    parser.add_argument("--sample-rate", type=int, default=16000)
    parser.add_argument("--max-duration-seconds", type=float, default=61.0)
    return parser


def main():
    args = build_parser().parse_args()
    receiver = AudioReceiver(
        args.output_dir,
        max_body_bytes=args.max_body_mb * 1024 * 1024,
        expected_sample_rate=args.sample_rate,
        max_duration_seconds=args.max_duration_seconds,
    )
    server = ReusableThreadingHTTPServer(
        (args.bind, args.port),
        make_handler(receiver),
    )
    print(
        f"listening http://{args.bind}:{server.server_address[1]}/api/audio "
        f"output={receiver.output_root}",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
