"""Short live recording with session ownership and bounded NAS drain checks."""
import datetime
import json
from pathlib import Path
import signal
import time
import urllib.request

def api(path, post=False):
    req = urllib.request.Request("http://127.0.0.1:18080/api/" + path, method="POST" if post else "GET")
    with urllib.request.urlopen(req, timeout=10) as r:
        value = json.load(r)
    assert value.get("ok", True), value
    return value

def interrupted(signum, frame):
    raise SystemExit(f"interrupted by {signum}")

signal.signal(signal.SIGTERM, interrupted)
signal.signal(signal.SIGINT, interrupted)
out = Path.home() / ("wvd-media-recovery-live-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
out.mkdir()
def save(name, data):
    (out / name).write_text(json.dumps(data, indent=2))

before = api("status")
assert before["recording_state"] == "idle" and not any(c.get("recording") for c in before["cameras"])
assert before["recording_staging"]["media_recovery_grace_ms"] == 120000
save("before.json", before)
start = api("record/start-all", True)
save("start.json", start)
session = start["recording_session_id"]
print(json.dumps({"directory": str(out), "session": session}), flush=True)
try:
    for _ in range(6):
        time.sleep(10)
        current = api("status")
        assert current["recording_session_id"] == session and current["recording_all"]
        with (out / "samples.jsonl").open("a") as f:
            f.write(json.dumps(current) + "\n")
finally:
    current = api("status")
    if current.get("recording_session_id") == session and current.get("recording_all"):
        stopped = time.monotonic()
        save("stop.json", api("record/stop-all", True))
        for _ in range(60):
            time.sleep(3)
            current = api("status")
            save("after.json", current)
            uploader = current.get("recording_uploader", {})
            if (current["recording_state"] == "idle"
                and not current.get("record_finalize_outstanding_segments")
                and not any(c.get("record_tail_draining") or c.get("segment_finalize_pending")
                            or c.get("segment_finalize_active") for c in current["cameras"])
                and not uploader.get("pending_segments")
                and not uploader.get("local_pending_segments")
                and not uploader.get("nas_finalize_pending_segments")
                and not uploader.get("active_capture_workers")
                and not uploader.get("active_full_copy_workers")):
                result = {"drained_after_seconds": time.monotonic() - stopped, "directory": str(out)}
                save("result.json", result)
                print(json.dumps(result), flush=True)
                break
        else:
            raise RuntimeError("recording did not drain within 180 seconds")
