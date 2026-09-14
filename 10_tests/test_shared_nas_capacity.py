"""Receiver consumes a fresh local capacity snapshot, never a network stat."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import test_receiver_hardening as h


def run(receiver):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        nas = root / "nas"
        nas.mkdir()
        status = root / "capacity.json"
        ports = [h.free_port(socket.SOCK_STREAM) for _ in range(2)]
        config = dict(media_bind_ip="127.0.0.1", media_port=ports[0],
            admin_bind_ip="127.0.0.1", admin_port=ports[1],
            status_bind_ip="127.0.0.1", status_port=h.free_port(socket.SOCK_DGRAM),
            nas_root=str(nas), state_path=str(root / "state.json"),
            log_directory=str(root / "logs"), shared_nas_min_free_mb=1024,
            min_free_disk_mb=0, preview_enabled=False,
            receiver_discovery={"enabled": False}, clock_sync={"enabled": False},
            task_audio={"enabled": False}, recording_start_lead_ms=0,
            nas_auto_mount={"enabled": False, "status_path": str(status)},
            recording_staging={"enabled": True, "root": str(root / "staging")})
        path = root / "config.json"
        path.write_text(json.dumps(config))
        with (root / "output.log").open("w") as log:
            proc = subprocess.Popen([receiver, "--config", str(path)], stdout=log, stderr=log)
            try:
                h.wait_http(ports[1])
                def start_ok():
                    return json.loads(h.request(ports[1], "POST", "/api/record/start-all")[2]).get("ok")
                self_status = {"ready": True, "free_bytes": 0, "mount_point": str(nas),
                    "updated_us": time.time_ns() // 1000}
                status.write_text(json.dumps(self_status))
                assert start_ok() is False
                self_status["free_bytes"] = 10 * 2**30
                self_status["updated_us"] = 1
                status.write_text(json.dumps(self_status))
                assert start_ok() is False
                self_status["updated_us"] = time.time_ns() // 1000
                self_status["mount_point"] = "/wrong"
                status.write_text(json.dumps(self_status))
                assert start_ok() is False
                self_status["mount_point"] = str(nas)
                status.write_text(json.dumps(self_status))
                assert start_ok() is True
                h.request(ports[1], "POST", "/api/record/stop-all")
                status.unlink()
                assert start_ok() is False
                print("shared NAS low/stale/wrong-path/missing snapshots rejected; recovery accepted")
            finally:
                proc.terminate()
                proc.wait(timeout=15)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--receiver", required=True)
    run(p.parse_args().receiver)
