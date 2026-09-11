import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import urllib.request

p = argparse.ArgumentParser()
p.add_argument('--candidate', required=True)
p.add_argument('--unit', required=True)
p.add_argument('--native', choices=['on', 'off'], required=True)
p.add_argument('--receiver', default='192.168.1.196')
a = p.parse_args()
def run(*args, timeout=45):
    return subprocess.run(args, check=True, timeout=timeout)
def status():
    with urllib.request.urlopen('http://'+a.receiver+':8080/api/status', timeout=8) as r:
        return json.load(r)
def idle():
    s = status()
    if s.get('recording_state') != 'idle' or any(c.get('recording') or c.get('record_tail_draining') for c in s['cameras']):
        raise RuntimeError('receiver not idle; deployment refused')
def replace(src, dst):
    owner = dst.stat()
    temp = dst.with_name(dst.name+'.native-replace')
    shutil.copy2(src, temp)
    os.chown(temp, owner.st_uid, owner.st_gid)
    os.replace(temp, dst)
idle()
pids = subprocess.check_output(['pgrep', '-x', 'gemini_sender'], text=True).split()
assert len(pids) == 1, pids
proc = Path('/proc')/pids[0]
binary = (proc/'exe').resolve()
args = (proc/'cmdline').read_bytes().decode().split('\0')
config = Path(args[args.index('--config')+1])
c = json.loads(config.read_text())
sender = c['sender_id']
backup = Path('/var/lib/gwv3/native-rollout-8569b21')
backup.mkdir(parents=True, exist_ok=False)
backup.chmod(0o700)
shutil.copy2(binary, backup/'gemini_sender')
shutil.copy2(config, backup/'sender.json')
for cam in c['cameras']:
    cam['native_rgb_capture'] = a.native == 'on'
candidate_config = backup/'candidate.json'
candidate_config.write_text(json.dumps(c, indent=2)+'\n')
shutil.copystat(config, candidate_config)
env = os.environ.copy()
for entry in (proc/'environ').read_bytes().split(b'\0'):
    if b'=' in entry:
        k, v = entry.split(b'=', 1)
        if k.decode() in ('LD_LIBRARY_PATH', 'GST_PLUGIN_PATH_1_0', 'GST_PLUGIN_PATH'):
            env[k.decode()] = v.decode()
subprocess.run([a.candidate, '--config', str(candidate_config), '--validate-config'], check=True, env=env, timeout=20)
report = {'sender_id':sender, 'unit':a.unit, 'binary':str(binary), 'config':str(config), 'native_rgb_capture':a.native == 'on', 'backup':str(backup), 'old_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(), 'candidate_sha256':hashlib.sha256(Path(a.candidate).read_bytes()).hexdigest()}
try:
    idle()
    run('systemctl', 'stop', a.unit)
    replace(Path(a.candidate), binary)
    replace(candidate_config, config)
    run('systemctl', 'start', a.unit)
    deadline = time.monotonic()+100
    consecutive = 0
    while time.monotonic() < deadline:
        time.sleep(3)
        cams = [c for c in status()['cameras'] if c['sender_id'] == sender]
        good = len(cams) == len(c['cameras']) and all(x.get('live') and x.get('sender_build_commit') == '8569b2122b46' and x.get('sender_rgb_input_fps',0)>25 and x.get('sender_depth_input_fps',0)>25 and 0<=x.get('rgb_receive_age_ms',-1)<3000 and 0<=x.get('depth_receive_age_ms',-1)<3000 for x in cams)
        print(json.dumps({'sender':sender,'good':good,'fps':[x.get('sender_rgb_input_fps') for x in cams],'commits':[x.get('sender_build_commit') for x in cams]}), flush=True)
        consecutive = consecutive+1 if good else 0
        if consecutive >= 3:
            report['deployed'] = True
            break
    else:
        raise RuntimeError('candidate live acceptance timed out')
except BaseException as e:
    report.update(deployed=False, error=str(e))
    replace(backup/'gemini_sender', binary)
    replace(backup/'sender.json', config)
    run('systemctl','restart',a.unit)
    raise
finally:
    (backup/'result.json').write_text(json.dumps(report, indent=2)+'\n')
