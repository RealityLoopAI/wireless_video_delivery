#!/usr/bin/env python3
import datetime
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


SOURCE_ROOT = Path(__file__).resolve().parents[1]


class State:
    recording = False
    completed = 3
    calls = []
    tail_deadline = 0.0
    session_id = 0
    replace_after_stop = False
    replace_before_stop = False
    started_checks = 0


class Handler(BaseHTTPRequestHandler):
    def log_message(self, _format, *_args):
        pass

    def _send(self, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        State.calls.append(("GET", self.path))
        if State.recording:
            State.started_checks += 1
            if State.replace_before_stop and State.started_checks >= 2:
                State.session_id = 456
        if State.tail_deadline and time.monotonic() >= State.tail_deadline:
            State.tail_deadline = 0.0
            State.completed += 1
        self._send(
            {
                "recording_state": "recording" if State.recording else "idle",
                "recording_all": State.recording,
                "recording_session_id": State.session_id,
                "recording_start_ready": not State.recording,
                "record_finalize_outstanding_segments": 0,
                "record_queue_total_bytes": 0,
                "recording_delivery_pending": 0,
                "recording_uploader": {"pending_segments": 0, "active_transfers": 0},
                "cameras": [
                    {
                        "camera_key": "sender-a_cam01",
                        "live": True,
                        "recording": State.recording,
                        "record_tail_draining": bool(State.tail_deadline),
                        "segment_finalize_completed": State.completed,
                        "record_write_errors": 0,
                    }
                ],
            }
        )

    def do_POST(self):
        State.calls.append(("POST", self.path))
        if self.path == "/api/record/start-all":
            State.recording = True
            State.session_id = 123
            State.started_checks = 0
        elif self.path == "/api/record/stop-all":
            State.recording = False
            # Stop acknowledges before delayed media completes the segment.
            State.tail_deadline = time.monotonic() + 0.4
            if State.replace_after_stop:
                State.recording = True
                State.session_id = 456
        self._send({"ok": True, "recording_session_id": 123})


def main():
    State.recording = False
    State.completed = 3
    State.calls = []
    State.tail_deadline = 0.0
    with tempfile.TemporaryDirectory(prefix="gwv3-acceptance-test-") as temporary_text:
        temporary = Path(temporary_text)
        session_date = datetime.datetime.fromtimestamp(123 / 1_000_000).strftime(
            "%Y-%m-%d"
        )
        segment = temporary / "nas" / "sender-a_cam01" / session_date / "segment"
        segment.mkdir(parents=True)
        (segment / "deployment_canaryrecording_ready.json").write_text(
            json.dumps({"recording_session_id": 123, "ready": True,
                        "recording_complete": False, "recording_quality_status": "partial",
                        "recording_quality_reason": "rgb contains a gap over 500 ms"}), encoding="utf-8"
        )
        (segment / "frames.csv").write_text("header\nframe1\nframe2\n", encoding="utf-8")
        (segment / "rgb.mp4").write_bytes(b"test-rgb")
        (segment / "depth.mkv").write_bytes(b"test-depth")
        fake_bin = temporary / "bin"
        fake_bin.mkdir()
        ffprobe = fake_bin / "ffprobe"
        ffprobe.write_text("#!/bin/sh\necho video\n", encoding="utf-8")
        ffprobe.chmod(0o755)
        config = temporary / "receiver.json"
        config.write_text(
            json.dumps(
                {
                    "nas_root": str(temporary / "nas"),
                    "recording_staging": {"root": str(temporary / "staging")},
                }
            ),
            encoding="utf-8",
        )
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            command = [
                sys.executable,
                str(SOURCE_ROOT / "05_tools/run_deployment_acceptance.py"),
                "--admin",
                f"http://127.0.0.1:{server.server_port}",
                "--record-seconds",
                "0.05",
                "--start-timeout",
                "2",
                "--finalize-timeout",
                "2",
                "--config",
                str(config),
                "--cleanup-on-success",
                "--require-complete-recording",
            ]
            environment = {**os.environ, "PATH": f"{fake_bin}:{os.environ['PATH']}"}
            partial = subprocess.run(command, text=True, capture_output=True, env=environment, timeout=5)
            partial_report = json.loads(partial.stdout)
            assert partial.returncode != 0, partial.stdout
            assert partial_report["ok"] is False, partial.stdout
            assert partial_report["recording_files"]["quality_passed"] is False
            assert "rgb contains a gap" in partial_report["error"]
            assert segment.exists(), "failed quality verification must retain evidence"
            (segment / "deployment_canaryrecording_ready.json").write_text(
                json.dumps({"recording_session_id": 123, "ready": True}), encoding="utf-8"
            )
            unknown = subprocess.run(command, text=True, capture_output=True, env=environment, timeout=5)
            unknown_report = json.loads(unknown.stdout)
            assert unknown.returncode != 0 and unknown_report["ok"] is False
            assert "unknown" in unknown_report["error"]
            assert segment.exists(), "legacy metadata is not proof of complete capture"
            for before_stop in (False, True):
                State.calls = []
                State.recording = False
                State.replace_after_stop = not before_stop
                State.replace_before_stop = before_stop
                changed = subprocess.run(command, text=True, capture_output=True,
                                         env=environment, timeout=5)
                changed_report = json.loads(changed.stdout)
                assert changed.returncode != 0, changed.stdout
                assert "recording session changed" in changed_report["error"], changed.stdout
                stops = State.calls.count(("POST", "/api/record/stop-all"))
                assert stops == (0 if before_stop else 1), State.calls
                assert State.recording and State.session_id == 456
                assert segment.exists(), "session replacement must not delete evidence"
            State.replace_after_stop = False
            State.replace_before_stop = False
            State.recording = False
            State.session_id = 0
            (segment / "deployment_canaryrecording_ready.json").write_text(
                json.dumps({"recording_session_id": 123, "ready": True,
                            "recording_complete": True, "recording_quality_status": "complete"}), encoding="utf-8"
            )
            result = subprocess.run(
                command,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=environment,
                timeout=5,
            )
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
        assert result.returncode == 0, result.stdout + result.stderr
        assert not segment.exists()
    assert result.returncode == 0, result.stdout + result.stderr
    report = json.loads(result.stdout)
    assert report["ok"] is True
    assert report["camera_results"]["sender-a_cam01"][
        "segment_finalize_completed_delta"
    ] == 1
    assert ("POST", "/api/record/start-all") in State.calls
    assert ("POST", "/api/record/stop-all") in State.calls
    print("deployment acceptance test passed")


if __name__ == "__main__":
    main()
