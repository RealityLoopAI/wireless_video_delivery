"""Deploy this receiver-only fix while idle, retaining an atomic rollback."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import urllib.request

COMMIT = "2d62473"
UNIT = "gwv3-gemini-receiver.service"

def status():
    with urllib.request.urlopen("http://127.0.0.1:18080/api/status", timeout=5) as r:
        return json.load(r)

s = status()
assert s["recording_state"] == "idle"
assert not any(c.get("recording") or c.get("record_tail_draining")
               or c.get("segment_finalize_pending") or c.get("segment_finalize_active") for c in s["cameras"])
expected = {c["camera_key"] for c in s["cameras"] if c.get("live")}
assert expected
target = Path("/home/loop/wireless_video_delivery-field-v2026.09.07.1/12_build/bin/gemini_receiver")
source = Path(f"/home/loop/wvd-receiver-{COMMIT}-build/bin/gemini_receiver")
backup = target.with_name(f"gemini_receiver.before-media-recovery-{COMMIT}")
assert not backup.exists()
shutil.copy2(target, backup)
candidate = target.with_name("gemini_receiver.recovery-new")
shutil.copy2(source, candidate)
try:
    subprocess.run(["systemctl", "--user", "stop", UNIT], check=True, timeout=60)
    os.replace(candidate, target)
    subprocess.run(["systemctl", "--user", "start", UNIT], check=True, timeout=20)
    good = 0
    for _ in range(40):
        time.sleep(2)
        try:
            current = status()
            live = {c["camera_key"] for c in current["cameras"]
                    if c.get("live") and 0 <= c.get("rgb_receive_age_ms", -1) < 5000}
            assert current["recording_staging"]["media_recovery_grace_ms"] == 120000
            good = good + 1 if expected <= live else 0
            if good >= 3:
                break
        except (OSError, ValueError):
            good = 0
    else:
        raise RuntimeError("previously live cameras did not recover")
except BaseException:
    shutil.copy2(backup, candidate)
    os.replace(candidate, target)
    subprocess.run(["systemctl", "--user", "restart", UNIT], check=True, timeout=60)
    raise
receipt = {"commit": COMMIT, "binary": str(target), "backup": str(backup),
           "sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
           "deployed_at_us": time.time_ns() // 1000, "verified_cameras": sorted(expected),
           "media_recovery_grace_ms": 120000}
Path("/home/loop/media-recovery-deployed.json").write_text(json.dumps(receipt, indent=2))
print(json.dumps(receipt), flush=True)
