import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import urllib.request

def status():
    with urllib.request.urlopen('http://127.0.0.1:18080/api/status', timeout=5) as r:
        return json.load(r)

s = status()
assert s['recording_state'] == 'idle'
assert not any(c.get('recording') or c.get('record_tail_draining') or c.get('segment_finalize_pending') or c.get('segment_finalize_active') for c in s['cameras'])
unit = 'gwv3-gemini-receiver.service'
target = Path('/home/loop/wireless_video_delivery-field-v2026.09.07.1/12_build/bin/gemini_receiver')
source = Path('/home/loop/wvd-receiver-15e3b4d-build/bin/gemini_receiver')
backup = target.with_name('gemini_receiver.before-depth-rotation-15e3b4d')
assert not backup.exists()
shutil.copy2(target, backup)
candidate = target.with_name('gemini_receiver.rotation-new')
shutil.copy2(source, candidate)
try:
    subprocess.run(['systemctl', '--user', 'stop', unit], check=True, timeout=60)
    os.replace(candidate, target)
    subprocess.run(['systemctl', '--user', 'start', unit], check=True, timeout=20)
    for attempt in range(30):
        time.sleep(2)
        try:
            current = status()
            if len([c for c in current['cameras'] if c.get('live')]) == 6:
                break
        except (OSError, ValueError):
            pass
    else:
        raise RuntimeError('six senders did not return after receiver restart')
except BaseException:
    shutil.copy2(backup, candidate)
    os.replace(candidate, target)
    subprocess.run(['systemctl', '--user', 'restart', unit], check=True, timeout=60)
    raise
receipt = {'commit': '15e3b4dad8738df4bed9be8415333822a45790ed',
           'binary': str(target), 'backup': str(backup),
           'sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
           'deployed_at_us': time.time_ns() // 1000, 'live_cameras': 6}
Path('/home/loop/depth-rotation-deployed.json').write_text(json.dumps(receipt, indent=2))
print(json.dumps(receipt))
