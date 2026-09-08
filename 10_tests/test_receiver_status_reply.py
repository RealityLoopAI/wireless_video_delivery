#!/usr/bin/env python3
"""A connected UDP sender accepts controls only from its status peer tuple."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time

import test_receiver_hardening as h


def run(receiver):
    with tempfile.TemporaryDirectory(prefix="gwv3_status_reply_") as tmp:
        root = Path(tmp)
        ports = {"status": h.free_port(socket.SOCK_DGRAM),
                 "media": h.free_port(socket.SOCK_STREAM), "admin": h.free_port(socket.SOCK_STREAM)}
        assert len(set(ports.values())) == 3
        cfg = {"status_bind_ip": "127.0.0.1", "status_port": ports["status"],
               "media_bind_ip": "127.0.0.1", "media_port": ports["media"],
               "admin_bind_ip": "127.0.0.1", "admin_port": ports["admin"],
               "receiver_discovery": {"enabled": False}, "clock_sync": {"enabled": False},
               "nas_auto_mount": {"enabled": False}, "preview_enabled": False,
               "nas_root": str(root / "nas"), "state_path": str(root / "state.json"),
               "log_directory": str(root / "logs"), "task_audio": {"enabled": False},
               "recording_staging": {"enabled": False}}
        config = root / "receiver.json"
        config.write_text(json.dumps(cfg))
        payload = h.generate_h264_fixture(1)
        with (root / "stdout.log").open("wb") as log:
            proc = subprocess.Popen([receiver, "--config", str(config)], stdout=log, stderr=log)
            try:
                h.wait_http(ports["admin"])
                # The kernel rejects datagrams from a different source port,
                # modelling an address/port-restricted NAT without firewall edits.
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as status:
                    status.connect(("127.0.0.1", ports["status"]))
                    status.settimeout(2)
                    announce = {"protocol_version": "3.0", "message_type": "camera_announce",
                                "sender_id": "reply-test", "camera_id": "cam01",
                                "rgb_profile": {"width": 64, "height": 48, "fps": 30}}
                    status.send(json.dumps(announce).encode())
                    time.sleep(.1)
                    with socket.create_connection(("127.0.0.1", ports["media"]), timeout=3) as media:
                        media.sendall(h.rgb_packet("reply-test", "cam01", 1, 64, 48,
                                                  time.time_ns() // 1000, payload))
                        deadline = time.monotonic() + 3
                        while time.monotonic() < deadline:
                            _, _, raw = h.request(ports["admin"], "GET", "/api/status")
                            if any(c["live"] for c in json.loads(raw)["cameras"]):
                                break
                            time.sleep(.02)
                        code, _, raw = h.request(ports["admin"], "POST", "/api/record/start-all")
                        assert code == 200 and json.loads(raw)["ok"], (code, raw)
                        packet, peer = status.recvfrom(65536)
                        control = json.loads(packet)
                        assert peer == ("127.0.0.1", ports["status"]), peer
                        assert control["control"] == "force_rgb_keyframe", control
                        assert control["sender_id"] == "reply-test", control
                        assert control["reason"] == "record_start_all", control
                        print("PASS status control uses original UDP peer", peer, flush=True)
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
                    raise AssertionError("receiver shutdown hung")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--receiver", required=True)
    run(parser.parse_args().receiver)
