#!/usr/bin/env python3
import json
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


SOURCE_ROOT = Path(__file__).resolve().parents[1]


class State:
    recording = False
    completed = 3
    calls = []


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
        self._send(
            {
                "recording_state": "recording" if State.recording else "idle",
                "recording_all": State.recording,
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
        elif self.path == "/api/record/stop-all":
            State.recording = False
            State.completed += 1
        self._send({"ok": True})


def main():
    State.recording = False
    State.completed = 3
    State.calls = []
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        result = subprocess.run(
            [
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
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
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
